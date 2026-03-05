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
  const auto dir = MakeTempDirectory("idempotency");
  const auto wal = dir / "000001.wal";

  {
    kvstore::engine::KvEngine engine(wal);
    if (!Expect(engine.Open(), "engine open should succeed")) {
      return 1;
    }

    const auto first_put = engine.Put("dup-key", "v1", "dup-req-1");
    const auto duplicate_put = engine.Put("dup-key", "v2", "dup-req-1");

    if (!Expect(first_put.Ok() && first_put.applied,
                "first idempotent write should apply") ||
        !Expect(duplicate_put.Ok() && duplicate_put.duplicate,
                "duplicate request should be skipped")) {
      return 1;
    }

    const auto value = engine.Get("dup-key");
    if (!Expect(value.has_value() && value.value() == "v1",
                "duplicate must not overwrite original value")) {
      return 1;
    }
  }

  kvstore::engine::KvEngine restarted(wal);
  if (!Expect(restarted.Open(), "restart open should succeed")) {
    return 1;
  }

  const auto retry_after_restart =
      restarted.Put("dup-key", "v3", "dup-req-1");
  const auto final_value = restarted.Get("dup-key");

  if (!Expect(retry_after_restart.Ok() && retry_after_restart.duplicate,
              "duplicate retry after restart should be skipped") ||
      !Expect(final_value.has_value() && final_value.value() == "v1",
              "value should remain from original request")) {
    return 1;
  }

  return 0;
}
