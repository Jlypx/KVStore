#include "kvstore/raft/test_transport.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

auto Expect(bool condition, const std::string& message) -> bool {
  if (!condition) {
    std::cerr << "ASSERTION FAILED: " << message << '\n';
    return false;
  }
  return true;
}

auto WaitForCommitOnLiveNodes(kvstore::raft::TestCluster* cluster,
                             const std::vector<kvstore::raft::NodeId>& node_ids,
                             kvstore::raft::LogIndex index,
                             std::uint64_t max_ticks) -> bool {
  for (std::uint64_t i = 0; i < max_ticks; ++i) {
    cluster->Tick();
    bool ok = true;
    for (kvstore::raft::NodeId id : node_ids) {
      const auto* node = cluster->node(id);
      if (node == nullptr || !cluster->IsNodeUp(id)) {
        continue;
      }
      if (node->committed_index() < index) {
        ok = false;
      }
    }
    if (ok) {
      return true;
    }
  }
  return false;
}

auto TestFailoverPreservesCommittedLog() -> bool {
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
  if (!Expect(leader != nullptr && leader->role() == kvstore::raft::Role::kLeader,
              "leader must exist")) {
    return false;
  }

  const auto r1 = leader->Propose("cmd-1");
  if (!Expect(r1.Ok(), "cmd-1 propose should succeed") ||
      !Expect(r1.index > 0, "cmd-1 index should be >0")) {
    return false;
  }

  if (!Expect(WaitForCommitOnLiveNodes(&cluster, options.node_ids, r1.index, 200),
              "cmd-1 must commit on all nodes")) {
    return false;
  }

  // Simulate leader failure.
  cluster.SetNodeUp(*leader_id, false);

  const auto new_leader_id = cluster.WaitForLeader(500);
  if (!Expect(new_leader_id.has_value(), "cluster should elect new leader")) {
    return false;
  }
  if (!Expect(*new_leader_id != *leader_id, "new leader must differ")) {
    return false;
  }

  const auto* new_leader = cluster.node(*new_leader_id);
  if (!Expect(new_leader != nullptr, "new leader pointer should exist") ||
      !Expect(new_leader->role() == kvstore::raft::Role::kLeader,
              "new leader role should be leader")) {
    return false;
  }

  const auto carried = new_leader->log_entry_at(r1.index);
  if (!Expect(carried.has_value(), "committed entry must exist on new leader") ||
      !Expect(carried->command == "cmd-1", "committed command preserved")) {
    return false;
  }
  if (!Expect(new_leader->committed_index() >= r1.index,
              "new leader must retain committed index")) {
    return false;
  }

  // Propose another entry with the new leader (4 nodes up; quorum is still 3/5).
  auto* mutable_new_leader = cluster.node(*new_leader_id);
  if (!Expect(mutable_new_leader != nullptr, "mutable new leader exists")) {
    return false;
  }
  const auto r2 = mutable_new_leader->Propose("cmd-2");
  if (!Expect(r2.Ok(), "cmd-2 propose should succeed")) {
    return false;
  }

  const std::vector<kvstore::raft::NodeId> live_ids = {1, 2, 3, 4, 5};
  if (!Expect(WaitForCommitOnLiveNodes(&cluster, live_ids, r2.index, 300),
              "cmd-2 must commit on live nodes")) {
    return false;
  }

  // Bring old leader back and ensure it catches up.
  cluster.SetNodeUp(*leader_id, true);
  cluster.RunTicks(200);

  const auto* recovered = cluster.node(*leader_id);
  if (!Expect(recovered != nullptr, "recovered node pointer should exist") ||
      !Expect(recovered->role() != kvstore::raft::Role::kLeader,
              "old leader should not remain leader after rejoin")) {
    return false;
  }

  const auto e1 = recovered->log_entry_at(r1.index);
  const auto e2 = recovered->log_entry_at(r2.index);
  if (!Expect(e1.has_value() && e1->command == "cmd-1",
              "recovered node must have cmd-1") ||
      !Expect(e2.has_value() && e2->command == "cmd-2",
              "recovered node must have cmd-2")) {
    return false;
  }
  if (!Expect(recovered->committed_index() >= r2.index,
              "recovered node must advance commit index")) {
    return false;
  }

  return true;
}

}  // namespace

int main() {
  if (!TestFailoverPreservesCommittedLog()) {
    return 1;
  }
  return 0;
}
