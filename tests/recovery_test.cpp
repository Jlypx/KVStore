#include "kvstore/engine/kv_engine.h"
#include "kvstore/engine/wal.h"

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

  const auto multi_wal_dir = MakeTempDirectory("recovery_multi_wal");
  const auto wal1 = multi_wal_dir / "000001.wal";
  const auto wal2 = multi_wal_dir / "000002.wal";
  {
    kvstore::engine::WalWriter writer1;
    kvstore::integrity::IntegrityError wal_error;
    if (!Expect(writer1.Open(wal1, &wal_error),
                "first wal writer should open")) {
      return 1;
    }
    kvstore::engine::WalRecord record1;
    record1.operation = kvstore::engine::WalOperation::kPut;
    record1.key = "k5";
    record1.value = "v5";
    record1.request_id = "req-8";
    if (!Expect(writer1.Append(record1, &wal_error),
                "first wal append should succeed")) {
      return 1;
    }

    kvstore::engine::WalWriter writer2;
    if (!Expect(writer2.Open(wal2, &wal_error),
                "second wal writer should open")) {
      return 1;
    }
    kvstore::engine::WalRecord record2;
    record2.operation = kvstore::engine::WalOperation::kPut;
    record2.key = "k6";
    record2.value = "v6";
    record2.request_id = "req-9";
    if (!Expect(writer2.Append(record2, &wal_error),
                "second wal append should succeed")) {
      return 1;
    }
  }

  kvstore::engine::KvEngine multi_wal_recovered(wal1);
  if (!Expect(multi_wal_recovered.Open(),
              "engine open with multiple wal generations should succeed")) {
    return 1;
  }
  const auto value_k5 = multi_wal_recovered.Get("k5");
  const auto value_k6 = multi_wal_recovered.Get("k6");
  if (!Expect(value_k5.has_value() && value_k5.value() == "v5",
              "first wal generation should replay") ||
      !Expect(value_k6.has_value() && value_k6.value() == "v6",
              "second wal generation should replay")) {
    return 1;
  }

  const auto rotated_dir = MakeTempDirectory("recovery_rotated_wal");
  const auto rotated_wal1 = rotated_dir / "000001.wal";
  const auto rotated_wal2 = rotated_dir / "000002.wal";
  {
    kvstore::engine::KvEngine rotating_engine(rotated_wal1);
    if (!Expect(rotating_engine.Open(), "rotating engine open should succeed")) {
      return 1;
    }
    if (!Expect(rotating_engine.Put("k7", "v7", "req-10").Ok(),
                "put before wal rotation should succeed")) {
      return 1;
    }
    if (!Expect(rotating_engine.Flush(),
                "flush before wal rotation should succeed")) {
      return 1;
    }
    if (!Expect(rotating_engine.Put("k8", "v8", "req-11").Ok(),
                "put after wal rotation point should succeed")) {
      return 1;
    }
  }

  if (!Expect(std::filesystem::exists(rotated_wal1),
              "first wal generation should exist")) {
    return 1;
  }
  if (!Expect(std::filesystem::exists(rotated_wal2),
              "second wal generation should exist after flush rotation")) {
    return 1;
  }

  kvstore::engine::KvEngine rotated_recovered(rotated_wal1);
  if (!Expect(rotated_recovered.Open(),
              "engine open after wal rotation should succeed")) {
    return 1;
  }
  const auto value_k7 = rotated_recovered.Get("k7");
  const auto value_k8 = rotated_recovered.Get("k8");
  if (!Expect(value_k7.has_value() && value_k7.value() == "v7",
              "value from first wal generation should survive rotation") ||
      !Expect(value_k8.has_value() && value_k8.value() == "v8",
              "value from second wal generation should survive rotation")) {
    return 1;
  }

  return 0;
}
