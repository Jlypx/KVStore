#ifndef KVSTORE_ENGINE_MEMTABLE_H
#define KVSTORE_ENGINE_MEMTABLE_H

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace kvstore::engine {

enum class MutationType {
  kPut,
  kDelete,
};

struct Mutation {
  MutationType type = MutationType::kPut;
  std::string key;
  std::string value;
  std::string request_id;
};

enum class ApplyDisposition {
  kApplied,
  kDuplicate,
};

enum class LookupState {
  kMissing,
  kValue,
  kTombstone,
};

struct LookupResult {
  LookupState state = LookupState::kMissing;
  std::string value;
};

struct ValueEntry {
  bool tombstone = false;
  std::string value;
};

struct MemTableEntrySnapshot {
  std::string key;
  bool tombstone = false;
  std::string value;
};

class MemTable {
 public:
  auto Apply(const Mutation& mutation) -> ApplyDisposition;

  [[nodiscard]] auto Get(const std::string& key) const
      -> LookupResult;

  [[nodiscard]] auto ContainsRequestId(const std::string& request_id) const
      -> bool;

  [[nodiscard]] auto EntryCount() const -> std::size_t;

  // Snapshot current key/value state for SSTable flush.
  // The returned vector is sorted by key in ascending order.
  [[nodiscard]] auto SortedEntries() const
      -> std::vector<MemTableEntrySnapshot>;

  // Clears only the key/value map, keeping request-id history for idempotency.
  auto ClearKvs() -> void;

  [[nodiscard]] auto SnapshotRequestIds() const -> std::vector<std::string>;
  auto RestoreSnapshotState(std::vector<MemTableEntrySnapshot> entries,
                            std::vector<std::string> request_ids) -> void;

 private:
  std::unordered_map<std::string, ValueEntry> kv_;
  std::unordered_set<std::string> applied_request_ids_;
};

}  // namespace kvstore::engine

#endif  // KVSTORE_ENGINE_MEMTABLE_H
