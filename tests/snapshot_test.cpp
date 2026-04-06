#include "kvstore/engine/kv_engine.h"
#include "kvstore/raft/raft_storage.h"
#include "kvstore/service/kv_raft_service.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace {

auto MakeTempDirectory(const std::string& suffix) -> std::filesystem::path {
  const auto path = std::filesystem::temp_directory_path() /
                    ("kvstore_snapshot_" + suffix + "_test");
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

auto TestEngineSnapshotExportAndInstall() -> bool {
  const auto source_dir = MakeTempDirectory("source");
  const auto source_wal = source_dir / "000001.wal";
  kvstore::engine::KvEngine source(source_wal);
  if (!Expect(source.Open(), "source engine should open")) {
    return false;
  }

  if (!Expect(source.Put("k1", "v1", "req-1").Ok(), "put k1 should succeed") ||
      !Expect(source.Put("k2", "v2", "req-2").Ok(), "put k2 should succeed") ||
      !Expect(source.Delete("k2", "req-3").Ok(), "delete k2 should succeed") ||
      !Expect(source.Flush(), "source flush should succeed")) {
    return false;
  }

  std::string payload;
  if (!Expect(source.ExportSnapshotPayload(&payload),
              "snapshot export should succeed")) {
    return false;
  }

  const auto target_dir = MakeTempDirectory("target");
  const auto target_wal = target_dir / "000001.wal";
  kvstore::engine::KvEngine target(target_wal);
  if (!Expect(target.Open(), "target engine should open")) {
    return false;
  }

  if (!Expect(target.InstallSnapshotPayload(payload),
              "snapshot install should succeed")) {
    return false;
  }

  const auto value_k1 = target.Get("k1");
  const auto value_k2 = target.Get("k2");
  if (!Expect(value_k1.has_value() && value_k1.value() == "v1",
              "k1 should restore from snapshot") ||
      !Expect(!value_k2.has_value(), "deleted key should stay absent")) {
    return false;
  }

  const auto duplicate_put = target.Put("k1", "v-new", "req-1");
  return Expect(duplicate_put.Ok(), "duplicate put after snapshot should be valid") &&
         Expect(duplicate_put.duplicate, "duplicate request id should be preserved");
}

auto TestFollowerCatchupThroughSnapshotInstall() -> bool {
  const auto dir = MakeTempDirectory("catchup");
  kvstore::raft::NodeId leader_id = 0;
  kvstore::raft::NodeId lagging_follower = 0;

  {
    kvstore::service::RaftOptions options;
    options.snapshot_threshold_entries = 4;
    kvstore::service::KvRaftService service(dir, options);

    const auto leader = service.FindLeader();
    if (!Expect(leader.has_value(), "leader should be elected")) {
      return false;
    }
    leader_id = *leader;
    lagging_follower = (leader_id == 5) ? 4 : 5;
    service.SetNodeUp(lagging_follower, false);

    for (int i = 0; i < 12; ++i) {
      const auto result = service.Put("snap-k-" + std::to_string(i),
                                      "snap-v-" + std::to_string(i),
                                      "snap-req-" + std::to_string(i),
                                      std::chrono::steady_clock::now() +
                                          std::chrono::seconds(5));
      if (std::holds_alternative<kvstore::service::Error>(result)) {
        const auto& error = std::get<kvstore::service::Error>(result);
        std::cerr << "put-before i=" << i
                  << " error code=" << static_cast<int>(error.code)
                  << " message=" << error.message
                  << " leader_hint=" << error.leader_hint << '\n';
      }
      if (!Expect(std::holds_alternative<kvstore::service::PutResult>(result),
                  "leader write before snapshot catch-up should succeed")) {
        return false;
      }
    }

    if (!Expect(service.ForceLeaderSnapshotForTesting(),
                "leader snapshot should be forced after enough writes")) {
      return false;
    }

    service.SetNodeUp(lagging_follower, true);
    for (int i = 12; i < 16; ++i) {
      const auto result = service.Put("snap-k-" + std::to_string(i),
                                      "snap-v-" + std::to_string(i),
                                      "snap-req-" + std::to_string(i),
                                      std::chrono::steady_clock::now() +
                                          std::chrono::seconds(5));
      if (std::holds_alternative<kvstore::service::Error>(result)) {
        const auto& error = std::get<kvstore::service::Error>(result);
        std::cerr << "put-after i=" << i
                  << " error code=" << static_cast<int>(error.code)
                  << " message=" << error.message
                  << " leader_hint=" << error.leader_hint << '\n';
      }
      if (!Expect(std::holds_alternative<kvstore::service::PutResult>(result),
                  "leader write after follower recovery should succeed")) {
        return false;
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }

  const auto follower_raft_dir =
      dir / ("node" + std::to_string(lagging_follower)) / "raft";
  kvstore::raft::RaftStorage follower_storage(follower_raft_dir);
  kvstore::raft::PersistentRaftState follower_state;
  if (!Expect(follower_storage.Load(&follower_state),
              "follower raft storage should load after snapshot catch-up")) {
    return false;
  }
  if (!Expect(follower_state.snapshot_last_included_index > 0,
              "lagging follower should record installed snapshot metadata")) {
    return false;
  }

  const auto follower_wal =
      dir / ("node" + std::to_string(lagging_follower)) / "000001.wal";
  kvstore::engine::KvEngine follower_engine(follower_wal);
  if (!Expect(follower_engine.Open(), "follower engine should reopen")) {
    return false;
  }
  const auto latest = follower_engine.Get("snap-k-15");
  return Expect(latest.has_value() && latest.value() == "snap-v-15",
                "lagging follower engine should catch up through snapshot");
}

}  // namespace

int main() {
  if (!TestEngineSnapshotExportAndInstall()) {
    return 1;
  }
  if (!TestFollowerCatchupThroughSnapshotInstall()) {
    return 1;
  }
  return 0;
}
