#include "kvstore/raft/test_transport.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

auto Expect(bool condition, const std::string& message) -> bool {
  if (!condition) {
    std::cerr << "ASSERTION FAILED: " << message << '\n';
    return false;
  }
  return true;
}

auto WaitForCommit(kvstore::raft::TestCluster* cluster,
                   const std::vector<kvstore::raft::NodeId>& node_ids,
                   kvstore::raft::LogIndex index,
                   std::uint64_t max_ticks) -> bool {
  for (std::uint64_t i = 0; i < max_ticks; ++i) {
    cluster->Tick();
    bool all_committed = true;
    for (kvstore::raft::NodeId id : node_ids) {
      const auto* node = cluster->node(id);
      if (node == nullptr || !cluster->IsNodeUp(id)) {
        continue;
      }
      if (node->committed_index() < index) {
        all_committed = false;
      }
    }
    if (all_committed) {
      return true;
    }
  }
  return false;
}

auto TestReplicationAndMajorityCommit() -> bool {
  kvstore::raft::TestCluster::Options options;
  options.node_ids = {1, 2, 3, 4, 5};
  options.election_timeout_min_ticks = 5;
  options.election_timeout_max_ticks = 10;
  options.heartbeat_interval_ticks = 1;
  options.quorum_timeout_ticks = 5;

  kvstore::raft::TestCluster cluster(options);
  const auto leader_id = cluster.WaitForLeader(200);
  if (!Expect(leader_id.has_value(), "cluster should elect leader")) {
    return false;
  }

  // Allow a couple heartbeats to establish quorum-contact.
  cluster.RunTicks(5);

  auto* leader = cluster.node(*leader_id);
  if (!Expect(leader != nullptr, "leader pointer should exist") ||
      !Expect(leader->role() == kvstore::raft::Role::kLeader,
              "role should be leader")) {
    return false;
  }

  std::unordered_map<kvstore::raft::NodeId, std::vector<std::string>> applied;
  for (kvstore::raft::NodeId id : options.node_ids) {
    auto* node = cluster.node(id);
    if (node == nullptr) {
      return Expect(false, "node pointer must exist for all 5 nodes");
    }
    node->SetOnCommitted([id, &applied](std::vector<kvstore::raft::CommittedEntry> entries) {
      for (const auto& entry : entries) {
        applied[id].push_back(entry.command);
      }
    });
  }

  const auto r1 = leader->Propose("cmd-1");
  if (!Expect(r1.Ok(), "leader propose should succeed") ||
      !Expect(r1.index > 0, "propose index must be >0")) {
    return false;
  }

  if (!Expect(WaitForCommit(&cluster, options.node_ids, r1.index, 200),
              "cmd-1 must commit on all nodes")) {
    return false;
  }

  for (kvstore::raft::NodeId id : options.node_ids) {
    const auto* node = cluster.node(id);
    if (node == nullptr) {
      return false;
    }

    const auto entry = node->log_entry_at(r1.index);
    if (!Expect(entry.has_value(), "log entry should exist")) {
      return false;
    }
    if (!Expect(entry->command == "cmd-1", "command replicated to all nodes")) {
      return false;
    }
    if (!Expect(node->last_applied_index() >= r1.index,
                "entry should be applied once committed")) {
      return false;
    }
    if (!Expect(!applied[id].empty() && applied[id].back() == "cmd-1",
                "OnCommitted should include cmd-1")) {
      return false;
    }
  }

  // Majority-commit property: with 2 nodes down (3/5 available), a new entry
  // should still be commit-able.
  std::vector<kvstore::raft::NodeId> down_ids;
  for (kvstore::raft::NodeId id : options.node_ids) {
    if (id == *leader_id) {
      continue;
    }
    if (down_ids.size() < 2) {
      down_ids.push_back(id);
    }
  }
  if (!Expect(down_ids.size() == 2, "must pick 2 followers to take down")) {
    return false;
  }

  cluster.SetNodeUp(down_ids[0], false);
  cluster.SetNodeUp(down_ids[1], false);
  cluster.RunTicks(10);

  leader = cluster.node(*leader_id);
  if (!Expect(cluster.IsNodeUp(*leader_id), "leader must remain up") ||
      !Expect(leader != nullptr && leader->role() == kvstore::raft::Role::kLeader,
              "original leader should remain leader with 3 nodes up")) {
    return false;
  }

  const auto r2 = leader->Propose("cmd-2");
  if (!Expect(r2.Ok(), "cmd-2 propose should succeed with quorum") ||
      !Expect(r2.index == r1.index + 1, "log index should increment")) {
    return false;
  }

  std::vector<kvstore::raft::NodeId> live_ids;
  for (kvstore::raft::NodeId id : options.node_ids) {
    if (id == down_ids[0] || id == down_ids[1]) {
      continue;
    }
    live_ids.push_back(id);
  }
  if (!Expect(WaitForCommit(&cluster, live_ids, r2.index, 200),
              "cmd-2 must commit with 3 nodes")) {
    return false;
  }

  for (kvstore::raft::NodeId id : live_ids) {
    const auto* node = cluster.node(id);
    if (node == nullptr) {
      return false;
    }
    const auto entry = node->log_entry_at(r2.index);
    if (!Expect(entry.has_value() && entry->command == "cmd-2",
                "cmd-2 replicated to live nodes")) {
      return false;
    }
  }

  return true;
}

}  // namespace

int main() {
  if (!TestReplicationAndMajorityCommit()) {
    return 1;
  }
  return 0;
}
