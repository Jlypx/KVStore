#ifndef KVSTORE_SERVICE_KV_RAFT_SERVICE_H
#define KVSTORE_SERVICE_KV_RAFT_SERVICE_H

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "kvstore/raft/raft_types.h"

namespace kvstore::service {

struct PutResult {
  bool overwritten = false;
};

struct GetResult {
  bool found = false;
  std::string value;
};

struct DeleteResult {
  bool deleted = false;
};

enum class ErrorCode {
  kInvalidArgument,
  kNotLeader,
  kUnavailable,
  kTimeout,
  kInternal,
};

struct Error {
  ErrorCode code = ErrorCode::kInternal;
  std::string message;
  kvstore::raft::NodeId leader_hint = kvstore::raft::kNoLeader;
};

template <typename T>
using Result = std::variant<T, Error>;

struct RaftOptions {
  std::vector<kvstore::raft::NodeId> node_ids = {1, 2, 3, 4, 5};
  std::uint64_t election_timeout_min_ticks = 5;
  std::uint64_t election_timeout_max_ticks = 10;
  std::uint64_t heartbeat_interval_ticks = 1;
  std::uint64_t quorum_timeout_ticks = 5;
  std::uint64_t snapshot_threshold_entries = 32;
  std::chrono::milliseconds tick_period{1};
};

// In-process Raft+KV orchestration suitable for v1 tests.
//
// - Writes propose commands on the current leader.
// - Majority commit triggers RaftNode::on_committed callbacks.
// - Committed commands are applied to a per-node KvEngine.
// - Put/Delete are retry-safe via request_id at the service boundary.
class KvService {
 public:
  virtual ~KvService() = default;

  virtual auto Put(const std::string& key,
                   const std::string& value,
                   const std::string& request_id,
                   std::chrono::steady_clock::time_point deadline)
      -> Result<PutResult> = 0;

  virtual auto Get(const std::string& key) -> Result<GetResult> = 0;

  virtual auto Delete(const std::string& key,
                      const std::string& request_id,
                      std::chrono::steady_clock::time_point deadline)
      -> Result<DeleteResult> = 0;
};

class KvRaftService : public KvService {
 public:
  KvRaftService(std::filesystem::path data_dir, RaftOptions options);
  ~KvRaftService();

  KvRaftService(const KvRaftService&) = delete;
  KvRaftService& operator=(const KvRaftService&) = delete;

  auto Put(const std::string& key,
           const std::string& value,
           const std::string& request_id,
           std::chrono::steady_clock::time_point deadline)
      -> Result<PutResult> override;

  auto Get(const std::string& key) -> Result<GetResult> override;

  auto Delete(const std::string& key,
              const std::string& request_id,
              std::chrono::steady_clock::time_point deadline)
      -> Result<DeleteResult> override;

  // Test hooks.
  auto SetNodeUp(kvstore::raft::NodeId id, bool up) -> void;
  [[nodiscard]] auto FindLeader() const -> std::optional<kvstore::raft::NodeId>;
  auto ForceLeaderSnapshotForTesting() -> bool;

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace kvstore::service

#endif  // KVSTORE_SERVICE_KV_RAFT_SERVICE_H
