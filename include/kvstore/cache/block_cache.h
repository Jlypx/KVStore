#ifndef KVSTORE_CACHE_BLOCK_CACHE_H
#define KVSTORE_CACHE_BLOCK_CACHE_H

#include <cstddef>
#include <cstdint>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kvstore::cache {

struct CacheCounters {
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t evicts = 0;
};

// Deterministic, single-threaded LRU block cache.
//
// - Capacity is measured in number of entries (not bytes) for deterministic
//   tests.
// - Counters are monotonic (only increment).
class BlockCache {
 public:
  struct Key {
    std::string file_id;
    std::uint64_t offset = 0;
    std::uint32_t frame_size = 0;

    auto operator==(const Key& other) const -> bool {
      return file_id == other.file_id && offset == other.offset &&
             frame_size == other.frame_size;
    }
  };

  explicit BlockCache(std::size_t capacity_entries);

  BlockCache(const BlockCache&) = delete;
  auto operator=(const BlockCache&) -> BlockCache& = delete;

  [[nodiscard]] auto capacity() const -> std::size_t { return capacity_entries_; }
  [[nodiscard]] auto size() const -> std::size_t { return entries_.size(); }
  [[nodiscard]] auto counters() const -> CacheCounters { return counters_; }

  // Returns cached payload on hit.
  auto Get(const Key& key, std::vector<std::byte>* payload_out) -> bool;

  // Inserts or updates cached payload.
  auto Put(Key key, std::vector<std::byte> payload) -> void;

 private:
  struct KeyHash {
    auto operator()(const Key& key) const -> std::size_t;
  };

  struct Entry {
    std::vector<std::byte> payload;
    std::list<Key>::iterator lru_it;
  };

  auto Touch(std::unordered_map<Key, Entry, KeyHash>::iterator it) -> void;
  auto EvictIfNeeded() -> void;

  std::size_t capacity_entries_ = 0;
  CacheCounters counters_;
  std::list<Key> lru_;  // front = most recently used
  std::unordered_map<Key, Entry, KeyHash> entries_;
};

}  // namespace kvstore::cache

#endif  // KVSTORE_CACHE_BLOCK_CACHE_H
