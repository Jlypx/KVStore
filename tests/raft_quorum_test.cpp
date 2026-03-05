#include "kvstore/raft/test_transport.h"

#include <cstdint>
#include <iostream>
#include <string>

namespace {

auto Expect(bool condition, const std::string& message) -> bool {
  if (!condition) {
    std::cerr << "ASSERTION FAILED: " << message << '\n';
    return false;
  }
  return true;
}

auto TestQuorumLossRejectsNewProposals() -> bool {
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
  cluster.RunTicks(5);

  auto* leader = cluster.node(*leader_id);
  if (!Expect(leader != nullptr, "leader pointer should exist") ||
      !Expect(leader->role() == kvstore::raft::Role::kLeader,
              "leader role should be leader")) {
    return false;
  }
  if (!Expect(leader->HasQuorumContact(), "leader should have quorum contact")) {
    return false;
  }

  const auto committed_before = leader->committed_index();
  const auto last_index_before = leader->last_log_index();

  // Drop to 2 live nodes (leader + one follower). Majority is still 3/5.
  kvstore::raft::NodeId keep_follower = 0;
  for (kvstore::raft::NodeId id : options.node_ids) {
    if (id != *leader_id) {
      keep_follower = id;
      break;
    }
  }
  if (!Expect(keep_follower != 0, "must pick a follower to keep up")) {
    return false;
  }

  for (kvstore::raft::NodeId id : options.node_ids) {
    if (id == *leader_id || id == keep_follower) {
      continue;
    }
    cluster.SetNodeUp(id, false);
  }

  // Wait for the leader's quorum-contact window to expire.
  cluster.RunTicks(options.quorum_timeout_ticks + 2);

  leader = cluster.node(*leader_id);
  if (!Expect(cluster.IsNodeUp(*leader_id), "leader must remain up") ||
      !Expect(leader != nullptr && leader->role() == kvstore::raft::Role::kLeader,
              "leader stays leader but loses quorum contact")) {
    return false;
  }

  if (!Expect(!leader->HasQuorumContact(),
              "leader must report quorum contact false with only 2 live nodes")) {
    return false;
  }

  const auto propose = leader->Propose("cmd-no-quorum");
  if (!Expect(propose.status == kvstore::raft::ProposeStatus::kQuorumUnavailable,
              "propose must fail with quorum unavailable")) {
    return false;
  }

  // Even with additional ticks, the proposal should not be committed or applied.
  cluster.RunTicks(20);
  if (!Expect(leader->committed_index() == committed_before,
              "commit index must not advance without quorum")) {
    return false;
  }
  if (!Expect(leader->last_log_index() == last_index_before,
              "leader must not append new log entry without quorum")) {
    return false;
  }

  return true;
}

}  // namespace

int main() {
  if (!TestQuorumLossRejectsNewProposals()) {
    return 1;
  }
  return 0;
}
