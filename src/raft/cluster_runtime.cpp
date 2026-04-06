#include "kvstore/raft/cluster_runtime.h"

#include <utility>

#include "kvstore/raft/test_transport.h"

namespace kvstore {
namespace raft {
namespace {

auto ToTestClusterOptions(EmbeddedClusterOptions options) -> TestCluster::Options {
  TestCluster::Options out;
  out.node_ids = std::move(options.node_ids);
  out.election_timeout_min_ticks = options.election_timeout_min_ticks;
  out.election_timeout_max_ticks = options.election_timeout_max_ticks;
  out.heartbeat_interval_ticks = options.heartbeat_interval_ticks;
  out.quorum_timeout_ticks = options.quorum_timeout_ticks;
  out.snapshot_threshold_entries = options.snapshot_threshold_entries;
  out.storage_root = std::move(options.storage_root);
  return out;
}

class EmbeddedRaftCluster final : public RaftCluster {
 public:
  explicit EmbeddedRaftCluster(EmbeddedClusterOptions options)
      : cluster_(ToTestClusterOptions(std::move(options))) {}

  auto Tick() -> void override { cluster_.Tick(); }

  auto SetNodeUp(NodeId id, bool up) -> void override { cluster_.SetNodeUp(id, up); }

  [[nodiscard]] auto IsNodeUp(NodeId id) const -> bool override {
    return cluster_.IsNodeUp(id);
  }

  [[nodiscard]] auto FindLeader() const -> std::optional<NodeId> override {
    return cluster_.FindLeader();
  }

  auto WaitForLeader(std::uint64_t max_ticks) -> std::optional<NodeId> override {
    return cluster_.WaitForLeader(max_ticks);
  }

  [[nodiscard]] auto node(NodeId id) -> RaftNode* override { return cluster_.node(id); }

  [[nodiscard]] auto node(NodeId id) const -> const RaftNode* override {
    return cluster_.node(id);
  }

 private:
  TestCluster cluster_;
};

}  // namespace

auto CreateEmbeddedRaftCluster(EmbeddedClusterOptions options)
    -> std::unique_ptr<RaftCluster> {
  return std::make_unique<EmbeddedRaftCluster>(std::move(options));
}

}  // namespace raft
}  // namespace kvstore
