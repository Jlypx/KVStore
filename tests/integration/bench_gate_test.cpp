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

auto MakeTempDirectory() -> std::filesystem::path {
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       Clock::now().time_since_epoch())
                       .count();
  const auto path = std::filesystem::temp_directory_path() /
                    ("kvstore_task7_bench_" + std::to_string(now));
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
  const auto result = svc->Put(key, value, request_id, Clock::now() + timeout);
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

auto P99Ms(const std::vector<double>& values) -> double {
  if (values.empty()) {
    return 0.0;
  }
  std::vector<double> sorted = values;
  std::sort(sorted.begin(), sorted.end());
  const std::size_t idx =
      std::min(sorted.size() - 1,
               (sorted.size() * static_cast<std::size_t>(99) + 99) / 100 - 1);
  return sorted[idx];
}

}  // namespace

int main() {
  kvstore::service::KvRaftService svc(MakeTempDirectory(),
                                      kvstore::service::RaftOptions{});

  const auto leader_before = WaitForLeader(&svc, std::chrono::seconds(2));
  if (!leader_before.has_value()) {
    std::cout << "{\"pass\":false,\"reason\":\"leader_not_elected\"}\n";
    return 1;
  }

  constexpr int kSamples = 300;
  std::vector<double> put_ms;
  std::vector<double> get_ms;
  put_ms.reserve(kSamples);
  get_ms.reserve(kSamples);

  for (int i = 0; i < kSamples; ++i) {
    const auto key = "bench-k-" + std::to_string(i);
    const auto val = "bench-v-" + std::to_string(i);
    const auto req = "bench-put-" + std::to_string(i);

    const auto start = Clock::now();
    if (!PutOk(&svc, key, val, req, std::chrono::seconds(2))) {
      std::cout << "{\"pass\":false,\"reason\":\"put_failed\",\"index\":" << i
                << "}\n";
      return 1;
    }
    const auto end = Clock::now();
    put_ms.push_back(
        static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                                end - start)
                                .count()) /
        1000.0);
  }

  for (int i = 0; i < kSamples; ++i) {
    const auto key = "bench-k-" + std::to_string(i);
    const auto start = Clock::now();
    const auto value = GetValue(&svc, key);
    const auto end = Clock::now();
    if (!value.has_value()) {
      std::cout << "{\"pass\":false,\"reason\":\"get_failed\",\"index\":" << i
                << "}\n";
      return 1;
    }
    get_ms.push_back(
        static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                                end - start)
                                .count()) /
        1000.0);
  }

  // no acknowledged write loss under a single-node crash:
  // 1) acknowledged write, 2) crash current leader, 3) verify value after re-election.
  const std::string crash_key = "bench-crash-key";
  const std::string crash_value = "bench-crash-value";
  if (!PutOk(&svc, crash_key, crash_value, "bench-crash-write")) {
    std::cout << "{\"pass\":false,\"reason\":\"crash_write_not_acked\"}\n";
    return 1;
  }

  svc.SetNodeUp(*leader_before, false);
  const auto leader_after =
      WaitForLeader(&svc, std::chrono::seconds(5), *leader_before);
  if (!leader_after.has_value()) {
    std::cout << "{\"pass\":false,\"reason\":\"leader_not_replaced_after_crash\"}\n";
    return 1;
  }

  const auto persisted = GetValue(&svc, crash_key);
  const bool no_ack_loss = persisted.has_value() && (*persisted == crash_value);
  if (!no_ack_loss) {
    std::cout << "{\"pass\":false,\"reason\":\"acknowledged_write_lost\"}\n";
    return 1;
  }

  const double put_p99_ms = P99Ms(put_ms);
  const double get_p99_ms = P99Ms(get_ms);

  std::cout << "{"
            << "\"pass\":true,"
            << "\"samples\":" << kSamples << ','
            << "\"p99_durable_write_ms\":" << put_p99_ms << ','
            << "\"p99_read_ms\":" << get_p99_ms << ','
            << "\"no_acknowledged_write_loss\":" << (no_ack_loss ? "true" : "false")
            << ','
            << "\"leader_before\":" << *leader_before << ','
            << "\"leader_after\":" << *leader_after << "}\n";
  return 0;
}
