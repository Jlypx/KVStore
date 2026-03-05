#include "kvstore/raft/test_transport.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>

namespace {

auto Expect(bool condition, const std::string& message) -> bool {
  if (!condition) {
    std::cerr << "ASSERTION FAILED: " << message << '\n';
    return false;
  }
  return true;
}

auto TestLeaderElectionSingleLeaderPerTerm() -> bool {
  kvstore::raft::TestCluster::Options options;
  options.node_ids = {1, 2, 3, 4, 5};
  options.election_timeout_min_ticks = 5;
  options.election_timeout_max_ticks = 10;
  options.heartbeat_interval_ticks = 1;
  options.quorum_timeout_ticks = 5;

  kvstore::raft::TestCluster cluster(options);
  const auto leader = cluster.WaitForLeader(200);
  if (!Expect(leader.has_value(), "cluster should elect a leader")) {
    return false;
  }

  // In a stable cluster (no partitions), we should not observe two leaders.
  // Also, per-term uniqueness is a safety invariant.
  std::map<kvstore::raft::Term, kvstore::raft::NodeId> leader_by_term;
  for (std::size_t step = 0; step < 200; ++step) {
    cluster.Tick();

    std::size_t leader_count = 0;
    for (kvstore::raft::NodeId id : options.node_ids) {
      const auto* node = cluster.node(id);
      if (node == nullptr || !cluster.IsNodeUp(id)) {
        continue;
      }
      if (node->role() != kvstore::raft::Role::kLeader) {
        continue;
      }

      leader_count += 1;
      const auto term = node->current_term();
      const auto it = leader_by_term.find(term);
      if (it == leader_by_term.end()) {
        leader_by_term[term] = id;
      } else if (it->second != id) {
        return Expect(false, "observed two leaders in same term");
      }
    }

    if (!Expect(leader_count <= 1, "stable cluster must have <=1 leader")) {
      return false;
    }
  }

  const auto stable_leader = cluster.FindLeader();
  if (!Expect(stable_leader.has_value(), "cluster should still have a leader")) {
    return false;
  }

  const auto* leader_node = cluster.node(*stable_leader);
  if (!Expect(leader_node != nullptr, "leader node pointer should exist") ||
      !Expect(leader_node->role() == kvstore::raft::Role::kLeader,
              "leader role must be leader")) {
    return false;
  }

  // Quorum-contact gate should eventually become true in a stable cluster.
  if (!Expect(leader_node->HasQuorumContact(),
              "leader should have quorum contact in stable cluster")) {
    return false;
  }

  return true;
}

}  // namespace

int main() {
  if (!TestLeaderElectionSingleLeaderPerTerm()) {
    return 1;
  }
  return 0;
}
