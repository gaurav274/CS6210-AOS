#pragma once

#include <mr_task_factory.h>
#include "mr_tasks.h"
#include <grpcpp/grpcpp.h>
#include "masterworker.grpc.pb.h"
#include <thread>

using grpc::Server;
using grpc::ServerAsyncResponseWriter;
using grpc::ServerBuilder;
using grpc::ServerCompletionQueue;
using grpc::ServerContext;
using grpc::Status;

using masterworker::File;
using masterworker::ShardPartition;
using masterworker::MapQuery;
using masterworker::MapResult;
using masterworker::MasterWorker;
using masterworker::ReduceQuery;
using masterworker::ReduceResult;
using masterworker::HeartbeatQuery;
using masterworker::HeartbeatResult;

extern std::shared_ptr<BaseMapper> get_mapper_from_task_factory(const std::string &user_id);
extern std::shared_ptr<BaseReducer> get_reducer_from_task_factory(const std::string& user_id);

int hash(const std::string &input, int partitions)
{
	int sum = 0;
	for (auto c : input)
	{
		sum += int(c);
	}
	return sum % partitions;
}

class CallData{
public:
	virtual void Proceed() = 0;
};

class MapperCallData final : public CallData{
public:
		
		MapperCallData(MasterWorker::AsyncService* service, ServerCompletionQueue* cq, std::string ip_addr)
			: service_(service), cq_(cq), responder_(&ctx_), status_(CREATE), worker_ip_addr_(ip_addr)
		{
			// Invoke the serving logic right away.
			Proceed();
		}

		void Proceed()
		{
			if (status_ == CREATE) {
				// Make this instance progress to the PROCESS state.
				status_ = PROCESS;

				service_->Requestmapper(&ctx_, &request_, &responder_, cq_, cq_, this);

			} else if (status_ == PROCESS) {
				
				new MapperCallData(service_, cq_, worker_ip_addr_);


				// Process mapper job
				reply_ = handle_mapper_job(request_);
				
				status_ = FINISH;
				responder_.Finish(reply_, Status::OK, this);
				// std::cout << "result: " << reply_.files(0).filename() << std::endl;

			} else {
				GPR_ASSERT(status_ == FINISH);
				delete this;
			}
		}


	private:
		MapResult handle_mapper_job(const MapQuery&);
		BaseMapperInternal* get_base_mapper_internal(BaseMapper*);

		std::string worker_ip_addr_;
		MasterWorker::AsyncService *service_;
		ServerCompletionQueue* cq_;
		ServerContext ctx_;
		MapQuery request_;
		MapResult reply_;
		ServerAsyncResponseWriter<MapResult> responder_;
		enum CallStatus { CREATE, PROCESS, FINISH };
		CallStatus status_;
};


class ReducerCallData final : public CallData{
public:
		
		ReducerCallData(MasterWorker::AsyncService* service, ServerCompletionQueue* cq, std::string ip_addr)
			: service_(service), cq_(cq), responder_(&ctx_), status_(CREATE), worker_ip_addr_(ip_addr)
		{
			// Invoke the serving logic right away.
			Proceed();
		}

		void Proceed()
		{
			if (status_ == CREATE) {
				// Make this instance progress to the PROCESS state.
				status_ = PROCESS;

				service_->Requestreducer(&ctx_, &request_, &responder_, cq_, cq_, this);

			} else if (status_ == PROCESS) {
				
				new ReducerCallData(service_, cq_, worker_ip_addr_);

				// Process reducer job
				reply_ = handle_reducer_job(request_);

				status_ = FINISH;
				responder_.Finish(reply_, Status::OK, this);

			} else {
				GPR_ASSERT(status_ == FINISH);
				delete this;
			}
		}


	private:
		ReduceResult handle_reducer_job(const ReduceQuery&);
		BaseReducerInternal* get_base_reducer_internal(BaseReducer*);

		std::string worker_ip_addr_;
		MasterWorker::AsyncService* service_;
		ServerCompletionQueue* cq_;
		ServerContext ctx_;
		ReduceQuery request_;
		ReduceResult reply_;
		ServerAsyncResponseWriter<ReduceResult> responder_;
		enum CallStatus { CREATE, PROCESS, FINISH };
		CallStatus status_; 
};

class HeartbeatCallData final : public CallData{
public:
		HeartbeatCallData(MasterWorker::AsyncService* service, ServerCompletionQueue* cq, std::string ip_addr)
			: service_(service), cq_(cq), responder_(&ctx_), status_(CREATE), worker_ip_addr_(ip_addr)
		{
			// Invoke the serving logic right away.
			Proceed();
		}

		void Proceed()
		{
			if (status_ == CREATE) {
				// Make this instance progress to the PROCESS state.
				status_ = PROCESS;

				service_->Requestheartbeat(&ctx_, &request_, &responder_, cq_, cq_, this);

			} else if (status_ == PROCESS) {

				new HeartbeatCallData(service_, cq_, worker_ip_addr_);

				// Process heartbeat message
				reply_.set_id(worker_ip_addr_);

				status_ = FINISH;
				responder_.Finish(reply_, Status::OK, this);

			} else {
				GPR_ASSERT(status_ == FINISH);
				delete this;
			}
		}


	private:
		HeartbeatResult handle_heartbeat_job(const HeartbeatQuery&);

		std::string worker_ip_addr_;
		MasterWorker::AsyncService* service_;
		ServerCompletionQueue* cq_;
		ServerContext ctx_;
		HeartbeatQuery request_;
		HeartbeatResult reply_;
		ServerAsyncResponseWriter<HeartbeatResult> responder_;
		enum CallStatus { CREATE, PROCESS, FINISH };
		CallStatus status_;
};


/* CS6210_TASK: Handle all the task a Worker is supposed to do.
	This is a big task for this project, will test your understanding of map reduce */
class Worker {

	public:
		/* DON'T change the function signature of this constructor */
		Worker(std::string ip_addr_port);

		/* DON'T change this function's signature */
		bool run();
		~Worker(){
			server_->Shutdown();
  			cq_->Shutdown();
		}
		static BaseMapperInternal* get_base_mapper_internal(BaseMapper* mapper){
			return mapper->impl_;
		}
		static BaseReducerInternal* get_base_reducer_internal(BaseReducer* mapper){
			return mapper->impl_;
		}

	private:
		/* NOW you can add below, data members and member functions as per the need of your implementation*/
		bool handle_rpcs();
		void heartbeat();

		std::string ip_addr_port_;
		std::unique_ptr<ServerCompletionQueue> cq_;
		std::unique_ptr<ServerCompletionQueue> cq2_;
  		MasterWorker::AsyncService service_;
  		std::unique_ptr<Server> server_;
};


/* CS6210_TASK: ip_addr_port is the only information you get when started.
	You can populate your other class data members here if you want */
Worker::Worker(std::string ip_addr_port) : ip_addr_port_(ip_addr_port){ 
}


/* CS6210_TASK: Here you go. once this function is called your woker's job is to keep looking for new tasks 
	from Master, complete when given one and again keep looking for the next one.
	Note that you have the access to BaseMapper's member BaseMapperInternal impl_ and 
	BaseReduer's member BaseReducerInternal impl_ directly, 
	so you can manipulate them however you want when running map/reduce tasks*/
bool Worker::run() {
	
	ServerBuilder builder;

	builder.AddListeningPort(ip_addr_port_, grpc::InsecureServerCredentials());
	builder.RegisterService(&service_);
	
	cq_ = builder.AddCompletionQueue();
	cq2_ = builder.AddCompletionQueue();
	server_ = builder.BuildAndStart();

	std::thread heartbeat_job(&Worker::heartbeat, this);

	handle_rpcs();
	
	return true;
}

void Worker::heartbeat() {

	void* tag;  // uniquely identifies a request.
	bool ok;

	new HeartbeatCallData(&service_, cq2_.get(), ip_addr_port_);

	while(true) {
		GPR_ASSERT(cq2_->Next(&tag, &ok));
		GPR_ASSERT(ok);
		static_cast<CallData*>(tag)->Proceed();
	}
}

bool Worker::handle_rpcs(){
	std::string ip_addr_num_ = ip_addr_port_.substr(ip_addr_port_.find(":") + 1);
	std::cout << ip_addr_port_ << " " << ip_addr_num_ << "\n";
	new MapperCallData(&service_, cq_.get(), ip_addr_num_);
	new ReducerCallData(&service_, cq_.get(), ip_addr_num_);

	void* tag;  // uniquely identifies a request.
	bool ok;
	while (true) {
		GPR_ASSERT(cq_->Next(&tag, &ok));
		//GPR_ASSERT(ok);
		if (!ok) std::cout << "recv error: " << ok << " for worker " << ip_addr_port_ << "\n";
		static_cast<CallData*>(tag)->Proceed();
	}
}


BaseMapperInternal* MapperCallData::get_base_mapper_internal(BaseMapper* mapper){
	Worker::get_base_mapper_internal(mapper);
}

MapResult MapperCallData::handle_mapper_job(const MapQuery& request){
	MapResult result;
	// handle mapper job
	auto user_mapper = get_mapper_from_task_factory(request.user_id());
	
	/* mapper implementation */ 
	// 1. Setup the base internal mapper
	auto base_mapper = get_base_mapper_internal(user_mapper.get());
	base_mapper->n_partitions = request.n_partitions();
	// The intermediate files for each partition
	std::vector<std::string> interm_files;
	// Name for intermediate files
	for (int i = 0; i < request.n_partitions(); i++)
		interm_files.push_back("intermediate/" + std::to_string(i) + "_" + worker_ip_addr_  + ".txt");
	base_mapper->interm_files = interm_files;

	// 2. Call mapper on each shard
	for (int i = 0; i < request.partitions_size(); i++)
	{
		ShardPartition partition = request.partitions(i);
		
		//read the shard
		std::ifstream f(partition.filename());
		f.seekg(partition.start());
		int bytes_to_read = partition.end() - partition.start();
		std::string result(bytes_to_read, ' ');
		f.read(&result[0], bytes_to_read);
		
		// Call map on each line
		std::stringstream ss(result); 
		std::string line;
		while (getline(ss, line)){			
			user_mapper->map(line);
		}
		f.close();
	}

	// 3.flush the key value pairs to intermediate files
	base_mapper->flush();
	// 4. Prepare result
	for(int i = 0; i < request.n_partitions(); i++){
		File* file = result.add_files();
		file->set_filename(interm_files[i]);
	}

	return result;
}


BaseReducerInternal* ReducerCallData::get_base_reducer_internal(BaseReducer* mapper){
	Worker::get_base_reducer_internal(mapper);
}


ReduceResult ReducerCallData::handle_reducer_job(const ReduceQuery& query){
	ReduceResult result;
	// handle reducer job
	auto user_reducer = get_reducer_from_task_factory(query.user_id());

	/*Reducer implementation*/
	// 1. Setup the internal reducer
	auto base_reducer = get_base_reducer_internal(user_reducer.get());
	std::string output_file = query.output_dir() + "/" + std::to_string(query.partition_id());
	std::vector<std::string> files;
	base_reducer->filename = output_file;
	
	// 2. Call user reducer on this file
	std::map<std::string, std::vector<std::string> > kvs;
	for (int i = 0; i < query.files_size(); i++){
		std::ifstream f(query.files(i).filename());
		if(f){
			std::string line;
			while(getline(f, line)){
				int pos = line.find_first_of(',');
				std::string key = line.substr(0, pos);
				std::string value = line.substr(pos + 2);
				if (kvs.find(key) == kvs.end())
					kvs[key] = std::vector<std::string>();
				kvs[key].push_back(value);
			}
		}	
	}
	std::cout << kvs.size() << std::endl;
	for(auto const& kv:kvs){
		
		user_reducer->reduce(kv.first, kv.second);
	}
	// 3. write to a output file
	File* file = result.mutable_file();
	file->set_filename(output_file);
	return result;
}
