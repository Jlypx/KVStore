#include "kvstore/engine/compaction.h"

#include <algorithm>
#include <unordered_map>

#include "kvstore/cache/block_cache.h"

namespace kvstore::engine {

auto Compactor::CompactToSingleSstable(
    const std::vector<std::filesystem::path>& inputs_oldest_to_newest,
    const std::filesystem::path& output, const SstWriteOptions& options,
    std::shared_ptr<cache::BlockCache> block_cache,
    integrity::IntegrityError* error) -> bool {
  // Newest wins: iterate newest->oldest and take first occurrence per key.
  std::unordered_map<std::string, SstEntry> chosen;
  chosen.reserve(1024);

  for (std::size_t i = inputs_oldest_to_newest.size(); i > 0; --i) {
    const auto& input_path = inputs_oldest_to_newest[i - 1];
    SstReader reader(block_cache);
    integrity::IntegrityError open_error;
    if (!reader.Open(input_path, &open_error)) {
      if (error != nullptr) {
        *error = open_error;
      }
      return false;
    }
    std::vector<SstEntry> kvs;
    integrity::IntegrityError scan_error;
    if (!reader.ScanAllEntries(&kvs, &scan_error)) {
      if (error != nullptr) {
        *error = scan_error;
      }
      return false;
    }
    for (auto& entry : kvs) {
      if (chosen.find(entry.key) == chosen.end()) {
        chosen.emplace(entry.key, std::move(entry));
      }
    }
  }

  std::vector<SstEntry> out_entries;
  out_entries.reserve(chosen.size());
  for (auto& [key, entry] : chosen) {
    out_entries.push_back(std::move(entry));
  }
  std::sort(out_entries.begin(), out_entries.end(),
            [](const auto& a, const auto& b) { return a.key < b.key; });

  return SstWriter::Write(output, out_entries, options, error);
}

}  // namespace kvstore::engine
