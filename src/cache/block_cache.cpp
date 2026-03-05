#include "kvstore/cache/block_cache.h"

#include <cstddef>
#include <functional>

namespace kvstore::cache {

BlockCache::BlockCache(std::size_t capacity_entries)
    : capacity_entries_(capacity_entries) {}

auto BlockCache::KeyHash::operator()(const Key& key) const -> std::size_t {
  std::size_t h = 0;
  const std::hash<std::string> hash_str;
  const std::hash<std::uint64_t> hash_u64;
  const std::hash<std::uint32_t> hash_u32;
  h ^= hash_str(key.file_id) + 0x9e3779b97f4a7c15ULL + (h << 6U) + (h >> 2U);
  h ^= hash_u64(key.offset) + 0x9e3779b97f4a7c15ULL + (h << 6U) + (h >> 2U);
  h ^= hash_u32(key.frame_size) + 0x9e3779b97f4a7c15ULL + (h << 6U) + (h >> 2U);
  return h;
}

auto BlockCache::Touch(
    std::unordered_map<Key, Entry, KeyHash>::iterator it) -> void {
  lru_.splice(lru_.begin(), lru_, it->second.lru_it);
  it->second.lru_it = lru_.begin();
}

auto BlockCache::EvictIfNeeded() -> void {
  if (capacity_entries_ == 0) {
    // No caching.
    entries_.clear();
    lru_.clear();
    return;
  }
  while (entries_.size() > capacity_entries_) {
    const auto& lru_key = lru_.back();
    auto it = entries_.find(lru_key);
    if (it != entries_.end()) {
      entries_.erase(it);
      counters_.evicts += 1;
    }
    lru_.pop_back();
  }
}

auto BlockCache::Get(const Key& key, std::vector<std::byte>* payload_out) -> bool {
  auto it = entries_.find(key);
  if (it == entries_.end()) {
    counters_.misses += 1;
    return false;
  }
  counters_.hits += 1;
  Touch(it);
  if (payload_out != nullptr) {
    *payload_out = it->second.payload;
  }
  return true;
}

auto BlockCache::Put(Key key, std::vector<std::byte> payload) -> void {
  if (capacity_entries_ == 0) {
    return;
  }
  auto it = entries_.find(key);
  if (it != entries_.end()) {
    it->second.payload = std::move(payload);
    Touch(it);
    return;
  }
  lru_.push_front(key);
  Entry entry;
  entry.payload = std::move(payload);
  entry.lru_it = lru_.begin();
  entries_.emplace(std::move(key), std::move(entry));
  EvictIfNeeded();
}

}  // namespace kvstore::cache
