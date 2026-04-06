#ifndef KVSTORE_RAFT_TEST_TRANSPORT_H
#define KVSTORE_RAFT_TEST_TRANSPORT_H

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include "kvstore/raft/inprocess_transport.h"
#include "kvstore/raft/raft_node.h"

namespace kvstore::raft {

class TestTransport : public InProcessTransport {
 public:
  auto RegisterNode(NodeId id, RaftNode* node) -> void;
};

class TestCluster {
 public:
  struct Options {
    std::vector<NodeId> node_ids;
    std::uint64_t election_timeout_min_ticks = 10;
    std::uint64_t election_timeout_max_ticks = 20;
    std::uint64_t heartbeat_interval_ticks = 2;
    std::uint64_t quorum_timeout_ticks = 0;
    std::filesystem::path storage_root;
  };

  explicit TestCluster(Options options);
  TestCluster();

  auto Tick() -> void;
  auto RunTicks(std::uint64_t ticks) -> void;

  auto SetNodeUp(NodeId id, bool up) -> void { transport_.SetNodeUp(id, up); }
  [[nodiscard]] auto IsNodeUp(NodeId id) const -> bool {
    return transport_.IsNodeUp(id);
  }

  [[nodiscard]] auto FindLeader() const -> std::optional<NodeId>;
  auto WaitForLeader(std::uint64_t max_ticks) -> std::optional<NodeId>;

  [[nodiscard]] auto node(NodeId id) -> RaftNode*;
  [[nodiscard]] auto node(NodeId id) const -> const RaftNode*;

  [[nodiscard]] auto transport() -> TestTransport& { return transport_; }
  [[nodiscard]] auto transport() const -> const TestTransport& {
    return transport_;
  }

 private:
  Options options_;
  TestTransport transport_;
  std::vector<std::unique_ptr<RaftNode>> nodes_;
};

}  // namespace kvstore::raft

#endif  // KVSTORE_RAFT_TEST_TRANSPORT_H
