#include "kvstore/raft/test_transport.h"

#include <cstddef>
#include <cstdint>

namespace kvstore::raft {
namespace {

auto DefaultNodeIds() -> std::vector<NodeId> {
  return {1, 2, 3, 4, 5};
}

}  // namespace

auto TestTransport::RegisterNode(NodeId id, RaftNode* node) -> void {
  nodes_[id] = node;
  if (up_.find(id) == up_.end()) {
    up_[id] = true;
  }
}

auto TestTransport::SetNodeUp(NodeId id, bool up) -> void { up_[id] = up; }

auto TestTransport::IsNodeUp(NodeId id) const -> bool {
  const auto it = up_.find(id);
  if (it == up_.end()) {
    return false;
  }
  return it->second;
}

auto TestTransport::Send(Message message) -> void {
  if (!IsNodeUp(message.from) || !IsNodeUp(message.to)) {
    return;
  }

  queue_.push_back(Envelope{.seq = next_seq_, .message = std::move(message)});
  next_seq_ += 1;
}

auto TestTransport::DeliverSome(std::size_t limit) -> std::size_t {
  std::size_t delivered = 0;
  while (!queue_.empty() && delivered < limit) {
    Envelope envelope = std::move(queue_.front());
    queue_.pop_front();

    if (!IsNodeUp(envelope.message.to)) {
      continue;
    }
    const auto it = nodes_.find(envelope.message.to);
    if (it == nodes_.end() || it->second == nullptr) {
      continue;
    }

    it->second->Step(envelope.message);
    delivered += 1;
  }
  return delivered;
}

auto TestTransport::DeliverAll() -> std::size_t {
  // Hard cap to avoid infinite loops if a bug produces message storms.
  constexpr std::size_t kMax = 1'000'000;
  return DeliverSome(kMax);
}

TestCluster::TestCluster()
    : TestCluster(Options{.node_ids = DefaultNodeIds(),
                          .storage_root = std::filesystem::path{}}) {}

TestCluster::TestCluster(Options options) : options_(std::move(options)) {
  if (options_.node_ids.empty()) {
    options_.node_ids = DefaultNodeIds();
  }

  nodes_.reserve(options_.node_ids.size());
  for (NodeId id : options_.node_ids) {
    RaftNodeConfig config;
    config.node_id = id;
    config.static_node_ids = options_.node_ids;
    config.election_timeout_ticks_min = options_.election_timeout_min_ticks;
    config.election_timeout_ticks_max = options_.election_timeout_max_ticks;
    config.heartbeat_interval_ticks = options_.heartbeat_interval_ticks;
    config.quorum_timeout_ticks = options_.quorum_timeout_ticks;
    config.random_seed = static_cast<std::uint32_t>(id * 97U + 3U);
    if (!options_.storage_root.empty()) {
      config.storage_dir =
          options_.storage_root / ("node" + std::to_string(id)) / "raft";
    }

    auto sender = [this](Message message) { transport_.Send(std::move(message)); };
    nodes_.push_back(std::make_unique<RaftNode>(std::move(config), sender));
    transport_.RegisterNode(id, nodes_.back().get());
    transport_.SetNodeUp(id, true);
  }
}

auto TestCluster::Tick() -> void {
  for (const auto& node : nodes_) {
    if (!node) {
      continue;
    }
    if (!IsNodeUp(node->node_id())) {
      continue;
    }
    node->Tick();
  }

  transport_.DeliverAll();
}

auto TestCluster::RunTicks(std::uint64_t ticks) -> void {
  for (std::uint64_t i = 0; i < ticks; ++i) {
    Tick();
  }
}

auto TestCluster::node(NodeId id) -> RaftNode* {
  for (const auto& node : nodes_) {
    if (node && node->node_id() == id) {
      return node.get();
    }
  }
  return nullptr;
}

auto TestCluster::node(NodeId id) const -> const RaftNode* {
  for (const auto& node : nodes_) {
    if (node && node->node_id() == id) {
      return node.get();
    }
  }
  return nullptr;
}

auto TestCluster::FindLeader() const -> std::optional<NodeId> {
  std::optional<NodeId> leader;
  for (const auto& node : nodes_) {
    if (!node) {
      continue;
    }
    if (!IsNodeUp(node->node_id())) {
      continue;
    }
    if (node->role() != Role::kLeader) {
      continue;
    }
    if (leader.has_value()) {
      return std::nullopt;
    }
    leader = node->node_id();
  }
  return leader;
}

auto TestCluster::WaitForLeader(std::uint64_t max_ticks)
    -> std::optional<NodeId> {
  for (std::uint64_t i = 0; i < max_ticks; ++i) {
    Tick();
    const auto leader = FindLeader();
    if (leader.has_value()) {
      return leader;
    }
  }
  return std::nullopt;
}

}  // namespace kvstore::raft
