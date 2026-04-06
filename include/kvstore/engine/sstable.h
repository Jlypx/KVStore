#ifndef KVSTORE_ENGINE_SSTABLE_H
#define KVSTORE_ENGINE_SSTABLE_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kvstore/integrity/integrity_error.h"

namespace kvstore::cache {
class BlockCache;
}  // namespace kvstore::cache

namespace kvstore::engine {

// SSTable v1 (minimal) format spec.
//
// Goals:
// - Immutable sorted-string table, optimized for point lookups.
// - Block-level CRC32C checksums for integrity verification.
// - Simple index-at-end for fast lookup without scanning whole file.
//
// Layout (all integers little-endian):
//
//   [FileHeader (16 bytes)]
//   [DataBlockFrame 0]
//   [DataBlockFrame 1]
//   ...
//   [IndexBlockFrame]
//   [Footer (32 bytes)]
//
// FileHeader (16 bytes):
//   u32 magic            = 0x4b535354  // 'KSST'
//   u16 version          = 1
//   u16 reserved         = 0
//   u32 target_block_sz  // writer hint; reader may ignore
//   u32 reserved2        = 0
//
// Block framing (DataBlockFrame and IndexBlockFrame):
//   u32 payload_size
//   u8  payload[payload_size]
//   u32 crc32c
//
// Checksum coverage definition:
//   crc32c is computed over EXACTLY the payload bytes (and not the size field
//   nor the checksum field).
//
// Data block payload (sorted by key ascending):
//   repeated entries until payload end:
//     u32 key_size
//     u32 value_size
//     u8  key[key_size]
//     u8  value[value_size]
//
// Index block payload (one entry per data block):
//   u32 block_count
//   repeated block_count times:
//     u32 first_key_size
//     u8  first_key[first_key_size]
//     u64 block_offset        // file offset to that block frame
//     u32 block_frame_size    // total bytes of that block frame
//
// Footer (32 bytes):
//   u32 footer_magic     = 0x4b534654  // 'KSFT'
//   u16 footer_version   = 1
//   u16 reserved         = 0
//   u64 index_offset
//   u32 index_frame_size
//   u32 reserved2        = 0
//   u32 reserved3        = 0
//   u32 footer_crc32c    // crc32c over the first 28 bytes of the footer
//
// NOTE: This format is intentionally small and scoped to Task 4. It is NOT a
// full LevelDB/RocksDB compatible format (no bloom filters, no varints, etc.).

constexpr std::uint32_t kSstMagic = 0x4b535354U;
constexpr std::uint16_t kSstVersion = 1;
constexpr std::uint32_t kSstFooterMagic = 0x4b534654U;
constexpr std::uint16_t kSstFooterVersion = 1;
constexpr std::size_t kSstHeaderSize = 16;
constexpr std::size_t kSstFooterSize = 32;

struct SstWriteOptions {
  std::uint32_t target_block_size = 4096;
};

enum class SstEntryKind : std::uint8_t {
  kValue = 1,
  kTombstone = 2,
};

struct SstEntry {
  std::string key;
  std::string value;
  bool tombstone = false;
};

struct SstBlockIndexEntry {
  std::string first_key;
  std::uint64_t block_offset = 0;
  std::uint32_t block_frame_size = 0;
};

struct SstGetResult {
  bool found = false;
  bool tombstone = false;
  std::optional<std::string> value;
  std::optional<integrity::IntegrityError> error;
  [[nodiscard]] auto Ok() const -> bool { return !error.has_value(); }
};

class SstWriter {
 public:
  static auto Write(const std::filesystem::path& path,
                    const std::vector<std::pair<std::string, std::string>>& kvs,
                    const SstWriteOptions& options,
                    integrity::IntegrityError* error) -> bool;

  static auto Write(const std::filesystem::path& path,
                    const std::vector<SstEntry>& entries,
                    const SstWriteOptions& options,
                    integrity::IntegrityError* error) -> bool;
};

class SstReader {
 public:
  explicit SstReader(std::shared_ptr<cache::BlockCache> block_cache = nullptr);

  auto Open(const std::filesystem::path& path,
            integrity::IntegrityError* error) -> bool;

  [[nodiscard]] auto Get(const std::string& key) const -> SstGetResult;

  // Reads all key/value pairs from this table (in ascending key order).
  // This verifies each data block checksum.
  auto ScanAll(std::vector<std::pair<std::string, std::string>>* out,
               integrity::IntegrityError* error) const -> bool;

  auto ScanAllEntries(std::vector<SstEntry>* out,
                      integrity::IntegrityError* error) const -> bool;

  [[nodiscard]] auto path() const -> const std::filesystem::path& { return path_; }
  [[nodiscard]] auto index() const -> const std::vector<SstBlockIndexEntry>& {
    return index_;
  }

 private:
  [[nodiscard]] auto ReadAndVerifyBlockFrame(std::uint64_t offset,
                                             std::optional<std::uint32_t> expected_frame_size,
                                             std::vector<std::byte>* payload,
                                             integrity::IntegrityError* error) const -> bool;

  [[nodiscard]] auto FindBlockForKey(std::string_view key) const
      -> std::optional<std::size_t>;

  std::filesystem::path path_;
  std::uint32_t target_block_size_ = 0;
  std::vector<SstBlockIndexEntry> index_;
  std::shared_ptr<cache::BlockCache> block_cache_;
};

}  // namespace kvstore::engine

#endif  // KVSTORE_ENGINE_SSTABLE_H
