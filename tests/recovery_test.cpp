#include "kvstore/engine/kv_engine.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

auto MakeTempDirectory(const std::string& suffix) -> std::filesystem::path {
  const auto path = std::filesystem::temp_directory_path() /
                    ("kvstore_task3_" + suffix + "_test");
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

}  // namespace

int main() {
  const auto dir = MakeTempDirectory("recovery");
  const auto wal = dir / "000001.wal";

  {
    kvstore::engine::KvEngine engine(wal);
    if (!Expect(engine.Open(), "engine open should succeed")) {
      return 1;
    }

    const auto put_a = engine.Put("k1", "v1", "req-1");
    const auto put_b = engine.Put("k2", "v2", "req-2");
    const auto del_b = engine.Delete("k2", "req-3");

    if (!Expect(put_a.Ok() && put_a.applied, "first put should apply") ||
        !Expect(put_b.Ok() && put_b.applied, "second put should apply") ||
        !Expect(del_b.Ok() && del_b.applied, "delete should apply")) {
      return 1;
    }
  }

  kvstore::engine::KvEngine recovered(wal);
  if (!Expect(recovered.Open(), "recovered engine open should succeed")) {
    return 1;
  }

  const auto value_k1 = recovered.Get("k1");
  const auto value_k2 = recovered.Get("k2");
  const auto stats = recovered.recovery_stats();

  if (!Expect(value_k1.has_value() && value_k1.value() == "v1",
              "k1 value should survive restart") ||
      !Expect(!value_k2.has_value(), "k2 should remain deleted after replay") ||
      !Expect(stats.records_replayed == 3,
              "replay should consume exactly 3 records")) {
    return 1;
  }

  {
    kvstore::engine::KvEngine flushed_then_deleted(wal);
    if (!Expect(flushed_then_deleted.Open(), "engine reopen should succeed")) {
      return 1;
    }

    const auto put_c = flushed_then_deleted.Put("k3", "v3", "req-4");
    if (!Expect(put_c.Ok() && put_c.applied, "third put should apply")) {
      return 1;
    }
    if (!Expect(flushed_then_deleted.Flush(), "flush before delete should succeed")) {
      return 1;
    }

    const auto del_c = flushed_then_deleted.Delete("k3", "req-5");
    if (!Expect(del_c.Ok() && del_c.applied, "delete after flush should apply")) {
      return 1;
    }
    if (!Expect(flushed_then_deleted.Flush(),
                "flush after delete should succeed")) {
      return 1;
    }
  }

  kvstore::engine::KvEngine deleted_after_flush(wal);
  if (!Expect(deleted_after_flush.Open(),
              "engine reopen after flushed delete should succeed")) {
    return 1;
  }

  const auto value_k3 = deleted_after_flush.Get("k3");
  if (!Expect(!value_k3.has_value(),
              "k3 should remain deleted after flush plus restart")) {
    return 1;
  }

  const auto tombstone_dir = MakeTempDirectory("recovery_tombstone_sst");
  const auto tombstone_wal = tombstone_dir / "000001.wal";
  {
    kvstore::engine::KvEngine tombstone_engine(tombstone_wal);
    if (!Expect(tombstone_engine.Open(),
                "tombstone engine open should succeed")) {
      return 1;
    }
    if (!Expect(tombstone_engine.Put("k4", "v4", "req-6").Ok(),
                "put before tombstone flush should succeed")) {
      return 1;
    }
    if (!Expect(tombstone_engine.Flush(),
                "flush before tombstone delete should succeed")) {
      return 1;
    }
    if (!Expect(tombstone_engine.Delete("k4", "req-7").Ok(),
                "delete before tombstone flush should succeed")) {
      return 1;
    }
    if (!Expect(tombstone_engine.Flush(),
                "flush after tombstone delete should succeed")) {
      return 1;
    }
  }

  std::error_code remove_ec;
  std::filesystem::remove(tombstone_wal, remove_ec);
  if (!Expect(!remove_ec, "wal removal should succeed")) {
    return 1;
  }

  kvstore::engine::KvEngine tombstone_from_sst_only(tombstone_wal);
  if (!Expect(tombstone_from_sst_only.Open(),
              "engine open from SST-only state should succeed")) {
    return 1;
  }
  const auto value_k4 = tombstone_from_sst_only.Get("k4");
  if (!Expect(!value_k4.has_value(),
              "k4 should remain deleted when recovered from SST only")) {
    return 1;
  }

  return 0;
}
