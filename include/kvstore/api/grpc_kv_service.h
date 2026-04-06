#ifndef KVSTORE_API_GRPC_KV_SERVICE_H
#define KVSTORE_API_GRPC_KV_SERVICE_H

#include <memory>

#include <grpcpp/grpcpp.h>

#include "kvstore/service/kv_raft_service.h"

#include "kvstore/v1/kv.grpc.pb.h"

namespace kvstore::api {

auto ToGrpcStatus(const kvstore::service::Error& e) -> grpc::Status;

class GrpcKvService final : public kvstore::v1::KV::Service {
 public:
  explicit GrpcKvService(std::shared_ptr<kvstore::service::KvService> svc)
      : svc_(std::move(svc)) {}

  auto Put(grpc::ServerContext* context,
           const kvstore::v1::PutRequest* request,
           kvstore::v1::PutResponse* response) -> grpc::Status override;

  auto Get(grpc::ServerContext* context,
           const kvstore::v1::GetRequest* request,
           kvstore::v1::GetResponse* response) -> grpc::Status override;

  auto Delete(grpc::ServerContext* context,
              const kvstore::v1::DeleteRequest* request,
              kvstore::v1::DeleteResponse* response) -> grpc::Status override;

 private:
  std::shared_ptr<kvstore::service::KvService> svc_;
};

}  // namespace kvstore::api

#endif  // KVSTORE_API_GRPC_KV_SERVICE_H
