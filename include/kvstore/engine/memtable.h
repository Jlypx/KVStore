#ifndef KVSTORE_ENGINE_MEMTABLE_H
#define KVSTORE_ENGINE_MEMTABLE_H

#include <cstddef>
#include <optional>
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

class MemTable {
 public:
  auto Apply(const Mutation& mutation) -> ApplyDisposition;

  [[nodiscard]] auto Get(const std::string& key) const
      -> std::optional<std::string>;

  [[nodiscard]] auto ContainsRequestId(const std::string& request_id) const
      -> bool;

  [[nodiscard]] auto EntryCount() const -> std::size_t;

 private:
  std::unordered_map<std::string, std::string> kv_;
  std::unordered_set<std::string> applied_request_ids_;
};

}  // namespace kvstore::engine

#endif  // KVSTORE_ENGINE_MEMTABLE_H
