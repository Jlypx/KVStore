#ifndef KVSTORE_ENGINE_COMPACTION_H
#define KVSTORE_ENGINE_COMPACTION_H

#include <filesystem>
#include <memory>
#include <utility>
#include <vector>

#include "kvstore/engine/sstable.h"
#include "kvstore/integrity/integrity_error.h"

namespace kvstore::cache {
class BlockCache;
}  // namespace kvstore::cache

namespace kvstore::engine {

// Minimal compaction: merge input SSTables into one output SSTable.
//
// Correctness rule: "newest wins" for duplicate keys.
// - If inputs are ordered oldest->newest, compaction will process newest first.
// - Input blocks are read via SstReader which verifies block checksums.
// - Output blocks are written via SstWriter with fresh block checksums.
class Compactor {
 public:
  static auto CompactToSingleSstable(
      const std::vector<std::filesystem::path>& inputs_oldest_to_newest,
      const std::filesystem::path& output,
      const SstWriteOptions& options,
      std::shared_ptr<cache::BlockCache> block_cache,
      integrity::IntegrityError* error) -> bool;
};

}  // namespace kvstore::engine

#endif  // KVSTORE_ENGINE_COMPACTION_H
