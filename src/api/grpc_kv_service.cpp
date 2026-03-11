#include "kvstore/api/grpc_kv_service.h"

#include <chrono>
#include <string>

namespace kvstore::api {

auto ToGrpcStatus(const kvstore::service::Error& e) -> grpc::Status {
  using kvstore::service::ErrorCode;
  switch (e.code) {
    case ErrorCode::kInvalidArgument:
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, e.message);
    case ErrorCode::kNotLeader:
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          e.message + " (leader_hint=" +
                              std::to_string(e.leader_hint) + ")");
    case ErrorCode::kUnavailable:
      return grpc::Status(grpc::StatusCode::UNAVAILABLE, e.message);
    case ErrorCode::kTimeout:
      return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, e.message);
    case ErrorCode::kInternal:
    default:
      return grpc::Status(grpc::StatusCode::INTERNAL, e.message);
  }
}

namespace {

auto DeadlineFromContext(grpc::ServerContext* context)
    -> std::chrono::steady_clock::time_point {
  // gRPC deadline uses system_clock internally; approximate to steady_clock.
  const auto deadline = context->deadline();
  if (deadline == std::chrono::system_clock::time_point::max()) {
    return std::chrono::steady_clock::now() + std::chrono::seconds(10);
  }

  const auto now_sys = std::chrono::system_clock::now();
  const auto now_steady = std::chrono::steady_clock::now();
  const auto remaining = deadline - now_sys;
  if (remaining <= std::chrono::system_clock::duration::zero()) {
    return std::chrono::steady_clock::now();
  }
  return now_steady +
         std::chrono::duration_cast<std::chrono::steady_clock::duration>(remaining);
}

}  // namespace

auto GrpcKvService::Put(grpc::ServerContext* context,
                        const kvstore::v1::PutRequest* request,
                        kvstore::v1::PutResponse* response) -> grpc::Status {
  const auto deadline = DeadlineFromContext(context);
  const auto res = svc_->Put(request->key(), request->value(), request->request_id(),
                             deadline);
  if (std::holds_alternative<kvstore::service::Error>(res)) {
    return ToGrpcStatus(std::get<kvstore::service::Error>(res));
  }
  const auto ok = std::get<kvstore::service::PutResult>(res);
  response->set_overwritten(ok.overwritten);
  return grpc::Status::OK;
}

auto GrpcKvService::Get(grpc::ServerContext* /*context*/,
                        const kvstore::v1::GetRequest* request,
                        kvstore::v1::GetResponse* response) -> grpc::Status {
  const auto res = svc_->Get(request->key());
  if (std::holds_alternative<kvstore::service::Error>(res)) {
    return ToGrpcStatus(std::get<kvstore::service::Error>(res));
  }
  const auto ok = std::get<kvstore::service::GetResult>(res);
  response->set_found(ok.found);
  if (ok.found) {
    response->set_value(ok.value);
  }
  return grpc::Status::OK;
}

auto GrpcKvService::Delete(grpc::ServerContext* context,
                           const kvstore::v1::DeleteRequest* request,
                           kvstore::v1::DeleteResponse* response) -> grpc::Status {
  const auto deadline = DeadlineFromContext(context);
  const auto res = svc_->Delete(request->key(), request->request_id(), deadline);
  if (std::holds_alternative<kvstore::service::Error>(res)) {
    return ToGrpcStatus(std::get<kvstore::service::Error>(res));
  }
  const auto ok = std::get<kvstore::service::DeleteResult>(res);
  response->set_deleted(ok.deleted);
  return grpc::Status::OK;
}

}  // namespace kvstore::api
