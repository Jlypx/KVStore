#ifndef KVSTORE_ENGINE_KV_ENGINE_H
#define KVSTORE_ENGINE_KV_ENGINE_H

#include <filesystem>
#include <optional>
#include <string>

#include "kvstore/engine/memtable.h"
#include "kvstore/engine/wal.h"
#include "kvstore/integrity/integrity_error.h"

namespace kvstore::engine {

struct ApplyResult {
  bool applied = false;
  bool duplicate = false;
  std::optional<integrity::IntegrityError> error;

  [[nodiscard]] auto Ok() const -> bool { return !error.has_value(); }
};

class KvEngine {
 public:
  explicit KvEngine(std::filesystem::path wal_path);

  auto Open() -> bool;

  auto Put(std::string key, std::string value, std::string request_id)
      -> ApplyResult;

  auto Delete(std::string key, std::string request_id) -> ApplyResult;

  [[nodiscard]] auto Get(const std::string& key) const
      -> std::optional<std::string>;

  [[nodiscard]] auto recovery_stats() const -> WalReplayStats;

  [[nodiscard]] auto last_integrity_error() const
      -> std::optional<integrity::IntegrityError>;

 private:
  auto ApplyMutation(const Mutation& mutation) -> ApplyResult;

  std::filesystem::path wal_path_;
  MemTable memtable_;
  WalWriter wal_writer_;
  WalReplayStats recovery_stats_;
  std::optional<integrity::IntegrityError> last_integrity_error_;
};

}  // namespace kvstore::engine

#endif  // KVSTORE_ENGINE_KV_ENGINE_H
