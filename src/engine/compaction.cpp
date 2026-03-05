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
  std::unordered_map<std::string, std::string> chosen;
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
    std::vector<std::pair<std::string, std::string>> kvs;
    integrity::IntegrityError scan_error;
    if (!reader.ScanAll(&kvs, &scan_error)) {
      if (error != nullptr) {
        *error = scan_error;
      }
      return false;
    }
    for (auto& [key, value] : kvs) {
      if (chosen.find(key) == chosen.end()) {
        chosen.emplace(std::move(key), std::move(value));
      }
    }
  }

  std::vector<std::pair<std::string, std::string>> out_entries;
  out_entries.reserve(chosen.size());
  for (auto& [key, value] : chosen) {
    out_entries.emplace_back(std::move(key), std::move(value));
  }
  std::sort(out_entries.begin(), out_entries.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

  return SstWriter::Write(output, out_entries, options, error);
}

}  // namespace kvstore::engine
