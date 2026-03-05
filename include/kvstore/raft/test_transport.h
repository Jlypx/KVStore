#ifndef KVSTORE_RAFT_TEST_TRANSPORT_H
#define KVSTORE_RAFT_TEST_TRANSPORT_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "kvstore/raft/raft_node.h"
#include "kvstore/raft/raft_types.h"

namespace kvstore::raft {

class TestTransport {
 public:
  auto RegisterNode(NodeId id, RaftNode* node) -> void;

  auto SetNodeUp(NodeId id, bool up) -> void;
  [[nodiscard]] auto IsNodeUp(NodeId id) const -> bool;

  // RaftNode SendFn target.
  auto Send(Message message) -> void;

  auto DeliverSome(std::size_t limit) -> std::size_t;
  auto DeliverAll() -> std::size_t;

  [[nodiscard]] auto pending_message_count() const -> std::size_t {
    return queue_.size();
  }

 private:
  struct Envelope {
    std::uint64_t seq = 0;
    Message message;
  };

  std::uint64_t next_seq_ = 1;
  std::deque<Envelope> queue_;
  std::unordered_map<NodeId, RaftNode*> nodes_;
  std::unordered_map<NodeId, bool> up_;
};

class TestCluster {
 public:
  struct Options {
    std::vector<NodeId> node_ids;
    std::uint64_t election_timeout_min_ticks = 10;
    std::uint64_t election_timeout_max_ticks = 20;
    std::uint64_t heartbeat_interval_ticks = 2;
    std::uint64_t quorum_timeout_ticks = 0;
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
