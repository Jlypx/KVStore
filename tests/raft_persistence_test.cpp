#include "kvstore/raft/raft_storage.h"
#include "kvstore/raft/test_transport.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

auto MakeTempDirectory(const std::string& suffix) -> std::filesystem::path {
  const auto path = std::filesystem::temp_directory_path() /
                    ("kvstore_raft_persistence_" + suffix + "_test");
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
  std::filesystem::create_directories(path, ec);
  return path;
}

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

auto TestRaftStorageRoundTrip() -> bool {
  const auto dir = MakeTempDirectory("storage");
  kvstore::raft::RaftStorage storage(dir);

  std::vector<kvstore::raft::LogEntry> expected_log = {
      kvstore::raft::LogEntry{.term = 0, .command = ""},
      kvstore::raft::LogEntry{.term = 2, .command = "cmd-a"},
      kvstore::raft::LogEntry{.term = 3, .command = "cmd-b"},
  };

  if (!Expect(storage.StoreMetadata(3, 2, 10, 2), "metadata store should succeed") ||
      !Expect(storage.StoreLog(expected_log), "log store should succeed")) {
    return false;
  }

  kvstore::raft::PersistentRaftState loaded;
  if (!Expect(storage.Load(&loaded), "storage load should succeed")) {
    return false;
  }

  return Expect(loaded.current_term == 3, "current term should round-trip") &&
         Expect(loaded.voted_for == 2, "voted_for should round-trip") &&
         Expect(loaded.snapshot_last_included_index == 10,
                "snapshot index should round-trip") &&
         Expect(loaded.snapshot_last_included_term == 2,
                "snapshot term should round-trip") &&
         Expect(loaded.log.size() == expected_log.size(),
                "log size should round-trip") &&
         Expect(loaded.log[1].command == "cmd-a", "first command should round-trip") &&
         Expect(loaded.log[2].command == "cmd-b", "second command should round-trip");
}

auto TestRaftNodeLoadsPersistedStateOnRestart() -> bool {
  const auto dir = MakeTempDirectory("cluster");

  kvstore::raft::TestCluster::Options options;
  options.node_ids = {1, 2, 3, 4, 5};
  options.election_timeout_min_ticks = 5;
  options.election_timeout_max_ticks = 10;
  options.heartbeat_interval_ticks = 1;
  options.quorum_timeout_ticks = 5;
  options.storage_root = dir;

  kvstore::raft::LogIndex committed_index = 0;
  kvstore::raft::Term committed_term = 0;
  {
    kvstore::raft::TestCluster cluster(options);
    const auto leader_id = cluster.WaitForLeader(200);
    if (!Expect(leader_id.has_value(), "cluster should elect leader")) {
      return false;
    }
    cluster.RunTicks(5);

    auto* leader = cluster.node(*leader_id);
    if (!Expect(leader != nullptr, "leader pointer should exist")) {
      return false;
    }

    const auto proposal = leader->Propose("persisted-cmd");
    if (!Expect(proposal.Ok(), "proposal should succeed")) {
      return false;
    }
    if (!Expect(WaitForCommit(&cluster, options.node_ids, proposal.index, 200),
                "proposal should commit")) {
      return false;
    }

    committed_index = proposal.index;
    committed_term = leader->current_term();
  }

  kvstore::raft::TestCluster restarted(options);
  const auto* restarted_node = restarted.node(1);
  if (!Expect(restarted_node != nullptr, "restarted node should exist")) {
    return false;
  }

  const auto entry = restarted_node->log_entry_at(committed_index);
  return Expect(restarted_node->current_term() >= committed_term,
                "restarted node should load persisted term") &&
         Expect(entry.has_value(), "restarted node should load persisted log") &&
         Expect(entry->command == "persisted-cmd",
                "restarted node should keep committed command");
}

auto TestLocalSnapshotTruncatesRaftPrefix() -> bool {
  kvstore::raft::TestCluster::Options options;
  options.node_ids = {1, 2, 3, 4, 5};
  options.election_timeout_min_ticks = 5;
  options.election_timeout_max_ticks = 10;
  options.heartbeat_interval_ticks = 1;
  options.quorum_timeout_ticks = 5;
  options.snapshot_threshold_entries = 4;

  kvstore::raft::TestCluster cluster(options);
  const auto leader_id = cluster.WaitForLeader(200);
  if (!Expect(leader_id.has_value(), "cluster should elect leader")) {
    return false;
  }
  cluster.RunTicks(5);

  auto* leader = cluster.node(*leader_id);
  if (!Expect(leader != nullptr, "leader should exist")) {
    return false;
  }

  kvstore::raft::LogIndex last_index = 0;
  for (int i = 0; i < 8; ++i) {
    const auto proposal = leader->Propose("snap-cmd-" + std::to_string(i));
    if (!Expect(proposal.Ok(), "proposal before snapshot should succeed")) {
      return false;
    }
    last_index = proposal.index;
    if (!Expect(WaitForCommit(&cluster, options.node_ids, proposal.index, 200),
                "proposal before snapshot should commit")) {
      return false;
    }
  }

  const auto before_snapshot_entry = leader->log_entry_at(1);
  if (!Expect(before_snapshot_entry.has_value(),
              "pre-snapshot entry should be present")) {
    return false;
  }

  const auto suggested_snapshot = leader->MaybeSnapshotMetadata();
  if (!Expect(suggested_snapshot.has_value(), "snapshot metadata should be available")) {
    return false;
  }
  const auto snapshot_term_entry = leader->log_entry_at(last_index - 1);
  if (!Expect(snapshot_term_entry.has_value(),
              "snapshot boundary entry should be readable")) {
    return false;
  }
  const kvstore::raft::SnapshotMetadata snapshot{
      .last_included_index = last_index - 1,
      .last_included_term = snapshot_term_entry->term,
  };
  leader->InstallLocalSnapshot(snapshot);

  const auto old_entry = leader->log_entry_at(1);
  const auto latest_entry = leader->log_entry_at(last_index);
  return Expect(leader->snapshot_last_included_index() ==
                    snapshot.last_included_index,
                "snapshot base index should advance") &&
         Expect(!old_entry.has_value(),
                "entries before snapshot base should be truncated") &&
         Expect(latest_entry.has_value() &&
                    latest_entry->command == "snap-cmd-7",
                "log suffix after snapshot should remain available");
}

}  // namespace

int main() {
  if (!TestRaftStorageRoundTrip()) {
    return 1;
  }
  if (!TestRaftNodeLoadsPersistedStateOnRestart()) {
    return 1;
  }
  if (!TestLocalSnapshotTruncatesRaftPrefix()) {
    return 1;
  }
  return 0;
}
