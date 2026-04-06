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
      kv_[mutation.key] =
          ValueEntry{.tombstone = false, .value = mutation.value};
      break;
    case MutationType::kDelete:
      kv_[mutation.key] = ValueEntry{.tombstone = true, .value = ""};
      break;
  }

  return ApplyDisposition::kApplied;
}

auto MemTable::Get(const std::string& key) const -> LookupResult {
  const auto it = kv_.find(key);
  if (it == kv_.end()) {
    return LookupResult{};
  }
  if (it->second.tombstone) {
    return LookupResult{.state = LookupState::kTombstone, .value = ""};
  }
  return LookupResult{.state = LookupState::kValue, .value = it->second.value};
}

auto MemTable::ContainsRequestId(const std::string& request_id) const -> bool {
  return applied_request_ids_.contains(request_id);
}

auto MemTable::EntryCount() const -> std::size_t {
  return kv_.size();
}

auto MemTable::SortedEntries() const
    -> std::vector<MemTableEntrySnapshot> {
  std::vector<MemTableEntrySnapshot> entries;
  entries.reserve(kv_.size());
  for (const auto& [key, value] : kv_) {
    entries.push_back(
        MemTableEntrySnapshot{.key = key,
                              .tombstone = value.tombstone,
                              .value = value.value});
  }
  std::sort(entries.begin(), entries.end(),
            [](const auto& a, const auto& b) { return a.key < b.key; });
  return entries;
}

auto MemTable::ClearKvs() -> void { kv_.clear(); }

}  // namespace kvstore::engine
