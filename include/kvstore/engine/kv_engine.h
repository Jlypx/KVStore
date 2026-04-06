#ifndef KVSTORE_ENGINE_KV_ENGINE_H
#define KVSTORE_ENGINE_KV_ENGINE_H

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "kvstore/cache/block_cache.h"

#include "kvstore/engine/compaction.h"
#include "kvstore/engine/memtable.h"
#include "kvstore/engine/sstable.h"
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

  // Reads are served from MemTable first, then newest-to-oldest SSTables.
  // On SST corruption, this returns std::nullopt and sets last_integrity_error().
  [[nodiscard]] auto Get(const std::string& key) -> std::optional<std::string>;

  // Flushes MemTable to a new SSTable and clears in-memory kv state.
  // Request-id history is retained for idempotency.
  auto Flush() -> bool;

  // Compacts current SSTables into a single SSTable.
  // This is a minimal v1 compaction suitable for tests and correctness.
  auto Compact() -> bool;

  [[nodiscard]] auto recovery_stats() const -> WalReplayStats;

  [[nodiscard]] auto last_integrity_error() const
      -> std::optional<integrity::IntegrityError>;

 private:
  auto ApplyMutation(const Mutation& mutation) -> ApplyResult;
  auto LoadSstables() -> bool;
  auto NextSstPath() -> std::filesystem::path;
  auto NextWalPath() -> std::filesystem::path;

  std::filesystem::path wal_path_;
  std::filesystem::path sst_dir_;
  MemTable memtable_;
  WalWriter wal_writer_;
  WalReplayStats recovery_stats_;
  std::optional<integrity::IntegrityError> last_integrity_error_;

  std::shared_ptr<cache::BlockCache> block_cache_;

  struct SstableFile {
    std::filesystem::path path;
    SstReader reader;
  };
  std::vector<SstableFile> sstables_;  // oldest -> newest
  std::uint64_t next_sst_id_ = 1;
  std::uint64_t next_wal_generation_ = 1;
};

}  // namespace kvstore::engine

#endif  // KVSTORE_ENGINE_KV_ENGINE_H
