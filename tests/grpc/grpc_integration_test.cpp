#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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

struct ServerHandle {
  std::shared_ptr<kvstore::service::KvRaftService> core;
  std::unique_ptr<grpc::Server> server;
  std::unique_ptr<kvstore::api::GrpcKvService> service;
  int port = 0;
};

auto StartServer(const std::filesystem::path& data_dir) -> ServerHandle {
  ServerHandle h;
  h.core = std::make_shared<kvstore::service::KvRaftService>(
      data_dir, kvstore::service::RaftOptions{});
  h.service = std::make_unique<kvstore::api::GrpcKvService>(h.core);

  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &h.port);
  builder.RegisterService(h.service.get());
  h.server = builder.BuildAndStart();
  return h;
}

auto TestPutGetDelete() -> bool {
  const auto dir = MakeTempDirectory("grpc_integration");
  auto h = StartServer(dir);
  if (!Expect(h.server != nullptr && h.port > 0, "server should start")) {
    return false;
  }

  auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(h.port),
                                     grpc::InsecureChannelCredentials());
  auto stub = kvstore::v1::KV::NewStub(channel);

  {
    kvstore::v1::PutRequest req;
    req.set_key("k1");
    req.set_value("v1");
    req.set_request_id("req-1");
    kvstore::v1::PutResponse resp;
    grpc::ClientContext ctx;
    const auto st = stub->Put(&ctx, req, &resp);
    if (!Expect(st.ok(), "Put should succeed") ||
        !Expect(!resp.overwritten(), "first put should not be overwritten")) {
      return false;
    }
  }

  {
    kvstore::v1::GetRequest req;
    req.set_key("k1");
    kvstore::v1::GetResponse resp;
    grpc::ClientContext ctx;
    const auto st = stub->Get(&ctx, req, &resp);
    if (!Expect(st.ok(), "Get should succeed") ||
        !Expect(resp.found(), "Get should find key") ||
        !Expect(resp.value() == "v1", "Get should return v1")) {
      return false;
    }
  }

  {
    kvstore::v1::PutRequest req;
    req.set_key("k1");
    req.set_value("v2");
    req.set_request_id("req-2");
    kvstore::v1::PutResponse resp;
    grpc::ClientContext ctx;
    const auto st = stub->Put(&ctx, req, &resp);
    if (!Expect(st.ok(), "second Put should succeed") ||
        !Expect(resp.overwritten(), "second put should be overwritten")) {
      return false;
    }
  }

  {
    kvstore::v1::DeleteRequest req;
    req.set_key("k1");
    req.set_request_id("del-req-1");
    kvstore::v1::DeleteResponse resp;
    grpc::ClientContext ctx;
    const auto st = stub->Delete(&ctx, req, &resp);
    if (!Expect(st.ok(), "Delete should succeed") ||
        !Expect(resp.deleted(), "Delete should report deleted=true")) {
      return false;
    }
  }

  {
    kvstore::v1::GetRequest req;
    req.set_key("k1");
    kvstore::v1::GetResponse resp;
    grpc::ClientContext ctx;
    const auto st = stub->Get(&ctx, req, &resp);
    if (!Expect(st.ok(), "Get after delete should succeed") ||
        !Expect(!resp.found(), "Get after delete should not find key")) {
      return false;
    }
  }

  h.server->Shutdown();
  return true;
}

auto TestQuorumUnavailable() -> bool {
  const auto dir = MakeTempDirectory("grpc_quorum_unavailable");
  auto h = StartServer(dir);
  if (!Expect(h.server != nullptr && h.port > 0, "server should start")) {
    return false;
  }

  auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(h.port),
                                     grpc::InsecureChannelCredentials());
  auto stub = kvstore::v1::KV::NewStub(channel);

  // Seed one value while the cluster is healthy.
  {
    kvstore::v1::PutRequest req;
    req.set_key("q1");
    req.set_value("v1");
    req.set_request_id("quorum-seed-1");
    kvstore::v1::PutResponse resp;
    grpc::ClientContext ctx;
    const auto st = stub->Put(&ctx, req, &resp);
    if (!Expect(st.ok(), "seed Put should succeed")) {
      return false;
    }
  }

  const auto leader = h.core->FindLeader();
  if (!Expect(leader.has_value(), "leader should exist before partition")) {
    return false;
  }

  // Bring down all followers so leader loses quorum contact.
  const std::vector<kvstore::raft::NodeId> all_nodes = {1, 2, 3, 4, 5};
  for (const auto id : all_nodes) {
    if (id != *leader) {
      h.core->SetNodeUp(id, false);
    }
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  {
    kvstore::v1::PutRequest req;
    req.set_key("q1");
    req.set_value("v2");
    req.set_request_id("quorum-put-2");
    kvstore::v1::PutResponse resp;
    grpc::ClientContext ctx;
    const auto st = stub->Put(&ctx, req, &resp);
    if (!Expect(!st.ok() && st.error_code() == grpc::StatusCode::UNAVAILABLE,
                "Put should surface quorum-unavailable as UNAVAILABLE")) {
      return false;
    }
  }

  {
    kvstore::v1::GetRequest req;
    req.set_key("q1");
    kvstore::v1::GetResponse resp;
    grpc::ClientContext ctx;
    const auto st = stub->Get(&ctx, req, &resp);
    if (!Expect(!st.ok() && st.error_code() == grpc::StatusCode::UNAVAILABLE,
                "linearizable Get should reject without quorum")) {
      return false;
    }
  }

  h.server->Shutdown();
  return true;
}

}  // namespace

int main() {
  if (!TestPutGetDelete()) {
    return 1;
  }
  if (!TestQuorumUnavailable()) {
    return 1;
  }
  return 0;
}
