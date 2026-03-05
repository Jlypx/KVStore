#ifndef KVSTORE_ENGINE_WAL_H
#define KVSTORE_ENGINE_WAL_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>

#include "kvstore/integrity/integrity_error.h"

namespace kvstore::engine {

constexpr std::uint32_t kWalMagic = 0x4b565741;
constexpr std::uint16_t kWalVersion = 1;
constexpr std::size_t kWalHeaderSize = 24;

enum class WalOperation : std::uint8_t {
  kPut = 1,
  kDelete = 2,
};

struct WalRecord {
  WalOperation operation = WalOperation::kPut;
  std::string key;
  std::string value;
  std::string request_id;
};

struct WalReplayStats {
  std::size_t records_replayed = 0;
  std::size_t duplicate_requests = 0;
};

struct WalReplayResult {
  WalReplayStats stats;
  std::optional<integrity::IntegrityError> error;

  [[nodiscard]] auto Ok() const -> bool { return !error.has_value(); }
};

class WalWriter {
 public:
  WalWriter() = default;
  ~WalWriter() = default;

  WalWriter(const WalWriter&) = delete;
  auto operator=(const WalWriter&) -> WalWriter& = delete;

  WalWriter(WalWriter&&) = delete;
  auto operator=(WalWriter&&) -> WalWriter& = delete;

  auto Open(const std::filesystem::path& wal_path,
            integrity::IntegrityError* error) -> bool;

  auto Append(const WalRecord& record, integrity::IntegrityError* error) -> bool;

 private:
  std::filesystem::path wal_path_;
  std::ofstream stream_;
};

class WalReader {
 public:
  using RecordCallback = std::function<void(const WalRecord&)>;

  static auto Replay(const std::filesystem::path& wal_path,
                     const RecordCallback& callback, WalReplayResult* result)
      -> bool;
};

}  // namespace kvstore::engine

#endif  // KVSTORE_ENGINE_WAL_H
