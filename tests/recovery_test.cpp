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

  return 0;
}
