#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "kvstore/engine/kv_engine.h"
#include "kvstore/engine/sstable.h"
#include "kvstore/engine/wal.h"

namespace {

auto PrintUsage() -> int {
  std::cerr << "usage:\n"
            << "  integrity_gate_test make_wal <wal_path>\n"
            << "  integrity_gate_test replay_wal <wal_path>\n"
            << "  integrity_gate_test make_sst <wal_path>\n"
            << "  integrity_gate_test read_sst <sst_path> <key>\n";
  return 2;
}

auto PrintErrorJson(const std::string& mode, const std::string& reason) -> int {
  std::cout << "{\"mode\":\"" << mode
            << "\",\"pass\":false,\"reason\":\"" << reason << "\"}\n";
  return 1;
}

auto RunMakeWal(const std::filesystem::path& wal_path) -> int {
  std::error_code ec;
  std::filesystem::create_directories(wal_path.parent_path(), ec);
  if (ec) {
    return PrintErrorJson("make_wal", "mkdir_failed");
  }

  kvstore::engine::KvEngine engine(wal_path);
  if (!engine.Open()) {
    return PrintErrorJson("make_wal", "engine_open_failed");
  }
  if (!engine.Put("integrity-k", "integrity-v", "integrity-req-1").Ok()) {
    return PrintErrorJson("make_wal", "put_failed");
  }

  std::cout << "{\"mode\":\"make_wal\",\"pass\":true,\"wal\":\""
            << wal_path.string() << "\"}\n";
  return 0;
}

auto RunReplayWal(const std::filesystem::path& wal_path) -> int {
  kvstore::engine::WalReplayResult replay_result;
  const bool replay_ok = kvstore::engine::WalReader::Replay(
      wal_path, [](const kvstore::engine::WalRecord&) {}, &replay_result);

  if (replay_ok && replay_result.Ok()) {
    std::cout << "{\"mode\":\"replay_wal\",\"pass\":true,\"replay_ok\":true}"
              << '\n';
    return 0;
  }

  if (!replay_result.error.has_value()) {
    return PrintErrorJson("replay_wal", "failed_without_integrity_error");
  }

  std::cout << "{\"mode\":\"replay_wal\",\"pass\":false,\"replay_ok\":false,"
            << "\"integrity_code\":\""
            << kvstore::integrity::ToString(replay_result.error->code)
            << "\",\"message\":\"" << replay_result.error->message << "\"}\n";
  return 1;
}

auto RunMakeSst(const std::filesystem::path& wal_path) -> int {
  std::error_code ec;
  std::filesystem::create_directories(wal_path.parent_path(), ec);
  if (ec) {
    return PrintErrorJson("make_sst", "mkdir_failed");
  }

  kvstore::engine::KvEngine engine(wal_path);
  if (!engine.Open()) {
    return PrintErrorJson("make_sst", "engine_open_failed");
  }
  if (!engine.Put("integrity-k", "sst-value", "integrity-sst-req-1").Ok()) {
    return PrintErrorJson("make_sst", "put_failed");
  }
  if (!engine.Flush()) {
    return PrintErrorJson("make_sst", "flush_failed");
  }

  const auto sst_path = wal_path.parent_path() / "sst" / "000001.sst";
  if (!std::filesystem::exists(sst_path)) {
    return PrintErrorJson("make_sst", "sst_not_found");
  }

  std::cout << "{\"mode\":\"make_sst\",\"pass\":true,\"sst\":\""
            << sst_path.string() << "\"}\n";
  return 0;
}

auto RunReadSst(const std::filesystem::path& sst_path, const std::string& key) -> int {
  kvstore::engine::SstReader reader;
  kvstore::integrity::IntegrityError open_error;
  if (!reader.Open(sst_path, &open_error)) {
    std::cout << "{\"mode\":\"read_sst\",\"pass\":false,\"open_ok\":false,"
              << "\"integrity_code\":\"" << kvstore::integrity::ToString(open_error.code)
              << "\",\"message\":\"" << open_error.message << "\"}\n";
    return 1;
  }

  const auto got = reader.Get(key);
  if (got.Ok()) {
    std::cout << "{\"mode\":\"read_sst\",\"pass\":true,\"read_ok\":true,\"found\":"
              << (got.found ? "true" : "false") << "}\n";
    return 0;
  }

  if (!got.error.has_value()) {
    return PrintErrorJson("read_sst", "failed_without_integrity_error");
  }

  std::cout << "{\"mode\":\"read_sst\",\"pass\":false,\"read_ok\":false,"
            << "\"integrity_code\":\""
            << kvstore::integrity::ToString(got.error->code)
            << "\",\"message\":\"" << got.error->message << "\"}\n";
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    return PrintUsage();
  }

  const std::string mode = argv[1];
  if (mode == "make_wal" && argc == 3) {
    return RunMakeWal(argv[2]);
  }
  if (mode == "replay_wal" && argc == 3) {
    return RunReplayWal(argv[2]);
  }
  if (mode == "make_sst" && argc == 3) {
    return RunMakeSst(argv[2]);
  }
  if (mode == "read_sst" && argc == 4) {
    return RunReadSst(argv[2], argv[3]);
  }
  return PrintUsage();
}
