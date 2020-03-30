/* Reference: https://github.com/grpc/grpc/blob/master/examples/cpp/helloworld/
*/
#include "threadpool.h"

#include <iostream>
#include <string>
#include <fstream>
#include "store.grpc.pb.h"

#include <grpcpp/grpcpp.h>
#include <grpc/support/log.h>

using grpc::Server;
using grpc::ServerAsyncResponseWriter;
using grpc::ServerBuilder;
using grpc::ServerCompletionQueue;
using grpc::ServerContext;
using grpc::Status;
using store::ProductInfo;
using store::ProductQuery;
using store::ProductReply;
using store::Store;

//Vendor related stuff
#include "vendor.grpc.pb.h"
using grpc::ClientAsyncResponseReader;
using grpc::ClientContext;
using grpc::CompletionQueue;
using grpc::Channel;
using vendor::BidQuery;
using vendor::BidReply;
using vendor::Vendor;

class VendorClient{
public:
  explicit VendorClient(std::shared_ptr<Channel> channel)
      : stub_(Vendor::NewStub(channel)) {}

  // Assembles the client's payload, sends it and presents the response back
  // from the server.
  bool get_product_bid(const std::string& product_name, BidReply &bid_reply) {
    // Data we are sending to the server.
    BidQuery request;
    request.set_product_name(product_name);

    
    // Context for the client. It could be used to convey extra information to
    // the server and/or tweak certain RPC behaviors.
    ClientContext context;

    // The producer-consumer queue we use to communicate asynchronously with the
    // gRPC runtime.
    CompletionQueue cq;

    // Storage for the status of the RPC upon completion.
    Status status;

    std::unique_ptr<ClientAsyncResponseReader<BidReply> > rpc(
        stub_->PrepareAsyncgetProductBid(&context, request, &cq));

    // StartCall initiates the RPC call
    rpc->StartCall();

    rpc->Finish(&bid_reply, &status, (void*)1);
    void* got_tag;
    bool ok = false;
    GPR_ASSERT(cq.Next(&got_tag, &ok));

    GPR_ASSERT(got_tag == (void*)1);
    GPR_ASSERT(ok);

    // Act upon the status of the actual RPC.
    if (status.ok()) {
      return true;
    } else {
      std::cout << status.error_message() << std::endl;
      return false;
    }
  }

 private:
  // Out of the passed in Channel comes the stub, stored here, our view of the
  // server's exposed services.
  std::unique_ptr<Vendor::Stub> stub_;
};


class StoreImpl
{
 public:
   StoreImpl(std::vector<std::string> vendors, std::string launch_addr) 
   	:vendors_(vendors), launch_addr_(launch_addr) {}
  ~StoreImpl() {
    server_->Shutdown();
    // Always shutdown the completion queue after the server.
    cq_->Shutdown();
  }

  // There is no shutdown handling in this code.
  void Run() {
    
    ServerBuilder builder;
    // Listen on the given address without any authentication mechanism.
    builder.AddListeningPort(launch_addr_, grpc::InsecureServerCredentials());
    // Register "service_" as the instance through which we'll communicate with
    // clients. In this case it corresponds to an *asynchronous* service.
    builder.RegisterService(&service_);
    // Get hold of the completion queue used for the asynchronous communication
    // with the gRPC runtime.
    cq_ = builder.AddCompletionQueue();
    // Finally assemble the server.
    server_ = builder.BuildAndStart();
    std::cout << "Server listening on " << launch_addr_ << std::endl;

    // Proceed to the server's main loop.
    HandleRpcs();
  }

 private:
  // Class encompasing the state and logic needed to serve a request.
  class CallData {
   public:
    // Take in the "service" instance (in this case representing an asynchronous
    // server) and the completion queue "cq" used for asynchronous communication
    // with the gRPC runtime.
    CallData(Store::AsyncService* service, ServerCompletionQueue* cq, std::vector<std::string> vendors)
        : service_(service), cq_(cq), responder_(&ctx_), status_(CREATE), vendors_(vendors) {
      // Invoke the serving logic right away.
      Proceed();
    }

    void Proceed() {
      if (status_ == CREATE) {
        // Make this instance progress to the PROCESS state.
        status_ = PROCESS;

        service_->RequestgetProducts(&ctx_, &request_, &responder_, cq_, cq_,
                                  this);
      } else if (status_ == PROCESS) {
        // Spawn a new CallData instance to serve new clients while we process
        // the one for this CallData. The instance will deallocate itself as
        // part of its FINISH state.
        new CallData(service_, cq_, vendors_);

        // The actual processing.
        // Call the vendor to get all the product info

		    for(auto vendor: vendors_){
            VendorClient vendor_client(grpc::CreateChannel(vendor, grpc::InsecureChannelCredentials()));
            BidReply bid_reply;
            bool res = vendor_client.get_product_bid(request_.product_name(), bid_reply);
            if(res){
              ProductInfo *info = reply_.add_products();
              info->set_price(bid_reply.price());
              info->set_vendor_id(bid_reply.vendor_id());
            }
        }

        // And we are done! Let the gRPC runtime know we've finished, using the
        // memory address of this instance as the uniquely identifying tag for
        // the event.
        status_ = FINISH;
        responder_.Finish(reply_, Status::OK, this);
      } else {
        GPR_ASSERT(status_ == FINISH);
        // Once in the FINISH state, deallocate ourselves (CallData).
        delete this;
      }
    }

   private:
    // The means of communication with the gRPC runtime for an asynchronous
    // server.
    Store::AsyncService* service_;
    // The producer-consumer queue where for asynchronous server notifications.
    ServerCompletionQueue* cq_;
    // Context for the rpc, allowing to tweak aspects of it such as the use
    // of compression, authentication, as well as to send metadata back to the
    // client.
    ServerContext ctx_;

    // What we get from the client.
    ProductQuery request_;
    // What we send back to the client.
    ProductReply reply_;

    // The means to get back to the client.
    ServerAsyncResponseWriter<ProductReply> responder_;

    // Let's implement a tiny state machine with the following states.
    enum CallStatus { CREATE, PROCESS, FINISH };
    CallStatus status_;  // The current serving state.
    std::vector<std::string> vendors_;
  };

  // This can be run in multiple threads if needed.
  void HandleRpcs() {
    // Spawn a new CallData instance to serve new clients.
    new CallData(&service_, cq_.get(), vendors_);
    void* tag;  // uniquely identifies a request.
    bool ok;
    while (true) {
      // Block waiting to read the next event from the completion queue. The
      // event is uniquely identified by its tag, which in this case is the
      // memory address of a CallData instance.
      // The return value of Next should always be checked. This return value
      // tells us whether there is any kind of event or cq_ is shutting down.
      GPR_ASSERT(cq_->Next(&tag, &ok));
      GPR_ASSERT(ok);
      static_cast<CallData*>(tag)->Proceed();
    }
  }

  std::unique_ptr<ServerCompletionQueue> cq_;
  Store::AsyncService service_;
  std::unique_ptr<Server> server_;
  std::vector<std::string> vendors_;
  std::string launch_addr_;

};

int main(int argc, char** argv) {
  std::string launch_addr;
  int num_threads;
  if (argc == 3)
  {
    launch_addr = std::string(argv[1]);
    num_threads = atoi(argv[2]);
  }
	// Read the vendor ports
	std::vector<std::string> ip_addrresses;
	std::string filename = "vendor_addresses.txt";
	std::ifstream myfile(filename);
	if(myfile.is_open()) {
    	std::string ip_addr;
    	while(getline(myfile, ip_addr)) {
      			ip_addrresses.push_back(ip_addr);
		}
		myfile.close();
    }
  	else{
    	std::cerr << "Failed to open file " << filename << std::endl;
    	return EXIT_FAILURE;
  	}
	StoreImpl store(ip_addrresses, launch_addr);
  store.Run();
	return 0;
}
