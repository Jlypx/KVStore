#include "kvstore/engine/kv_engine.h"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace kvstore::engine {
namespace {

auto ParseSstIdFromFilename(const std::filesystem::path& path)
    -> std::optional<std::uint64_t> {
  if (path.extension() != ".sst") {
    return std::nullopt;
  }
  const auto stem = path.stem().string();
  if (stem.empty()) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  for (char ch : stem) {
    if (!std::isdigit(static_cast<unsigned char>(ch))) {
      return std::nullopt;
    }
    const auto digit = static_cast<std::uint64_t>(ch - '0');
    value = value * 10ULL + digit;
  }
  return value;
}

auto FormatSstFilename(std::uint64_t id) -> std::string {
  // 6 digits (matches WAL naming style in tests), but allow growth beyond.
  std::string out = std::to_string(id);
  if (out.size() < 6) {
    out = std::string(6 - out.size(), '0') + out;
  }
  return out + ".sst";
}

auto ParseWalGenerationFromFilename(const std::filesystem::path& path)
    -> std::optional<std::uint64_t> {
  if (path.extension() != ".wal") {
    return std::nullopt;
  }
  const auto stem = path.stem().string();
  if (stem.empty()) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  for (char ch : stem) {
    if (!std::isdigit(static_cast<unsigned char>(ch))) {
      return std::nullopt;
    }
    value = value * 10ULL + static_cast<std::uint64_t>(ch - '0');
  }
  return value;
}

auto FormatWalFilename(std::uint64_t generation) -> std::string {
  std::string out = std::to_string(generation);
  if (out.size() < 6) {
    out = std::string(6 - out.size(), '0') + out;
  }
  return out + ".wal";
}

constexpr std::uint32_t kSnapshotMagic = 0x4b565353U;
constexpr std::uint16_t kSnapshotVersion = 1;

auto WriteSnapshotU16(std::string* out, std::uint16_t value) -> void {
  out->push_back(static_cast<char>(value & 0xffU));
  out->push_back(static_cast<char>((value >> 8U) & 0xffU));
}

auto WriteSnapshotU32(std::string* out, std::uint32_t value) -> void {
  out->push_back(static_cast<char>(value & 0xffU));
  out->push_back(static_cast<char>((value >> 8U) & 0xffU));
  out->push_back(static_cast<char>((value >> 16U) & 0xffU));
  out->push_back(static_cast<char>((value >> 24U) & 0xffU));
}

auto ReadSnapshotU16(std::string_view in, std::size_t* offset, std::uint16_t* out)
    -> bool {
  if (*offset + 2 > in.size()) {
    return false;
  }
  const unsigned char* p =
      reinterpret_cast<const unsigned char*>(in.data() + *offset);
  *out = static_cast<std::uint16_t>(p[0]) |
         (static_cast<std::uint16_t>(p[1]) << 8U);
  *offset += 2;
  return true;
}

auto ReadSnapshotU32(std::string_view in, std::size_t* offset, std::uint32_t* out)
    -> bool {
  if (*offset + 4 > in.size()) {
    return false;
  }
  const unsigned char* p =
      reinterpret_cast<const unsigned char*>(in.data() + *offset);
  *out = static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8U) |
         (static_cast<std::uint32_t>(p[2]) << 16U) |
         (static_cast<std::uint32_t>(p[3]) << 24U);
  *offset += 4;
  return true;
}

struct SnapshotValue {
  bool tombstone = false;
  std::string value;
};

}  // namespace

KvEngine::KvEngine(std::filesystem::path wal_path) : wal_path_(std::move(wal_path)) {}

auto KvEngine::Open() -> bool {
  memtable_ = MemTable{};
  recovery_stats_ = WalReplayStats{};
  last_integrity_error_.reset();

  sst_dir_ = wal_path_.parent_path() / "sst";
  block_cache_ = std::make_shared<cache::BlockCache>(64);
  // Recovery order matters: load immutable SST state first, then replay every
  // WAL generation on top so memtable_ represents the newest in-memory delta.
  if (!LoadSstables()) {
    return false;
  }

  WalReplayResult replay_result;
  std::size_t duplicate_requests = 0;
  const auto wal_segments = DiscoverWalSegments(wal_path_);
  for (const auto& wal_segment : wal_segments) {
    WalReplayResult segment_result;
    if (!WalReader::Replay(
            wal_segment,
            [this, &duplicate_requests](const WalRecord& record) {
              Mutation mutation;
              mutation.type = (record.operation == WalOperation::kPut)
                                  ? MutationType::kPut
                                  : MutationType::kDelete;
              mutation.key = record.key;
              mutation.value = record.value;
              mutation.request_id = record.request_id;

              const auto disposition = memtable_.Apply(mutation);
              if (disposition == ApplyDisposition::kDuplicate) {
                duplicate_requests += 1;
              }
            },
            &segment_result)) {
      last_integrity_error_ = segment_result.error;
      return false;
    }

    if (!segment_result.Ok()) {
      last_integrity_error_ = segment_result.error;
      return false;
    }
    replay_result.stats.records_replayed += segment_result.stats.records_replayed;
  }

  recovery_stats_ = replay_result.stats;
  recovery_stats_.duplicate_requests = duplicate_requests;

  // Re-open the newest WAL segment for appends. Older generations stay
  // read-only history until a flush/snapshot cycle removes them.
  const auto active_wal_path = wal_segments.empty() ? wal_path_ : wal_segments.back();
  next_wal_generation_ =
      ParseWalGenerationFromFilename(active_wal_path).value_or(0) + 1;
  integrity::IntegrityError open_error;
  if (!wal_writer_.Open(active_wal_path, &open_error)) {
    last_integrity_error_ = open_error;
    return false;
  }
  wal_path_ = active_wal_path;

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

auto KvEngine::Get(const std::string& key) -> std::optional<std::string> {
  last_integrity_error_.reset();

  const auto mem_value = memtable_.Get(key);
  if (mem_value.state == LookupState::kValue) {
    return mem_value.value;
  }
  if (mem_value.state == LookupState::kTombstone) {
    return std::nullopt;
  }

  // Newest-to-oldest search.
  for (std::size_t i = sstables_.size(); i > 0; --i) {
    const auto& sst = sstables_[i - 1];
    const auto res = sst.reader.Get(key);
    if (!res.Ok()) {
      last_integrity_error_ = res.error;
      return std::nullopt;
    }
    if (res.found) {
      if (res.tombstone) {
        return std::nullopt;
      }
      return res.value;
    }
  }
  return std::nullopt;
}

auto KvEngine::Flush() -> bool {
  last_integrity_error_.reset();
  const auto entries = memtable_.SortedEntries();
  if (entries.empty()) {
    return true;
  }

  // Flush persists the current memtable as an immutable SST, then rotates WAL
  // generation so subsequent writes only describe mutations after this flush.
  std::vector<SstEntry> sst_entries;
  sst_entries.reserve(entries.size());
  for (const auto& entry : entries) {
    sst_entries.push_back(
        SstEntry{.key = entry.key, .value = entry.value, .tombstone = entry.tombstone});
  }

  integrity::IntegrityError error;
  const auto path = NextSstPath();
  const SstWriteOptions options{
      .target_block_size = 4096,
  };
  if (!SstWriter::Write(path, sst_entries, options, &error)) {
    last_integrity_error_ = error;
    return false;
  }

  SstReader reader(block_cache_);
  if (!reader.Open(path, &error)) {
    last_integrity_error_ = error;
    return false;
  }

  const auto next_wal_path = NextWalPath();
  integrity::IntegrityError wal_open_error;
  if (!wal_writer_.Open(next_wal_path, &wal_open_error)) {
    last_integrity_error_ = wal_open_error;
    return false;
  }

  sstables_.push_back(SstableFile{.path = path, .reader = std::move(reader)});
  memtable_.ClearKvs();
  wal_path_ = next_wal_path;
  return true;
}

auto KvEngine::Compact() -> bool {
  last_integrity_error_.reset();

  // Ensure MemTable state is persisted before compacting.
  if (!Flush()) {
    return false;
  }
  if (sstables_.size() <= 1) {
    return true;
  }

  std::vector<std::filesystem::path> inputs;
  inputs.reserve(sstables_.size());
  for (const auto& sst : sstables_) {
    inputs.push_back(sst.path);
  }

  // Compaction rewrites the visible state into one SST. Tombstones are kept or
  // dropped by the compactor according to the merged newest value for each key.
  const auto output_path = NextSstPath();
  const SstWriteOptions options{
      .target_block_size = 4096,
  };
  integrity::IntegrityError compact_error;
  if (!Compactor::CompactToSingleSstable(inputs, output_path, options,
                                        block_cache_, &compact_error)) {
    last_integrity_error_ = compact_error;
    return false;
  }

  integrity::IntegrityError open_error;
  SstReader output_reader(block_cache_);
  if (!output_reader.Open(output_path, &open_error)) {
    last_integrity_error_ = open_error;
    return false;
  }

  std::error_code ec;
  for (const auto& input : inputs) {
    std::filesystem::remove(input, ec);
    if (ec) {
      last_integrity_error_ = integrity::IntegrityError{
          .code = integrity::IntegrityErrorCode::kIoError,
          .record_index = 0,
          .message = "failed to remove compacted SST: " + ec.message(),
      };
      return false;
    }
  }

  sstables_.clear();
  sstables_.push_back(
      SstableFile{.path = output_path, .reader = std::move(output_reader)});
  return true;
}

auto KvEngine::ExportSnapshotPayload(std::string* payload) -> bool {
  if (payload == nullptr) {
    return false;
  }
  last_integrity_error_.reset();

  std::unordered_map<std::string, SnapshotValue> merged;

  // Merge oldest-to-newest SSTs first, then overlay the mutable memtable so the
  // snapshot encodes the same key visibility a live Get() would observe.
  for (const auto& sstable : sstables_) {
    std::vector<SstEntry> entries;
    integrity::IntegrityError scan_error;
    if (!sstable.reader.ScanAllEntries(&entries, &scan_error)) {
      last_integrity_error_ = scan_error;
      return false;
    }
    for (const auto& entry : entries) {
      merged[entry.key] = SnapshotValue{.tombstone = entry.tombstone,
                                        .value = entry.value};
    }
  }

  for (const auto& entry : memtable_.SortedEntries()) {
    merged[entry.key] = SnapshotValue{.tombstone = entry.tombstone,
                                      .value = entry.value};
  }

  std::vector<std::pair<std::string, std::string>> live_entries;
  live_entries.reserve(merged.size());
  for (const auto& [key, value] : merged) {
    if (!value.tombstone) {
      live_entries.emplace_back(key, value.value);
    }
  }
  std::sort(live_entries.begin(), live_entries.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

  // Snapshot payload carries live KV pairs plus request_id history. The latter
  // preserves idempotency guarantees after a follower installs the snapshot.
  const auto request_ids = memtable_.SnapshotRequestIds();
  std::string encoded;
  encoded.reserve(16);
  WriteSnapshotU32(&encoded, kSnapshotMagic);
  WriteSnapshotU16(&encoded, kSnapshotVersion);
  WriteSnapshotU32(&encoded, static_cast<std::uint32_t>(live_entries.size()));
  for (const auto& [key, value] : live_entries) {
    WriteSnapshotU32(&encoded, static_cast<std::uint32_t>(key.size()));
    WriteSnapshotU32(&encoded, static_cast<std::uint32_t>(value.size()));
    encoded.append(key);
    encoded.append(value);
  }
  WriteSnapshotU32(&encoded, static_cast<std::uint32_t>(request_ids.size()));
  for (const auto& request_id : request_ids) {
    WriteSnapshotU32(&encoded, static_cast<std::uint32_t>(request_id.size()));
    encoded.append(request_id);
  }

  *payload = std::move(encoded);
  return true;
}

auto KvEngine::InstallSnapshotPayload(std::string_view payload) -> bool {
  last_integrity_error_.reset();

  std::size_t offset = 0;
  std::uint32_t magic = 0;
  std::uint16_t version = 0;
  std::uint32_t kv_count = 0;
  if (!ReadSnapshotU32(payload, &offset, &magic) ||
      !ReadSnapshotU16(payload, &offset, &version) ||
      !ReadSnapshotU32(payload, &offset, &kv_count)) {
    return false;
  }
  if (magic != kSnapshotMagic || version != kSnapshotVersion) {
    return false;
  }

  std::vector<SstEntry> snapshot_entries;
  snapshot_entries.reserve(kv_count);
  for (std::uint32_t i = 0; i < kv_count; ++i) {
    std::uint32_t key_size = 0;
    std::uint32_t value_size = 0;
    if (!ReadSnapshotU32(payload, &offset, &key_size) ||
        !ReadSnapshotU32(payload, &offset, &value_size) ||
        offset + key_size + value_size > payload.size()) {
      return false;
    }
    std::string key(payload.substr(offset, key_size));
    offset += key_size;
    std::string value(payload.substr(offset, value_size));
    offset += value_size;
    snapshot_entries.push_back(
        SstEntry{.key = std::move(key), .value = std::move(value), .tombstone = false});
  }

  std::uint32_t request_id_count = 0;
  if (!ReadSnapshotU32(payload, &offset, &request_id_count)) {
    return false;
  }
  std::vector<std::string> request_ids;
  request_ids.reserve(request_id_count);
  for (std::uint32_t i = 0; i < request_id_count; ++i) {
    std::uint32_t size = 0;
    if (!ReadSnapshotU32(payload, &offset, &size) || offset + size > payload.size()) {
      return false;
    }
    request_ids.emplace_back(payload.substr(offset, size));
    offset += size;
  }
  if (offset != payload.size()) {
    return false;
  }

  // Snapshot install is a full local state replacement: discard existing WAL
  // generations and SSTs, then rebuild storage from the transferred snapshot.
  std::error_code ec;
  for (const auto& segment : DiscoverWalSegments(wal_path_)) {
    std::filesystem::remove(segment, ec);
    if (ec) {
      return false;
    }
  }
  for (const auto& entry : std::filesystem::directory_iterator(sst_dir_, ec)) {
    if (ec) {
      return false;
    }
    if (entry.is_regular_file() && entry.path().extension() == ".sst") {
      std::filesystem::remove(entry.path(), ec);
      if (ec) {
        return false;
      }
    }
  }

  memtable_.RestoreSnapshotState({}, request_ids);
  sstables_.clear();
  next_sst_id_ = 1;
  next_wal_generation_ = 1;
  wal_path_ = wal_path_.parent_path() / FormatWalFilename(1);

  if (!snapshot_entries.empty()) {
    integrity::IntegrityError write_error;
    const auto snapshot_sst = NextSstPath();
    if (!SstWriter::Write(snapshot_sst, snapshot_entries,
                          SstWriteOptions{.target_block_size = 4096},
                          &write_error)) {
      last_integrity_error_ = write_error;
      return false;
    }
  }

  if (!LoadSstables()) {
    return false;
  }

  integrity::IntegrityError open_error;
  if (!wal_writer_.Open(wal_path_, &open_error)) {
    last_integrity_error_ = open_error;
    return false;
  }

  return true;
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

  // The write becomes visible in memory only after the WAL append succeeds, so
  // crash recovery never misses an acknowledged mutation.
  const auto apply_result = memtable_.Apply(mutation);
  return ApplyResult{
      .applied = (apply_result == ApplyDisposition::kApplied),
      .duplicate = (apply_result == ApplyDisposition::kDuplicate),
      .error = std::nullopt,
  };
}

auto KvEngine::LoadSstables() -> bool {
  sstables_.clear();
  next_sst_id_ = 1;

  std::error_code ec;
  std::filesystem::create_directories(sst_dir_, ec);
  if (ec) {
    last_integrity_error_ = integrity::IntegrityError{
        .code = integrity::IntegrityErrorCode::kIoError,
        .record_index = 0,
        .message = "failed to create SST directory: " + ec.message(),
    };
    return false;
  }

  std::vector<std::pair<std::uint64_t, std::filesystem::path>> sst_paths;
  for (const auto& entry : std::filesystem::directory_iterator(sst_dir_, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file()) {
      continue;
    }
    const auto id = ParseSstIdFromFilename(entry.path());
    if (!id.has_value()) {
      continue;
    }
    sst_paths.emplace_back(id.value(), entry.path());
  }
  if (ec) {
    last_integrity_error_ = integrity::IntegrityError{
        .code = integrity::IntegrityErrorCode::kIoError,
        .record_index = 0,
        .message = "failed to list SST directory: " + ec.message(),
    };
    return false;
  }

  std::sort(sst_paths.begin(), sst_paths.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

  std::uint64_t max_id = 0;
  for (const auto& [id, path] : sst_paths) {
    integrity::IntegrityError open_error;
    SstReader reader(block_cache_);
    if (!reader.Open(path, &open_error)) {
      last_integrity_error_ = open_error;
      return false;
    }
    sstables_.push_back(SstableFile{.path = path, .reader = std::move(reader)});
    max_id = std::max(max_id, id);
  }
  // Readers search sstables_ from back to front, so keeping this vector ordered
  // by creation id naturally makes newer tables shadow older ones.
  next_sst_id_ = max_id + 1;
  return true;
}

auto KvEngine::NextSstPath() -> std::filesystem::path {
  const auto id = next_sst_id_;
  next_sst_id_ += 1;
  return sst_dir_ / FormatSstFilename(id);
}

auto KvEngine::NextWalPath() -> std::filesystem::path {
  const auto generation = next_wal_generation_;
  next_wal_generation_ += 1;
  return wal_path_.parent_path() / FormatWalFilename(generation);
}

}  // namespace kvstore::engine
