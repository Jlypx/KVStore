#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include "kvstore/service/kv_raft_service.h"

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

auto WaitForLeader(kvstore::service::KvRaftService* svc,
                   std::chrono::milliseconds timeout)
    -> std::optional<kvstore::raft::NodeId> {
  const auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < timeout) {
    const auto leader = svc->FindLeader();
    if (leader.has_value()) {
      return leader;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return std::nullopt;
}

auto TestLeaderFailover() -> bool {
  const auto dir = MakeTempDirectory("integration_failover");
  kvstore::service::KvRaftService svc(dir, kvstore::service::RaftOptions{});

  const auto leader1 = WaitForLeader(&svc, std::chrono::seconds(2));
  if (!Expect(leader1.has_value(), "should elect leader")) {
    return false;
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  const auto put1 = svc.Put("k1", "v1", "failover-1", deadline);
  if (!Expect(std::holds_alternative<kvstore::service::PutResult>(put1),
              "Put before failover should succeed")) {
    return false;
  }

  // Drop leader and wait for re-election.
  svc.SetNodeUp(*leader1, false);
  const auto leader2 = WaitForLeader(&svc, std::chrono::seconds(3));
  if (!Expect(leader2.has_value(), "should elect new leader") ||
      !Expect(*leader2 != *leader1, "new leader should differ")) {
    return false;
  }

  const auto put2 = svc.Put("k1", "v2", "failover-2", deadline);
  if (!Expect(std::holds_alternative<kvstore::service::PutResult>(put2),
              "Put after failover should succeed")) {
    return false;
  }

  const auto get = svc.Get("k1");
  if (!Expect(std::holds_alternative<kvstore::service::GetResult>(get),
              "Get after failover should succeed")) {
    return false;
  }
  const auto got = std::get<kvstore::service::GetResult>(get);
  if (!Expect(got.found && got.value == "v2", "value should be v2")) {
    return false;
  }

  return true;
}

}  // namespace

int main() {
  if (!TestLeaderFailover()) {
    return 1;
  }
  return 0;
}
