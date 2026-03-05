#include "kvstore/engine/kv_engine.h"

#include <utility>

namespace kvstore::engine {

KvEngine::KvEngine(std::filesystem::path wal_path) : wal_path_(std::move(wal_path)) {}

auto KvEngine::Open() -> bool {
  memtable_ = MemTable{};
  recovery_stats_ = WalReplayStats{};
  last_integrity_error_.reset();

  WalReplayResult replay_result;
  std::size_t duplicate_requests = 0;
  if (!WalReader::Replay(
      wal_path_,
      [this, &duplicate_requests](const WalRecord& record) {
        Mutation mutation;
        mutation.type = (record.operation == WalOperation::kPut) ? MutationType::kPut
                                                                  : MutationType::kDelete;
        mutation.key = record.key;
        mutation.value = record.value;
        mutation.request_id = record.request_id;

        const auto disposition = memtable_.Apply(mutation);
        if (disposition == ApplyDisposition::kDuplicate) {
          duplicate_requests += 1;
        }
      },
      &replay_result)) {
    last_integrity_error_ = replay_result.error;
    return false;
  }

  if (!replay_result.Ok()) {
    last_integrity_error_ = replay_result.error;
    return false;
  }

  recovery_stats_ = replay_result.stats;
  recovery_stats_.duplicate_requests = duplicate_requests;

  integrity::IntegrityError open_error;
  if (!wal_writer_.Open(wal_path_, &open_error)) {
    last_integrity_error_ = open_error;
    return false;
  }

  return true;
}

auto KvEngine::Put(std::string key, std::string value, std::string request_id)
    -> ApplyResult {
  Mutation mutation;
  mutation.type = MutationType::kPut;
  mutation.key = std::move(key);
  mutation.value = std::move(value);
  mutation.request_id = std::move(request_id);
  return ApplyMutation(mutation);
}

auto KvEngine::Delete(std::string key, std::string request_id) -> ApplyResult {
  Mutation mutation;
  mutation.type = MutationType::kDelete;
  mutation.key = std::move(key);
  mutation.request_id = std::move(request_id);
  return ApplyMutation(mutation);
}

auto KvEngine::Get(const std::string& key) const -> std::optional<std::string> {
  return memtable_.Get(key);
}

auto KvEngine::recovery_stats() const -> WalReplayStats {
  return recovery_stats_;
}

auto KvEngine::last_integrity_error() const
    -> std::optional<integrity::IntegrityError> {
  return last_integrity_error_;
}

auto KvEngine::ApplyMutation(const Mutation& mutation) -> ApplyResult {
  last_integrity_error_.reset();

  if (mutation.request_id.empty()) {
    const auto error = integrity::IntegrityError{
        .code = integrity::IntegrityErrorCode::kInvalidRecord,
        .record_index = 0,
        .message = "request_id must be non-empty for idempotent apply",
    };
    last_integrity_error_ = error;
    return ApplyResult{
        .applied = false,
        .duplicate = false,
        .error = error,
    };
  }

  if (mutation.key.size() > 1024U || mutation.value.size() > 1024U * 1024U ||
      mutation.request_id.size() > 4U * 1024U) {
    const auto error = integrity::IntegrityError{
        .code = integrity::IntegrityErrorCode::kInvalidRecord,
        .record_index = 0,
        .message = "mutation exceeds v1 key/value/request_id limits",
    };
    last_integrity_error_ = error;
    return ApplyResult{
        .applied = false,
        .duplicate = false,
        .error = error,
    };
  }

  if (memtable_.ContainsRequestId(mutation.request_id)) {
    return ApplyResult{
        .applied = false,
        .duplicate = true,
        .error = std::nullopt,
    };
  }

  WalRecord wal_record;
  wal_record.operation = (mutation.type == MutationType::kPut)
                             ? WalOperation::kPut
                             : WalOperation::kDelete;
  wal_record.key = mutation.key;
  wal_record.value = mutation.value;
  wal_record.request_id = mutation.request_id;

  integrity::IntegrityError append_error;
  if (!wal_writer_.Append(wal_record, &append_error)) {
    last_integrity_error_ = append_error;
    return ApplyResult{
        .applied = false,
        .duplicate = false,
        .error = append_error,
    };
  }

  const auto apply_result = memtable_.Apply(mutation);
  return ApplyResult{
      .applied = (apply_result == ApplyDisposition::kApplied),
      .duplicate = (apply_result == ApplyDisposition::kDuplicate),
      .error = std::nullopt,
  };
}

}  // namespace kvstore::engine
