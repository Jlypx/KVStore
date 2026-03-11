#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "kvstore/service/kv_raft_service.h"

namespace {

using Clock = std::chrono::steady_clock;

auto MakeTempDirectory(const std::string& suffix) -> std::filesystem::path {
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       Clock::now().time_since_epoch())
                       .count();
  const auto path = std::filesystem::temp_directory_path() /
                    ("kvstore_task7_" + suffix + "_" + std::to_string(now));
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
  std::filesystem::create_directories(path, ec);
  return path;
}

auto WaitForLeader(kvstore::service::KvRaftService* svc,
                   std::chrono::milliseconds timeout,
                   std::optional<kvstore::raft::NodeId> exclude = std::nullopt)
    -> std::optional<kvstore::raft::NodeId> {
  const auto start = Clock::now();
  while (Clock::now() - start < timeout) {
    const auto leader = svc->FindLeader();
    if (leader.has_value() && (!exclude.has_value() || *leader != *exclude)) {
      return leader;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return std::nullopt;
}

auto PutOk(kvstore::service::KvRaftService* svc,
           const std::string& key,
           const std::string& value,
           const std::string& request_id,
           std::chrono::milliseconds timeout = std::chrono::seconds(2)) -> bool {
  const auto deadline = Clock::now() + timeout;
  const auto result = svc->Put(key, value, request_id, deadline);
  return std::holds_alternative<kvstore::service::PutResult>(result);
}

auto GetValue(kvstore::service::KvRaftService* svc, const std::string& key)
    -> std::optional<std::string> {
  const auto result = svc->Get(key);
  if (!std::holds_alternative<kvstore::service::GetResult>(result)) {
    return std::nullopt;
  }
  const auto got = std::get<kvstore::service::GetResult>(result);
  if (!got.found) {
    return std::nullopt;
  }
  return got.value;
}

auto PrintJson(const std::string& json) -> int {
  std::cout << json << '\n';
  return 0;
}

auto PrintFail(const std::string& mode, const std::string& reason) -> int {
  std::cout << "{\"mode\":\"" << mode << "\",\"pass\":false,\"reason\":\""
            << reason << "\"}" << '\n';
  return 1;
}

auto RunFailover() -> int {
  kvstore::service::KvRaftService svc(MakeTempDirectory("chaos_failover"),
                                      kvstore::service::RaftOptions{});

  const auto leader_before = WaitForLeader(&svc, std::chrono::seconds(2));
  if (!leader_before.has_value()) {
    return PrintFail("leader_failover", "leader_not_elected");
  }
  if (!PutOk(&svc, "failover-k", "v1", "failover-seed-1")) {
    return PrintFail("leader_failover", "seed_put_failed");
  }

  svc.SetNodeUp(*leader_before, false);
  const auto failover_start = Clock::now();
  const auto leader_after =
      WaitForLeader(&svc, std::chrono::seconds(5), *leader_before);
  if (!leader_after.has_value()) {
    return PrintFail("leader_failover", "new_leader_timeout");
  }
  const auto failover_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                            failover_start)
          .count();
  if (failover_ms > 5000) {
    return PrintFail("leader_failover", "failover_exceeds_5000ms");
  }

  if (!PutOk(&svc, "failover-k", "v2", "failover-post-2")) {
    return PrintFail("leader_failover", "post_failover_put_failed");
  }
  const auto value = GetValue(&svc, "failover-k");
  if (!value.has_value() || *value != "v2") {
    return PrintFail("leader_failover", "post_failover_get_mismatch");
  }

  return PrintJson("{\"mode\":\"leader_failover\",\"pass\":true,\"leader_before\":" +
                   std::to_string(*leader_before) +
                   ",\"leader_after\":" + std::to_string(*leader_after) +
                   ",\"failover_ms\":" + std::to_string(failover_ms) +
                   ",\"max_failover_ms\":5000}");
}

auto RunRestartRto() -> int {
  const auto data_dir = MakeTempDirectory("chaos_restart_rto");
  const auto deadline_timeout = std::chrono::seconds(2);

  {
    kvstore::service::KvRaftService svc(data_dir, kvstore::service::RaftOptions{});
    if (!WaitForLeader(&svc, std::chrono::seconds(2)).has_value()) {
      return PrintFail("restart_rto", "leader_not_elected_before_restart");
    }
    for (int i = 0; i < 200; ++i) {
      const auto key = "restart-k-" + std::to_string(i);
      const auto val = "restart-v-" + std::to_string(i);
      const auto req = "restart-req-" + std::to_string(i);
      if (!PutOk(&svc, key, val, req, deadline_timeout)) {
        return PrintFail("restart_rto", "seed_put_failed");
      }
    }
  }

  const auto restart_begin = Clock::now();
  kvstore::service::KvRaftService restarted(data_dir, kvstore::service::RaftOptions{});
  if (!WaitForLeader(&restarted, std::chrono::seconds(60)).has_value()) {
    return PrintFail("restart_rto", "leader_not_elected_after_restart");
  }
  const auto sample = GetValue(&restarted, "restart-k-42");
  if (!sample.has_value() || *sample != "restart-v-42") {
    return PrintFail("restart_rto", "recovered_value_mismatch");
  }
  const auto rto_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                            restart_begin)
          .count();
  if (rto_ms > 60000) {
    return PrintFail("restart_rto", "restart_rto_exceeds_60000ms");
  }

  return PrintJson("{\"mode\":\"restart_rto\",\"pass\":true,\"restart_rto_ms\":" +
                   std::to_string(rto_ms) + ",\"max_rto_ms\":60000}");
}

auto RunPartitionHeal() -> int {
  kvstore::service::KvRaftService svc(MakeTempDirectory("chaos_partition"),
                                      kvstore::service::RaftOptions{});
  const auto leader = WaitForLeader(&svc, std::chrono::seconds(2));
  if (!leader.has_value()) {
    return PrintFail("partition_heal", "leader_not_elected");
  }

  if (!PutOk(&svc, "partition-k", "before", "partition-before-1")) {
    return PrintFail("partition_heal", "seed_put_failed");
  }

  const std::vector<kvstore::raft::NodeId> node_ids = {1, 2, 3, 4, 5};
  std::size_t down_count = 0;
  for (const auto id : node_ids) {
    if (id == *leader || down_count >= 3) {
      continue;
    }
    svc.SetNodeUp(id, false);
    ++down_count;
  }

  bool unavailable_ok = false;
  const auto reject_deadline = Clock::now() + std::chrono::seconds(2);
  int attempt = 0;
  while (Clock::now() < reject_deadline && !unavailable_ok) {
    const auto request_id = "partition-during-" + std::to_string(attempt + 2);
    const auto unavailable =
        svc.Put("partition-k", "during", request_id,
                Clock::now() + std::chrono::milliseconds(200));
    if (std::holds_alternative<kvstore::service::Error>(unavailable)) {
      unavailable_ok =
          std::get<kvstore::service::Error>(unavailable).code ==
          kvstore::service::ErrorCode::kUnavailable;
    }
    ++attempt;
    if (!unavailable_ok) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  if (!unavailable_ok) {
    return PrintFail("partition_heal", "write_during_partition_not_rejected");
  }

  for (const auto id : node_ids) {
    svc.SetNodeUp(id, true);
  }

  const auto healed_leader = WaitForLeader(&svc, std::chrono::seconds(5));
  if (!healed_leader.has_value()) {
    return PrintFail("partition_heal", "leader_not_recovered_after_heal");
  }

  bool recovered_ok = false;
  const auto recover_deadline = Clock::now() + std::chrono::seconds(2);
  int recover_attempt = 0;
  while (Clock::now() < recover_deadline && !recovered_ok) {
    const auto request_id = "partition-after-" + std::to_string(recover_attempt + 3);
    const auto recovered =
        svc.Put("partition-k", "after", request_id,
                Clock::now() + std::chrono::milliseconds(200));
    recovered_ok = std::holds_alternative<kvstore::service::PutResult>(recovered);
    ++recover_attempt;
    if (!recovered_ok) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  if (!recovered_ok) {
    return PrintFail("partition_heal", "write_after_heal_failed");
  }
  const auto value = GetValue(&svc, "partition-k");
  if (!value.has_value() || *value != "after") {
    return PrintFail("partition_heal", "value_after_heal_mismatch");
  }

  return PrintJson("{\"mode\":\"partition_heal\",\"pass\":true,\"leader_before\":" +
                   std::to_string(*leader) +
                   ",\"leader_after\":" + std::to_string(*healed_leader) +
                   ",\"partition_write_rejected\":true}");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    return PrintFail("unknown", "usage: chaos_gate_test <failover|restart_rto|partition_heal>");
  }

  const std::string mode = argv[1];
  if (mode == "failover") {
    return RunFailover();
  }
  if (mode == "restart_rto") {
    return RunRestartRto();
  }
  if (mode == "partition_heal") {
    return RunPartitionHeal();
  }
  return PrintFail(mode, "unsupported_mode");
}
