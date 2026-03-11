#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "kvstore/api/grpc_kv_service.h"
#include "kvstore/service/kv_raft_service.h"

#include "kvstore/v1/kv.grpc.pb.h"

namespace {

auto MakeTempDirectory(const std::string& suffix) -> std::filesystem::path {
  const auto path = std::filesystem::temp_directory_path() /
                    ("kvstore_task6_" + suffix + "_test");
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
  std::filesystem::create_directories(path, ec);
  return path;
}

auto Expect(bool condition, const std::string& message) -> bool {
  if (!condition) {
    std::cerr << "ASSERTION FAILED: " << message << '\n';
    return false;
  }
  return true;
}

auto TestPutIdempotency() -> bool {
  const auto dir = MakeTempDirectory("grpc_idempotency");
  auto core = std::make_shared<kvstore::service::KvRaftService>(
      dir, kvstore::service::RaftOptions{});
  kvstore::api::GrpcKvService svc(core);

  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&svc);
  auto server = builder.BuildAndStart();
  if (!Expect(server != nullptr && port > 0, "server should start")) {
    return false;
  }

  auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                                     grpc::InsecureChannelCredentials());
  auto stub = kvstore::v1::KV::NewStub(channel);

  bool first_overwritten = false;
  {
    kvstore::v1::PutRequest req;
    req.set_key("k1");
    req.set_value("v1");
    req.set_request_id("idemp-1");
    kvstore::v1::PutResponse resp;
    grpc::ClientContext ctx;
    const auto st = stub->Put(&ctx, req, &resp);
    if (!Expect(st.ok(), "first Put should succeed")) {
      return false;
    }
    first_overwritten = resp.overwritten();
  }

  {
    kvstore::v1::PutRequest req;
    req.set_key("k1");
    req.set_value("v1");
    req.set_request_id("idemp-1");
    kvstore::v1::PutResponse resp;
    grpc::ClientContext ctx;
    const auto st = stub->Put(&ctx, req, &resp);
    if (!Expect(st.ok(), "duplicate Put should succeed") ||
        !Expect(resp.overwritten() == first_overwritten,
                "duplicate Put should return stable overwritten")) {
      return false;
    }
  }

  {
    kvstore::v1::PutRequest req;
    req.set_key("k1");
    req.set_value("DIFFERENT");
    req.set_request_id("idemp-1");
    kvstore::v1::PutResponse resp;
    grpc::ClientContext ctx;
    const auto st = stub->Put(&ctx, req, &resp);
    if (!Expect(!st.ok() && st.error_code() == grpc::StatusCode::INVALID_ARGUMENT,
                "conflicting retry should be INVALID_ARGUMENT")) {
      return false;
    }
  }

  server->Shutdown();
  return true;
}

auto TestDeleteIdempotency() -> bool {
  const auto dir = MakeTempDirectory("grpc_delete_idempotency");
  auto core = std::make_shared<kvstore::service::KvRaftService>(
      dir, kvstore::service::RaftOptions{});
  kvstore::api::GrpcKvService svc(core);

  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&svc);
  auto server = builder.BuildAndStart();
  if (!Expect(server != nullptr && port > 0, "server should start")) {
    return false;
  }

  auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                                     grpc::InsecureChannelCredentials());
  auto stub = kvstore::v1::KV::NewStub(channel);

  {
    kvstore::v1::PutRequest req;
    req.set_key("k1");
    req.set_value("v1");
    req.set_request_id("seed-put-1");
    kvstore::v1::PutResponse resp;
    grpc::ClientContext ctx;
    const auto st = stub->Put(&ctx, req, &resp);
    if (!Expect(st.ok(), "seed Put should succeed")) {
      return false;
    }
  }

  {
    kvstore::v1::DeleteRequest req;
    req.set_key("k1");
    req.set_request_id("delete-idemp-1");
    kvstore::v1::DeleteResponse resp;
    grpc::ClientContext ctx;
    const auto st = stub->Delete(&ctx, req, &resp);
    if (!Expect(st.ok(), "first Delete should succeed") ||
        !Expect(resp.deleted(), "first Delete should report deleted=true")) {
      return false;
    }
  }

  {
    kvstore::v1::DeleteRequest req;
    req.set_key("k1");
    req.set_request_id("delete-idemp-1");
    kvstore::v1::DeleteResponse resp;
    grpc::ClientContext ctx;
    const auto st = stub->Delete(&ctx, req, &resp);
    if (!Expect(st.ok(), "duplicate Delete should succeed") ||
        !Expect(resp.deleted(), "duplicate Delete should return stable deleted=true")) {
      return false;
    }
  }

  {
    kvstore::v1::DeleteRequest req;
    req.set_key("k1");
    kvstore::v1::DeleteResponse resp;
    grpc::ClientContext ctx;
    const auto st = stub->Delete(&ctx, req, &resp);
    if (!Expect(!st.ok() && st.error_code() == grpc::StatusCode::INVALID_ARGUMENT,
                "Delete without request_id should be INVALID_ARGUMENT")) {
      return false;
    }
  }

  server->Shutdown();
  return true;
}

}  // namespace

int main() {
  if (!TestPutIdempotency()) {
    return 1;
  }
  if (!TestDeleteIdempotency()) {
    return 1;
  }
  return 0;
}
