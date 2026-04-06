#ifndef KVSTORE_RAFT_CLUSTER_RUNTIME_H
#define KVSTORE_RAFT_CLUSTER_RUNTIME_H

#include <filesystem>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "kvstore/raft/raft_types.h"

namespace kvstore {
namespace raft {

class RaftNode;

struct EmbeddedClusterOptions {
  std::vector<NodeId> node_ids;
  std::uint64_t election_timeout_min_ticks = 10;
  std::uint64_t election_timeout_max_ticks = 20;
  std::uint64_t heartbeat_interval_ticks = 2;
  std::uint64_t quorum_timeout_ticks = 0;
  std::uint64_t snapshot_threshold_entries = 32;
  std::filesystem::path storage_root;
};

class RaftCluster {
 public:
  virtual ~RaftCluster() = default;

  virtual auto Tick() -> void = 0;
  virtual auto SetNodeUp(NodeId id, bool up) -> void = 0;
  [[nodiscard]] virtual auto IsNodeUp(NodeId id) const -> bool = 0;
  [[nodiscard]] virtual auto FindLeader() const -> std::optional<NodeId> = 0;
  virtual auto WaitForLeader(std::uint64_t max_ticks) -> std::optional<NodeId> = 0;
  [[nodiscard]] virtual auto node(NodeId id) -> RaftNode* = 0;
  [[nodiscard]] virtual auto node(NodeId id) const -> const RaftNode* = 0;
};

auto CreateEmbeddedRaftCluster(EmbeddedClusterOptions options)
    -> std::unique_ptr<RaftCluster>;

}
}

#endif
