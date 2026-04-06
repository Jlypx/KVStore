#ifndef KVSTORE_SERVICE_CLUSTER_NODE_SERVICE_H
#define KVSTORE_SERVICE_CLUSTER_NODE_SERVICE_H

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

#include "kvstore/engine/kv_engine.h"
#include "kvstore/raft/network_transport.h"
#include "kvstore/raft/raft_node.h"
#include "kvstore/runtime/cluster_config.h"
#include "kvstore/service/kv_raft_service.h"

namespace kvstore::service {

class ClusterNodeService final : public KvService {
 public:
  explicit ClusterNodeService(kvstore::runtime::ClusterProcessConfig config);
  ~ClusterNodeService();

  ClusterNodeService(const ClusterNodeService&) = delete;
  auto operator=(const ClusterNodeService&) -> ClusterNodeService& = delete;

  auto Start() -> bool;
  auto Stop() -> void;

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

  auto HandlePeerRequestVote(kvstore::raft::NodeId from,
                             const kvstore::raft::RequestVoteRequest& request)
      -> kvstore::raft::RequestVoteResponse;
  auto HandlePeerAppendEntries(kvstore::raft::NodeId from,
                               const kvstore::raft::AppendEntriesRequest& request)
      -> kvstore::raft::AppendEntriesResponse;
  auto HandlePeerInstallSnapshot(
      kvstore::raft::NodeId from,
      const kvstore::raft::InstallSnapshotRequest& request)
      -> kvstore::raft::InstallSnapshotResponse;

 [[nodiscard]] auto FindLeader() const -> std::optional<kvstore::raft::NodeId>;
  [[nodiscard]] auto self_id() const -> kvstore::raft::NodeId { return config_.self_id; }

 private:
  struct InflightPut {
    std::shared_ptr<std::promise<PutResult>> promise;
    std::shared_future<PutResult> future;
    std::uint32_t key_crc = 0;
    std::uint32_t value_crc = 0;
    std::size_t key_size = 0;
    std::size_t value_size = 0;
  };

  struct InflightDelete {
    std::shared_ptr<std::promise<DeleteResult>> promise;
    std::shared_future<DeleteResult> future;
  };

  struct PutCacheEntry {
    PutResult result;
    std::uint32_t key_crc = 0;
    std::uint32_t value_crc = 0;
    std::size_t key_size = 0;
    std::size_t value_size = 0;
  };

  auto TickLoop() -> void;
  auto OnCommitted(std::vector<kvstore::raft::CommittedEntry> entries) -> void;

  auto EnsureStarted() const -> bool;

  kvstore::runtime::ClusterProcessConfig config_;

  mutable std::recursive_mutex mu_;
  std::unique_ptr<kvstore::engine::KvEngine> engine_;
  std::unique_ptr<kvstore::raft::NetworkTransport> transport_;
  std::unique_ptr<kvstore::raft::RaftNode> node_;

  std::unordered_map<std::string, PutCacheEntry> completed_puts_;
  std::unordered_map<std::string, DeleteResult> completed_deletes_;
  std::unordered_map<std::string, InflightPut> inflight_puts_;
  std::unordered_map<std::string, InflightDelete> inflight_deletes_;

  std::atomic<bool> stop_{false};
  bool started_ = false;
  std::optional<kvstore::raft::SnapshotMetadata> pending_snapshot_;
  std::thread ticker_;
};

}  // namespace kvstore::service

#endif  // KVSTORE_SERVICE_CLUSTER_NODE_SERVICE_H
