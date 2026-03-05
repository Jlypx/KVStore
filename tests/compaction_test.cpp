#include "kvstore/engine/kv_engine.h"
#include "kvstore/engine/sstable.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "kvstore/integrity/integrity_error.h"

namespace {

auto MakeTempDirectory(const std::string& suffix) -> std::filesystem::path {
  const auto path = std::filesystem::temp_directory_path() /
                    ("kvstore_task4_" + suffix + "_test");
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

auto CorruptByte(const std::filesystem::path& file, std::streamoff offset) -> bool {
  std::fstream stream(file, std::ios::binary | std::ios::in | std::ios::out);
  if (!stream.is_open()) {
    return false;
  }
  stream.seekg(offset, std::ios::beg);
  char value = 0;
  stream.read(&value, 1);
  if (stream.gcount() != 1) {
    return false;
  }
  value = static_cast<char>(value ^ 0x1);
  stream.seekp(offset, std::ios::beg);
  stream.write(&value, 1);
  stream.flush();
  return stream.good();
}

auto CountSstFiles(const std::filesystem::path& dir) -> std::size_t {
  std::size_t count = 0;
  std::error_code ec;
  const auto sst_dir = dir / "sst";
  if (!std::filesystem::exists(sst_dir)) {
    return 0;
  }
  for (const auto& entry : std::filesystem::directory_iterator(sst_dir, ec)) {
    if (ec) {
      break;
    }
    if (entry.is_regular_file() && entry.path().extension() == ".sst") {
      count += 1;
    }
  }
  return count;
}

auto FirstSstPath(const std::filesystem::path& dir) -> std::filesystem::path {
  const auto sst_dir = dir / "sst";
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(sst_dir, ec)) {
    if (entry.is_regular_file() && entry.path().extension() == ".sst") {
      return entry.path();
    }
  }
  return {};
}

auto TestCompactionPreservesLatestValueAndReducesOverlap() -> bool {
  const auto dir = MakeTempDirectory("compaction_basic");
  const auto wal = dir / "000001.wal";

  kvstore::engine::KvEngine engine(wal);
  if (!Expect(engine.Open(), "engine open should succeed")) {
    return false;
  }

  engine.Put("k", "v1", "rid-1");
  engine.Put("a", "1", "rid-2");
  if (!Expect(engine.Flush(), "first flush should succeed")) {
    return false;
  }

  engine.Put("k", "v2", "rid-3");
  engine.Put("b", "2", "rid-4");
  if (!Expect(engine.Flush(), "second flush should succeed")) {
    return false;
  }

  if (!Expect(CountSstFiles(dir) >= 2, "expected at least 2 SST files")) {
    return false;
  }
  const auto before = engine.Get("k");
  if (!Expect(before.has_value() && before.value() == "v2",
              "newest value should be visible before compaction")) {
    return false;
  }

  if (!Expect(engine.Compact(), "compaction should succeed")) {
    if (engine.last_integrity_error().has_value()) {
      std::cerr << kvstore::integrity::FormatIntegrityLogLine(
                       engine.last_integrity_error().value())
                << '\n';
    }
    return false;
  }

  if (!Expect(CountSstFiles(dir) == 1, "compaction should leave exactly 1 SST")) {
    return false;
  }
  const auto after = engine.Get("k");
  return Expect(after.has_value() && after.value() == "v2",
                "latest value must survive compaction");
}

auto TestCompactionFailsOnCorruptedInput() -> bool {
  const auto dir = MakeTempDirectory("compaction_corrupt");
  const auto wal = dir / "000001.wal";

  kvstore::engine::KvEngine engine(wal);
  if (!Expect(engine.Open(), "engine open should succeed")) {
    return false;
  }

  engine.Put("k", "v1", "rid-1");
  if (!Expect(engine.Flush(), "flush should succeed")) {
    return false;
  }
  engine.Put("k", "v2", "rid-2");
  if (!Expect(engine.Flush(), "flush should succeed")) {
    return false;
  }

  const auto sst_path = FirstSstPath(dir);
  if (!Expect(!sst_path.empty(), "sst file should exist")) {
    return false;
  }
  // Corrupt first data block payload byte.
  const auto corrupt_offset =
      static_cast<std::streamoff>(kvstore::engine::kSstHeaderSize + 4);
  if (!Expect(CorruptByte(sst_path, corrupt_offset), "corrupt sst byte")) {
    return false;
  }

  const bool compact_ok = engine.Compact();
  if (!Expect(!compact_ok, "compaction must fail on corrupted input")) {
    return false;
  }
  return Expect(engine.last_integrity_error().has_value(), "integrity error expected") &&
         Expect(engine.last_integrity_error()->code ==
                    kvstore::integrity::IntegrityErrorCode::kChecksumMismatch,
                "checksum mismatch expected");
}

}  // namespace

int main() {
  if (!TestCompactionPreservesLatestValueAndReducesOverlap()) {
    return 1;
  }
  if (!TestCompactionFailsOnCorruptedInput()) {
    return 1;
  }
  return 0;
}
