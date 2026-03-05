#include "kvstore/engine/memtable.h"

#include <algorithm>

namespace kvstore::engine {

auto MemTable::Apply(const Mutation& mutation) -> ApplyDisposition {
  if (applied_request_ids_.contains(mutation.request_id)) {
    return ApplyDisposition::kDuplicate;
  }

  applied_request_ids_.insert(mutation.request_id);
  switch (mutation.type) {
    case MutationType::kPut:
      kv_[mutation.key] = mutation.value;
      break;
    case MutationType::kDelete:
      kv_.erase(mutation.key);
      break;
  }

  return ApplyDisposition::kApplied;
}

auto MemTable::Get(const std::string& key) const -> std::optional<std::string> {
  const auto it = kv_.find(key);
  if (it == kv_.end()) {
    return std::nullopt;
  }
  return it->second;
}

auto MemTable::ContainsRequestId(const std::string& request_id) const -> bool {
  return applied_request_ids_.contains(request_id);
}

auto MemTable::EntryCount() const -> std::size_t {
  return kv_.size();
}

auto MemTable::SortedEntries() const
    -> std::vector<std::pair<std::string, std::string>> {
  std::vector<std::pair<std::string, std::string>> entries;
  entries.reserve(kv_.size());
  for (const auto& [key, value] : kv_) {
    entries.emplace_back(key, value);
  }
  std::sort(entries.begin(), entries.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  return entries;
}

auto MemTable::ClearKvs() -> void { kv_.clear(); }

}  // namespace kvstore::engine
