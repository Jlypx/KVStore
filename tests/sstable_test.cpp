#include "kvstore/engine/kv_engine.h"
#include "kvstore/engine/sstable.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

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

auto TestSstableRoundTripAndChecksumDetection() -> bool {
  const auto dir = MakeTempDirectory("sstable_roundtrip");
  const auto sst = dir / "000001.sst";

  std::vector<std::pair<std::string, std::string>> kvs;
  kvs.emplace_back("alpha", "one");
  kvs.emplace_back("beta", "two");
  kvs.emplace_back("carrot", "three");

  kvstore::integrity::IntegrityError write_error;
  const kvstore::engine::SstWriteOptions options{.target_block_size = 64};
  if (!Expect(kvstore::engine::SstWriter::Write(sst, kvs, options, &write_error),
              "sst writer should succeed")) {
    std::cerr << kvstore::integrity::FormatIntegrityLogLine(write_error) << '\n';
    return false;
  }

  kvstore::engine::SstReader reader;
  kvstore::integrity::IntegrityError open_error;
  if (!Expect(reader.Open(sst, &open_error), "sst reader open should succeed")) {
    std::cerr << kvstore::integrity::FormatIntegrityLogLine(open_error) << '\n';
    return false;
  }

  const auto got_alpha = reader.Get("alpha");
  if (!Expect(got_alpha.Ok() && got_alpha.found && got_alpha.value.value() == "one",
              "alpha should be readable")) {
    return false;
  }

  // Corrupt first data block payload byte (after header + payload_size field).
  const auto corrupt_offset =
      static_cast<std::streamoff>(kvstore::engine::kSstHeaderSize + 4);
  if (!Expect(CorruptByte(sst, corrupt_offset), "corruption should succeed")) {
    return false;
  }

  kvstore::engine::SstReader corrupted;
  kvstore::integrity::IntegrityError open2;
  if (!Expect(corrupted.Open(sst, &open2),
              "sst reader open should still succeed (footer/index intact)")) {
    // If open fails due to index/footer corruption, that's also acceptable, but
    // this test targets data block checksum verification.
    std::cerr << kvstore::integrity::FormatIntegrityLogLine(open2) << '\n';
    return false;
  }

  const auto got_after_corrupt = corrupted.Get("alpha");
  return Expect(!got_after_corrupt.Ok(), "read should surface integrity error") &&
         Expect(got_after_corrupt.error.has_value(), "error must be populated") &&
         Expect(got_after_corrupt.error->code ==
                    kvstore::integrity::IntegrityErrorCode::kChecksumMismatch,
                "checksum mismatch expected");
}

auto TestEngineFlushServesReadsFromSst() -> bool {
  const auto dir = MakeTempDirectory("engine_flush");
  const auto wal = dir / "000001.wal";

  kvstore::engine::KvEngine engine(wal);
  if (!Expect(engine.Open(), "engine open should succeed")) {
    return false;
  }

  if (!Expect(engine.Put("k1", "v1", "rid-1").Ok(), "put should succeed") ||
      !Expect(engine.Put("k2", "v2", "rid-2").Ok(), "put should succeed")) {
    return false;
  }

  if (!Expect(engine.Flush(), "flush should succeed")) {
    if (engine.last_integrity_error().has_value()) {
      std::cerr << kvstore::integrity::FormatIntegrityLogLine(
                       engine.last_integrity_error().value())
                << '\n';
    }
    return false;
  }

  const auto value = engine.Get("k1");
  return Expect(value.has_value() && value.value() == "v1",
                "read should succeed after flush (from SST)");
}

}  // namespace

int main() {
  if (!TestSstableRoundTripAndChecksumDetection()) {
    return 1;
  }
  if (!TestEngineFlushServesReadsFromSst()) {
    return 1;
  }
  return 0;
}
