#include "kvstore/cache/block_cache.h"

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

auto Expect(bool condition, const std::string& message) -> bool {
  if (!condition) {
    std::cerr << "ASSERTION FAILED: " << message << '\n';
    return false;
  }
  return true;
}

auto Bytes(std::string_view s) -> std::vector<std::byte> {
  std::vector<std::byte> out;
  out.reserve(s.size());
  for (char ch : s) {
    out.push_back(static_cast<std::byte>(ch));
  }
  return out;
}

auto TestLruEvictionAndCounters() -> bool {
  kvstore::cache::BlockCache cache(/*capacity_entries=*/2);

  kvstore::cache::BlockCache::Key k1{.file_id = "f", .offset = 1, .frame_size = 10};
  kvstore::cache::BlockCache::Key k2{.file_id = "f", .offset = 2, .frame_size = 10};
  kvstore::cache::BlockCache::Key k3{.file_id = "f", .offset = 3, .frame_size = 10};

  std::vector<std::byte> payload;
  if (!Expect(!cache.Get(k1, &payload), "first get should miss") ||
      !Expect(cache.counters().misses == 1, "miss counter should increment")) {
    return false;
  }

  cache.Put(k1, Bytes("one"));
  if (!Expect(cache.Get(k1, &payload), "get after put should hit") ||
      !Expect(cache.counters().hits == 1, "hit counter should increment") ||
      !Expect(cache.size() == 1, "cache size should be 1")) {
    return false;
  }

  cache.Put(k2, Bytes("two"));
  // Touch k1 so k2 becomes LRU.
  if (!Expect(cache.Get(k1, &payload), "touch k1 should hit") ||
      !Expect(cache.counters().hits == 2, "hit counter monotonic")) {
    return false;
  }

  cache.Put(k3, Bytes("three"));
  if (!Expect(cache.size() == 2, "cache size should remain at capacity") ||
      !Expect(cache.counters().evicts == 1, "evict counter should increment")) {
    return false;
  }

  // k2 should have been evicted.
  if (!Expect(!cache.Get(k2, &payload), "evicted entry should miss") ||
      !Expect(cache.counters().misses == 2, "miss counter monotonic") ||
      !Expect(cache.Get(k1, &payload), "k1 should still be cached") ||
      !Expect(cache.Get(k3, &payload), "k3 should be cached") ||
      !Expect(cache.counters().hits >= 4, "hit counter should not decrease")) {
    return false;
  }

  return true;
}

}  // namespace

int main() {
  if (!TestLruEvictionAndCounters()) {
    return 1;
  }
  return 0;
}
