/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <forward_list>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include <grpc++/generic/async_generic_service.h>
#include <grpc++/security/server_credentials.h>
#include <grpc++/server.h>
#include <grpc++/server_builder.h>
#include <grpc++/server_context.h>
#include <grpc++/support/config.h>
#include <grpc/grpc.h>
#include <grpc/support/alloc.h>
#include <grpc/support/host_port.h>
#include <grpc/support/log.h>

#include "src/proto/grpc/testing/services.grpc.pb.h"
#include "test/core/util/test_config.h"
#include "test/cpp/qps/server.h"

namespace grpc {
namespace testing {

template <class RequestType, class ResponseType, class ServiceType,
          class ServerContextType>
class AsyncQpsServerTest : public Server {
 public:
  AsyncQpsServerTest(
      const ServerConfig &config,
      std::function<void(ServerBuilder *, ServiceType *)> register_service,
      std::function<void(ServiceType *, ServerContextType *, RequestType *,
                         ServerAsyncResponseWriter<ResponseType> *,
                         CompletionQueue *, ServerCompletionQueue *, void *)>
          request_unary_function,
      std::function<void(ServiceType *, ServerContextType *,
                         ServerAsyncReaderWriter<ResponseType, RequestType> *,
                         CompletionQueue *, ServerCompletionQueue *, void *)>
          request_streaming_function,
      std::function<grpc::Status(const PayloadConfig &, const RequestType *,
                                 ResponseType *)>
          process_rpc)
      : Server(config) {
    char *server_address = NULL;

    gpr_join_host_port(&server_address, "::", port());

    ServerBuilder builder;
    builder.AddListeningPort(server_address,
                             Server::CreateServerCredentials(config));
    gpr_free(server_address);

    register_service(&builder, &async_service_);

    int num_threads = config.async_server_threads();
    if (num_threads <= 0) {  // dynamic sizing
      num_threads = cores();
      gpr_log(GPR_INFO, "Sizing async server to %d threads", num_threads);
    }

    for (int i = 0; i < num_threads; i++) {
      srv_cqs_.emplace_back(builder.AddCompletionQueue());
    }

    server_ = builder.BuildAndStart();

    using namespace std::placeholders;

    auto process_rpc_bound =
        std::bind(process_rpc, config.payload_config(), _1, _2);

    for (int i = 0; i < 10000 / num_threads; i++) {
      for (int j = 0; j < num_threads; j++) {
        if (request_unary_function) {
          auto request_unary =
              std::bind(request_unary_function, &async_service_, _1, _2, _3,
                        srv_cqs_[j].get(), srv_cqs_[j].get(), _4);
          contexts_.push_front(
              new ServerRpcContextUnaryImpl(request_unary, process_rpc_bound));
        }
        if (request_streaming_function) {
          auto request_streaming =
              std::bind(request_streaming_function, &async_service_, _1, _2,
                        srv_cqs_[j].get(), srv_cqs_[j].get(), _3);
          contexts_.push_front(new ServerRpcContextStreamingImpl(
              request_streaming, process_rpc_bound));
        }
      }
    }

    for (int i = 0; i < num_threads; i++) {
      shutdown_state_.emplace_back(new PerThreadShutdownState());
    }
    for (int i = 0; i < num_threads; i++) {
      threads_.emplace_back(&AsyncQpsServerTest::ThreadFunc, this, i);
    }
  }
  ~AsyncQpsServerTest() {
    for (auto ss = shutdown_state_.begin(); ss != shutdown_state_.end(); ++ss) {
      (*ss)->set_shutdown();
    }
    server_->Shutdown();
    for (auto thr = threads_.begin(); thr != threads_.end(); thr++) {
      thr->join();
    }
    for (auto cq = srv_cqs_.begin(); cq != srv_cqs_.end(); ++cq) {
      (*cq)->Shutdown();
      bool ok;
      void *got_tag;
      while ((*cq)->Next(&got_tag, &ok))
        ;
    }
    while (!contexts_.empty()) {
      delete contexts_.front();
      contexts_.pop_front();
    }
  }

 private:
  void ThreadFunc(int rank) {
    // Wait until work is available or we are shutting down
    bool ok;
    void *got_tag;
    while (srv_cqs_[rank]->Next(&got_tag, &ok)) {
      ServerRpcContext *ctx = detag(got_tag);
      // The tag is a pointer to an RPC context to invoke
      const bool still_going = ctx->RunNextState(ok);
      if (!shutdown_state_[rank]->shutdown()) {
        // this RPC context is done, so refresh it
        if (!still_going) {
          ctx->Reset();
        }
      } else {
        return;
      }
    }
    return;
  }

  class ServerRpcContext {
   public:
    ServerRpcContext() {}
    virtual ~ServerRpcContext(){};
    virtual bool RunNextState(bool) = 0;  // next state, return false if done
    virtual void Reset() = 0;             // start this back at a clean state
  };
  static void *tag(ServerRpcContext *func) {
    return reinterpret_cast<void *>(func);
  }
  static ServerRpcContext *detag(void *tag) {
    return reinterpret_cast<ServerRpcContext *>(tag);
  }

  class ServerRpcContextUnaryImpl GRPC_FINAL : public ServerRpcContext {
   public:
    ServerRpcContextUnaryImpl(
        std::function<void(ServerContextType *, RequestType *,
                           grpc::ServerAsyncResponseWriter<ResponseType> *,
                           void *)>
            request_method,
        std::function<grpc::Status(const RequestType *, ResponseType *)>
            invoke_method)
        : srv_ctx_(new ServerContextType),
          next_state_(&ServerRpcContextUnaryImpl::invoker),
          request_method_(request_method),
          invoke_method_(invoke_method),
          response_writer_(srv_ctx_.get()) {
      request_method_(srv_ctx_.get(), &req_, &response_writer_,
                      AsyncQpsServerTest::tag(this));
    }
    ~ServerRpcContextUnaryImpl() GRPC_OVERRIDE {}
    bool RunNextState(bool ok) GRPC_OVERRIDE {
      return (this->*next_state_)(ok);
    }
    void Reset() GRPC_OVERRIDE {
      srv_ctx_.reset(new ServerContextType);
      req_ = RequestType();
      response_writer_ =
          grpc::ServerAsyncResponseWriter<ResponseType>(srv_ctx_.get());

      // Then request the method
      next_state_ = &ServerRpcContextUnaryImpl::invoker;
      request_method_(srv_ctx_.get(), &req_, &response_writer_,
                      AsyncQpsServerTest::tag(this));
    }

   private:
    bool finisher(bool) { return false; }
    bool invoker(bool ok) {
      if (!ok) {
        return false;
      }

      ResponseType response;

      // Call the RPC processing function
      grpc::Status status = invoke_method_(&req_, &response);

      // Have the response writer work and invoke on_finish when done
      next_state_ = &ServerRpcContextUnaryImpl::finisher;
      response_writer_.Finish(response, status, AsyncQpsServerTest::tag(this));
      return true;
    }
    std::unique_ptr<ServerContextType> srv_ctx_;
    RequestType req_;
    bool (ServerRpcContextUnaryImpl::*next_state_)(bool);
    std::function<void(ServerContextType *, RequestType *,
                       grpc::ServerAsyncResponseWriter<ResponseType> *, void *)>
        request_method_;
    std::function<grpc::Status(const RequestType *, ResponseType *)>
        invoke_method_;
    grpc::ServerAsyncResponseWriter<ResponseType> response_writer_;
  };

  class ServerRpcContextStreamingImpl GRPC_FINAL : public ServerRpcContext {
   public:
    ServerRpcContextStreamingImpl(
        std::function<void(
            ServerContextType *,
            grpc::ServerAsyncReaderWriter<ResponseType, RequestType> *, void *)>
            request_method,
        std::function<grpc::Status(const RequestType *, ResponseType *)>
            invoke_method)
        : srv_ctx_(new ServerContextType),
          next_state_(&ServerRpcContextStreamingImpl::request_done),
          request_method_(request_method),
          invoke_method_(invoke_method),
          stream_(srv_ctx_.get()) {
      request_method_(srv_ctx_.get(), &stream_, AsyncQpsServerTest::tag(this));
    }
    ~ServerRpcContextStreamingImpl() GRPC_OVERRIDE {}
    bool RunNextState(bool ok) GRPC_OVERRIDE {
      return (this->*next_state_)(ok);
    }
    void Reset() GRPC_OVERRIDE {
      srv_ctx_.reset(new ServerContextType);
      req_ = RequestType();
      stream_ = grpc::ServerAsyncReaderWriter<ResponseType, RequestType>(
          srv_ctx_.get());

      // Then request the method
      next_state_ = &ServerRpcContextStreamingImpl::request_done;
      request_method_(srv_ctx_.get(), &stream_, AsyncQpsServerTest::tag(this));
    }

   private:
    bool request_done(bool ok) {
      if (!ok) {
        return false;
      }
      stream_.Read(&req_, AsyncQpsServerTest::tag(this));
      next_state_ = &ServerRpcContextStreamingImpl::read_done;
      return true;
    }

    bool read_done(bool ok) {
      if (ok) {
        // invoke the method
        ResponseType response;
        // Call the RPC processing function
        grpc::Status status = invoke_method_(&req_, &response);
        // initiate the write
        stream_.Write(response, AsyncQpsServerTest::tag(this));
        next_state_ = &ServerRpcContextStreamingImpl::write_done;
      } else {  // client has sent writes done
        // finish the stream
        stream_.Finish(Status::OK, AsyncQpsServerTest::tag(this));
        next_state_ = &ServerRpcContextStreamingImpl::finish_done;
      }
      return true;
    }
    bool write_done(bool ok) {
      // now go back and get another streaming read!
      if (ok) {
        stream_.Read(&req_, AsyncQpsServerTest::tag(this));
        next_state_ = &ServerRpcContextStreamingImpl::read_done;
      } else {
        stream_.Finish(Status::OK, AsyncQpsServerTest::tag(this));
        next_state_ = &ServerRpcContextStreamingImpl::finish_done;
      }
      return true;
    }
    bool finish_done(bool ok) { return false; /* reset the context */ }

    std::unique_ptr<ServerContextType> srv_ctx_;
    RequestType req_;
    bool (ServerRpcContextStreamingImpl::*next_state_)(bool);
    std::function<void(
        ServerContextType *,
        grpc::ServerAsyncReaderWriter<ResponseType, RequestType> *, void *)>
        request_method_;
    std::function<grpc::Status(const RequestType *, ResponseType *)>
        invoke_method_;
    grpc::ServerAsyncReaderWriter<ResponseType, RequestType> stream_;
  };

  std::vector<std::thread> threads_;
  std::unique_ptr<grpc::Server> server_;
  std::vector<std::unique_ptr<grpc::ServerCompletionQueue>> srv_cqs_;
  ServiceType async_service_;
  std::forward_list<ServerRpcContext *> contexts_;

  class PerThreadShutdownState {
   public:
    PerThreadShutdownState() : shutdown_(false) {}

    bool shutdown() const {
      std::lock_guard<std::mutex> lock(mutex_);
      return shutdown_;
    }

    void set_shutdown() {
      std::lock_guard<std::mutex> lock(mutex_);
      shutdown_ = true;
    }

   private:
    mutable std::mutex mutex_;
    bool shutdown_;
  };
  std::vector<std::unique_ptr<PerThreadShutdownState>> shutdown_state_;
};

static void RegisterBenchmarkService(ServerBuilder *builder,
                                     BenchmarkService::AsyncService *service) {
  builder->RegisterService(service);
}
static void RegisterGenericService(ServerBuilder *builder,
                                   grpc::AsyncGenericService *service) {
  builder->RegisterAsyncGenericService(service);
}

static Status ProcessSimpleRPC(const PayloadConfig &,
                               const SimpleRequest *request,
                               SimpleResponse *response) {
  if (request->response_size() > 0) {
    if (!Server::SetPayload(request->response_type(), request->response_size(),
                            response->mutable_payload())) {
      return Status(grpc::StatusCode::INTERNAL, "Error creating payload.");
    }
  }
  return Status::OK;
}

static Status ProcessGenericRPC(const PayloadConfig &payload_config,
                                const ByteBuffer *request,
                                ByteBuffer *response) {
  int resp_size = payload_config.bytebuf_params().resp_size();
  std::unique_ptr<char[]> buf(new char[resp_size]);
  gpr_slice s = gpr_slice_from_copied_buffer(buf.get(), resp_size);
  Slice slice(s, Slice::STEAL_REF);
  *response = ByteBuffer(&slice, 1);
  return Status::OK;
}

std::unique_ptr<Server> CreateAsyncServer(const ServerConfig &config) {
  return std::unique_ptr<Server>(
      new AsyncQpsServerTest<SimpleRequest, SimpleResponse,
                             BenchmarkService::AsyncService,
                             grpc::ServerContext>(
          config, RegisterBenchmarkService,
          &BenchmarkService::AsyncService::RequestUnaryCall,
          &BenchmarkService::AsyncService::RequestStreamingCall,
          ProcessSimpleRPC));
}
std::unique_ptr<Server> CreateAsyncGenericServer(const ServerConfig &config) {
  return std::unique_ptr<Server>(
      new AsyncQpsServerTest<ByteBuffer, ByteBuffer, grpc::AsyncGenericService,
                             grpc::GenericServerContext>(
          config, RegisterGenericService, nullptr,
          &grpc::AsyncGenericService::RequestCall, ProcessGenericRPC));
}

}  // namespace testing
}  // namespace grpc
