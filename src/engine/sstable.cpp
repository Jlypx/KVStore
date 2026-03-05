#include "kvstore/engine/sstable.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>

#include "kvstore/cache/block_cache.h"
#include "kvstore/integrity/crc32c.h"

namespace kvstore::engine {
namespace {

auto MakeError(integrity::IntegrityErrorCode code, std::size_t record_index,
               std::string message) -> integrity::IntegrityError {
  return integrity::IntegrityError{.code = code,
                                  .record_index = record_index,
                                  .message = std::move(message)};
}

auto PutLe16(std::vector<std::byte>* bytes, std::uint16_t value) -> void {
  bytes->push_back(static_cast<std::byte>(value & 0xffU));
  bytes->push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
}

auto PutLe32(std::vector<std::byte>* bytes, std::uint32_t value) -> void {
  bytes->push_back(static_cast<std::byte>(value & 0xffU));
  bytes->push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
  bytes->push_back(static_cast<std::byte>((value >> 16U) & 0xffU));
  bytes->push_back(static_cast<std::byte>((value >> 24U) & 0xffU));
}

auto PutLe64(std::vector<std::byte>* bytes, std::uint64_t value) -> void {
  for (int i = 0; i < 8; ++i) {
    bytes->push_back(static_cast<std::byte>((value >> (8U * i)) & 0xffU));
  }
}

auto ReadLe16(const std::vector<std::byte>& bytes, std::size_t offset)
    -> std::uint16_t {
  return static_cast<std::uint16_t>(
      static_cast<std::uint8_t>(bytes[offset]) |
      (static_cast<std::uint16_t>(static_cast<std::uint8_t>(bytes[offset + 1])) <<
       8U));
}

auto ReadLe32(const std::vector<std::byte>& bytes, std::size_t offset)
    -> std::uint32_t {
  return static_cast<std::uint32_t>(
      static_cast<std::uint8_t>(bytes[offset]) |
      (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + 1])) <<
       8U) |
      (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + 2])) <<
       16U) |
      (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + 3])) <<
       24U));
}

auto ReadLe64(const std::vector<std::byte>& bytes, std::size_t offset)
    -> std::uint64_t {
  std::uint64_t out = 0;
  for (int i = 0; i < 8; ++i) {
    out |= (static_cast<std::uint64_t>(static_cast<std::uint8_t>(bytes[offset + i]))
            << (8U * i));
  }
  return out;
}

auto CopyBytesToString(const std::vector<std::byte>& bytes, std::size_t offset,
                       std::size_t size) -> std::string {
  std::string output;
  output.resize(size);
  for (std::size_t i = 0; i < size; ++i) {
    output[i] = static_cast<char>(static_cast<std::uint8_t>(bytes[offset + i]));
  }
  return output;
}

auto AppendString(std::vector<std::byte>* out, std::string_view s) -> void {
  out->reserve(out->size() + s.size());
  for (char ch : s) {
    out->push_back(static_cast<std::byte>(ch));
  }
}

auto WriteAll(std::ofstream* stream, const std::vector<std::byte>& bytes) -> bool {
  stream->write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
  return stream->good();
}

auto ReadExactly(std::ifstream* stream, std::vector<std::byte>* out,
                 std::size_t size) -> bool {
  out->assign(size, std::byte{0});
  stream->read(reinterpret_cast<char*>(out->data()),
               static_cast<std::streamsize>(size));
  return static_cast<std::size_t>(stream->gcount()) == size;
}

auto BuildBlockFrame(const std::vector<std::byte>& payload) -> std::vector<std::byte> {
  std::vector<std::byte> frame;
  frame.reserve(sizeof(std::uint32_t) + payload.size() + sizeof(std::uint32_t));
  PutLe32(&frame, static_cast<std::uint32_t>(payload.size()));
  frame.insert(frame.end(), payload.begin(), payload.end());
  const auto checksum = integrity::ComputeCrc32c(payload.data(), payload.size());
  PutLe32(&frame, checksum);
  return frame;
}

struct ParsedGet {
  bool ok = true;
  bool found = false;
  std::optional<std::string> value;
};

auto ParseDataBlockGet(std::string_view key, const std::vector<std::byte>& payload)
    -> ParsedGet {
  ParsedGet out;
  std::size_t offset = 0;
  while (offset + 8U <= payload.size()) {
    const auto key_size = static_cast<std::size_t>(ReadLe32(payload, offset));
    const auto value_size = static_cast<std::size_t>(ReadLe32(payload, offset + 4U));
    offset += 8U;
    if (key_size > payload.size() - offset) {
      out.ok = false;
      return out;
    }
    const auto key_str = std::string_view(
        reinterpret_cast<const char*>(payload.data() + offset), key_size);
    offset += key_size;
    if (value_size > payload.size() - offset) {
      out.ok = false;
      return out;
    }
    if (key_str == key) {
      out.found = true;
      out.value = CopyBytesToString(payload, offset, value_size);
      return out;
    }
    offset += value_size;
  }
  if (offset != payload.size()) {
    // Trailing bytes that don't contain a whole entry header.
    out.ok = false;
  }
  return out;
}

auto ParseDataBlockAll(const std::vector<std::byte>& payload,
                       std::vector<std::pair<std::string, std::string>>* out)
    -> bool {
  if (out == nullptr) {
    return true;
  }
  std::size_t offset = 0;
  while (offset + 8U <= payload.size()) {
    const auto key_size = static_cast<std::size_t>(ReadLe32(payload, offset));
    const auto value_size = static_cast<std::size_t>(ReadLe32(payload, offset + 4U));
    offset += 8U;
    if (key_size > payload.size() - offset) {
      return false;
    }
    const auto key = CopyBytesToString(payload, offset, key_size);
    offset += key_size;
    if (value_size > payload.size() - offset) {
      return false;
    }
    const auto value = CopyBytesToString(payload, offset, value_size);
    offset += value_size;
    out->emplace_back(key, value);
  }
  return offset == payload.size();
}

}  // namespace

auto SstWriter::Write(const std::filesystem::path& path,
                      const std::vector<std::pair<std::string, std::string>>& kvs,
                      const SstWriteOptions& options,
                      integrity::IntegrityError* error) -> bool {
  const auto parent = path.parent_path();
  if (!parent.empty()) {
    std::error_code mkdir_error;
    std::filesystem::create_directories(parent, mkdir_error);
    if (mkdir_error) {
      if (error != nullptr) {
        *error = MakeError(integrity::IntegrityErrorCode::kIoError, 0,
                           "failed to create SST parent directory: " +
                               mkdir_error.message());
      }
      return false;
    }
  }

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    if (error != nullptr) {
      *error = MakeError(integrity::IntegrityErrorCode::kIoError, 0,
                         "failed to open SST file for write");
    }
    return false;
  }

  // Header
  std::vector<std::byte> header;
  header.reserve(kSstHeaderSize);
  PutLe32(&header, kSstMagic);
  PutLe16(&header, kSstVersion);
  PutLe16(&header, 0);
  PutLe32(&header, options.target_block_size);
  PutLe32(&header, 0);
  if (!WriteAll(&out, header)) {
    if (error != nullptr) {
      *error = MakeError(integrity::IntegrityErrorCode::kIoError, 0,
                         "failed to write SST header");
    }
    return false;
  }

  // Build data blocks.
  std::vector<SstBlockIndexEntry> index;
  std::vector<std::byte> current_payload;
  std::string current_first_key;
  std::uint64_t current_offset = kSstHeaderSize;

  auto flush_block = [&](std::size_t block_index) -> bool {
    if (current_payload.empty()) {
      return true;
    }
    const auto frame = BuildBlockFrame(current_payload);
    const auto frame_size = static_cast<std::uint32_t>(frame.size());
    if (!WriteAll(&out, frame)) {
      if (error != nullptr) {
        *error = MakeError(integrity::IntegrityErrorCode::kIoError, block_index,
                           "failed to write SST data block frame");
      }
      return false;
    }
    index.push_back(SstBlockIndexEntry{
        .first_key = current_first_key,
        .block_offset = current_offset,
        .block_frame_size = frame_size,
    });
    current_offset += frame_size;
    current_payload.clear();
    current_first_key.clear();
    return true;
  };

  const std::size_t max_payload =
      std::max<std::uint32_t>(options.target_block_size, 128U);
  std::size_t block_index = 0;
  for (const auto& [key, value] : kvs) {
    // Entry encoding.
    std::vector<std::byte> entry;
    entry.reserve(8U + key.size() + value.size());
    if (key.size() > std::numeric_limits<std::uint32_t>::max() ||
        value.size() > std::numeric_limits<std::uint32_t>::max()) {
      if (error != nullptr) {
        *error = MakeError(integrity::IntegrityErrorCode::kInvalidRecord, 0,
                           "key/value exceeds u32 size limits");
      }
      return false;
    }
    PutLe32(&entry, static_cast<std::uint32_t>(key.size()));
    PutLe32(&entry, static_cast<std::uint32_t>(value.size()));
    AppendString(&entry, key);
    AppendString(&entry, value);

    if (current_payload.empty()) {
      current_first_key = key;
    }
    if (!current_payload.empty() && current_payload.size() + entry.size() > max_payload) {
      if (!flush_block(block_index)) {
        return false;
      }
      block_index += 1;
      current_first_key = key;
    }

    current_payload.insert(current_payload.end(), entry.begin(), entry.end());
  }
  if (!flush_block(block_index)) {
    return false;
  }

  // Index block
  const std::uint64_t index_offset = current_offset;
  std::vector<std::byte> index_payload;
  PutLe32(&index_payload, static_cast<std::uint32_t>(index.size()));
  for (const auto& entry : index) {
    PutLe32(&index_payload, static_cast<std::uint32_t>(entry.first_key.size()));
    AppendString(&index_payload, entry.first_key);
    PutLe64(&index_payload, entry.block_offset);
    PutLe32(&index_payload, entry.block_frame_size);
  }
  const auto index_frame = BuildBlockFrame(index_payload);
  const auto index_frame_size = static_cast<std::uint32_t>(index_frame.size());
  if (!WriteAll(&out, index_frame)) {
    if (error != nullptr) {
      *error = MakeError(integrity::IntegrityErrorCode::kIoError, 0,
                         "failed to write SST index block");
    }
    return false;
  }
  current_offset += index_frame_size;

  // Footer
  std::vector<std::byte> footer;
  footer.reserve(kSstFooterSize);
  PutLe32(&footer, kSstFooterMagic);
  PutLe16(&footer, kSstFooterVersion);
  PutLe16(&footer, 0);
  PutLe64(&footer, index_offset);
  PutLe32(&footer, index_frame_size);
  PutLe32(&footer, 0);
  PutLe32(&footer, 0);
  const auto footer_checksum =
      integrity::ComputeCrc32c(footer.data(), footer.size());
  PutLe32(&footer, footer_checksum);
  if (footer.size() != kSstFooterSize) {
    if (error != nullptr) {
      *error = MakeError(integrity::IntegrityErrorCode::kInvalidRecord, 0,
                         "internal error: SST footer size mismatch");
    }
    return false;
  }

  if (!WriteAll(&out, footer)) {
    if (error != nullptr) {
      *error = MakeError(integrity::IntegrityErrorCode::kIoError, 0,
                         "failed to write SST footer");
    }
    return false;
  }

  out.flush();
  if (!out.good()) {
    if (error != nullptr) {
      *error = MakeError(integrity::IntegrityErrorCode::kIoError, 0,
                         "failed to finalize SST file");
    }
    return false;
  }

  return true;
}

SstReader::SstReader(std::shared_ptr<cache::BlockCache> block_cache)
    : block_cache_(std::move(block_cache)) {}

auto SstReader::Open(const std::filesystem::path& path,
                     integrity::IntegrityError* error) -> bool {
  path_ = path;
  index_.clear();
  target_block_size_ = 0;

  std::ifstream input(path_, std::ios::binary);
  if (!input.is_open()) {
    if (error != nullptr) {
      *error = MakeError(integrity::IntegrityErrorCode::kIoError, 0,
                         "failed to open SST file");
    }
    return false;
  }

  std::vector<std::byte> header;
  if (!ReadExactly(&input, &header, kSstHeaderSize)) {
    if (error != nullptr) {
      *error = MakeError(integrity::IntegrityErrorCode::kTruncatedRecord, 0,
                         "truncated SST header");
    }
    return false;
  }
  const auto magic = ReadLe32(header, 0);
  const auto version = ReadLe16(header, 4);
  if (magic != kSstMagic) {
    if (error != nullptr) {
      *error = MakeError(integrity::IntegrityErrorCode::kInvalidMagic, 0,
                         "SST magic mismatch");
    }
    return false;
  }
  if (version != kSstVersion) {
    if (error != nullptr) {
      *error = MakeError(integrity::IntegrityErrorCode::kUnsupportedVersion, 0,
                         "unsupported SST version");
    }
    return false;
  }
  target_block_size_ = ReadLe32(header, 8);

  // Read footer from end.
  input.seekg(0, std::ios::end);
  const auto end_pos = input.tellg();
  if (end_pos < 0 || static_cast<std::uint64_t>(end_pos) < kSstHeaderSize + kSstFooterSize) {
    if (error != nullptr) {
      *error = MakeError(integrity::IntegrityErrorCode::kTruncatedRecord, 0,
                         "SST file too small to contain footer");
    }
    return false;
  }
  const auto footer_pos = static_cast<std::uint64_t>(end_pos) - kSstFooterSize;
  input.seekg(static_cast<std::streamoff>(footer_pos), std::ios::beg);
  std::vector<std::byte> footer;
  if (!ReadExactly(&input, &footer, kSstFooterSize)) {
    if (error != nullptr) {
      *error = MakeError(integrity::IntegrityErrorCode::kTruncatedRecord, 0,
                         "truncated SST footer");
    }
    return false;
  }

  const auto footer_magic = ReadLe32(footer, 0);
  const auto footer_version = ReadLe16(footer, 4);
  if (footer_magic != kSstFooterMagic) {
    if (error != nullptr) {
      *error = MakeError(integrity::IntegrityErrorCode::kInvalidMagic, 0,
                         "SST footer magic mismatch");
    }
    return false;
  }
  if (footer_version != kSstFooterVersion) {
    if (error != nullptr) {
      *error = MakeError(integrity::IntegrityErrorCode::kUnsupportedVersion, 0,
                         "unsupported SST footer version");
    }
    return false;
  }

  // Footer checksum verification: crc32c over first 28 bytes.
  const auto expected_footer_crc = ReadLe32(footer, 28);
  const auto computed_footer_crc =
      integrity::ComputeCrc32c(footer.data(), 28U);
  if (computed_footer_crc != expected_footer_crc) {
    if (error != nullptr) {
      std::ostringstream message;
      message << "SST footer checksum mismatch expected=" << expected_footer_crc
              << " actual=" << computed_footer_crc;
      *error = MakeError(integrity::IntegrityErrorCode::kChecksumMismatch, 0,
                         message.str());
    }
    return false;
  }

  const auto index_offset = ReadLe64(footer, 8);
  const auto index_frame_size = ReadLe32(footer, 16);
  if (index_offset < kSstHeaderSize || index_offset >= footer_pos) {
    if (error != nullptr) {
      *error = MakeError(integrity::IntegrityErrorCode::kInvalidRecord, 0,
                         "SST index offset out of range");
    }
    return false;
  }
  if (static_cast<std::uint64_t>(index_frame_size) > footer_pos - index_offset) {
    if (error != nullptr) {
      *error = MakeError(integrity::IntegrityErrorCode::kInvalidRecord, 0,
                         "SST index frame size out of range");
    }
    return false;
  }

  // Re-open to read index using normal helper.
  // (This keeps the reader stateless and thread-agnostic for now.)
  // We'll store index entries into index_.
  input.close();

  std::ifstream index_stream(path_, std::ios::binary);
  if (!index_stream.is_open()) {
    if (error != nullptr) {
      *error = MakeError(integrity::IntegrityErrorCode::kIoError, 0,
                         "failed to reopen SST for index read");
    }
    return false;
  }
  index_stream.seekg(static_cast<std::streamoff>(index_offset), std::ios::beg);
  std::vector<std::byte> index_frame;
  if (!ReadExactly(&index_stream, &index_frame, index_frame_size)) {
    if (error != nullptr) {
      *error = MakeError(integrity::IntegrityErrorCode::kTruncatedRecord, 0,
                         "truncated SST index frame");
    }
    return false;
  }

  if (index_frame_size < 8U) {
    if (error != nullptr) {
      *error = MakeError(integrity::IntegrityErrorCode::kInvalidRecord, 0,
                         "invalid SST index frame size");
    }
    return false;
  }
  const auto payload_size = ReadLe32(index_frame, 0);
  if (payload_size + 8U != index_frame.size()) {
    if (error != nullptr) {
      *error = MakeError(integrity::IntegrityErrorCode::kInvalidRecord, 0,
                         "SST index frame payload size mismatch");
    }
    return false;
  }
  const auto stored_crc = ReadLe32(index_frame, 4U + payload_size);
  std::vector<std::byte> payload(index_frame.begin() + 4,
                                 index_frame.begin() + 4 + payload_size);
  const auto computed_crc =
      integrity::ComputeCrc32c(payload.data(), payload.size());
  if (computed_crc != stored_crc) {
    if (error != nullptr) {
      std::ostringstream message;
      message << "SST index checksum mismatch expected=" << stored_crc
              << " actual=" << computed_crc;
      *error = MakeError(integrity::IntegrityErrorCode::kChecksumMismatch, 0,
                         message.str());
    }
    return false;
  }

  if (payload.size() < 4U) {
    if (error != nullptr) {
      *error = MakeError(integrity::IntegrityErrorCode::kInvalidRecord, 0,
                         "SST index payload too small");
    }
    return false;
  }
  const auto block_count = static_cast<std::size_t>(ReadLe32(payload, 0));
  std::size_t offset = 4U;
  index_.reserve(block_count);
  for (std::size_t i = 0; i < block_count; ++i) {
    if (offset + 4U > payload.size()) {
      if (error != nullptr) {
        *error = MakeError(integrity::IntegrityErrorCode::kInvalidRecord, i,
                           "truncated SST index entry");
      }
      return false;
    }
    const auto first_key_size = static_cast<std::size_t>(ReadLe32(payload, offset));
    offset += 4U;
    if (first_key_size > payload.size() - offset) {
      if (error != nullptr) {
        *error = MakeError(integrity::IntegrityErrorCode::kInvalidRecord, i,
                           "invalid SST index key size");
      }
      return false;
    }
    const auto first_key = CopyBytesToString(payload, offset, first_key_size);
    offset += first_key_size;
    if (offset + 12U > payload.size()) {
      if (error != nullptr) {
        *error = MakeError(integrity::IntegrityErrorCode::kInvalidRecord, i,
                           "truncated SST index entry fields");
      }
      return false;
    }
    const auto block_offset = ReadLe64(payload, offset);
    offset += 8U;
    const auto block_frame_size = ReadLe32(payload, offset);
    offset += 4U;
    index_.push_back(SstBlockIndexEntry{.first_key = first_key,
                                        .block_offset = block_offset,
                                        .block_frame_size = block_frame_size});
  }

  // Validate monotonic first_key ordering (writer guarantee).
  for (std::size_t i = 1; i < index_.size(); ++i) {
    if (!(index_[i - 1].first_key < index_[i].first_key)) {
      if (error != nullptr) {
        *error = MakeError(integrity::IntegrityErrorCode::kInvalidRecord, i,
                           "SST index first_key is not strictly increasing");
      }
      return false;
    }
  }

  return true;
}

auto SstReader::FindBlockForKey(std::string_view key) const
    -> std::optional<std::size_t> {
  if (index_.empty()) {
    return std::nullopt;
  }
  // Find the last index entry whose first_key <= key.
  std::size_t lo = 0;
  std::size_t hi = index_.size();
  while (lo + 1 < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (std::string_view(index_[mid].first_key) <= key) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  if (std::string_view(index_[lo].first_key) <= key) {
    return lo;
  }
  return std::nullopt;
}

auto SstReader::ReadAndVerifyBlockFrame(
    std::uint64_t offset, std::optional<std::uint32_t> expected_frame_size,
    std::vector<std::byte>* payload, integrity::IntegrityError* error) const
    -> bool {
  std::ifstream input(path_, std::ios::binary);
  if (!input.is_open()) {
    if (error != nullptr) {
      *error = MakeError(integrity::IntegrityErrorCode::kIoError, 0,
                         "failed to open SST for block read");
    }
    return false;
  }
  input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);

  std::vector<std::byte> size_bytes;
  if (!ReadExactly(&input, &size_bytes, 4U)) {
    if (error != nullptr) {
      *error = MakeError(integrity::IntegrityErrorCode::kTruncatedRecord, 0,
                         "truncated SST block size field");
    }
    return false;
  }
  const auto payload_size = ReadLe32(size_bytes, 0);
  const std::uint64_t frame_size = 4ULL + payload_size + 4ULL;
  if (expected_frame_size.has_value() &&
      frame_size != expected_frame_size.value()) {
    if (error != nullptr) {
      *error = MakeError(integrity::IntegrityErrorCode::kInvalidRecord, 0,
                         "SST block frame size mismatch");
    }
    return false;
  }
  std::vector<std::byte> local_payload;
  if (!ReadExactly(&input, &local_payload, payload_size)) {
    if (error != nullptr) {
      *error = MakeError(integrity::IntegrityErrorCode::kTruncatedRecord, 0,
                         "truncated SST block payload");
    }
    return false;
  }
  std::vector<std::byte> crc_bytes;
  if (!ReadExactly(&input, &crc_bytes, 4U)) {
    if (error != nullptr) {
      *error = MakeError(integrity::IntegrityErrorCode::kTruncatedRecord, 0,
                         "truncated SST block checksum field");
    }
    return false;
  }
  const auto stored_crc = ReadLe32(crc_bytes, 0);
  const auto computed_crc =
      integrity::ComputeCrc32c(local_payload.data(), local_payload.size());
  if (computed_crc != stored_crc) {
    if (error != nullptr) {
      std::ostringstream message;
      message << "SST block checksum mismatch expected=" << stored_crc
              << " actual=" << computed_crc;
      *error = MakeError(integrity::IntegrityErrorCode::kChecksumMismatch, 0,
                         message.str());
    }
    return false;
  }

  if (payload != nullptr) {
    *payload = std::move(local_payload);
  }
  return true;
}

auto SstReader::Get(const std::string& key) const -> SstGetResult {
  SstGetResult result;
  integrity::IntegrityError error;
  const auto block_index = FindBlockForKey(key);
  if (!block_index.has_value()) {
    result.found = false;
    return result;
  }
  const auto& entry = index_[block_index.value()];

  std::vector<std::byte> payload;
  bool have_payload = false;
  if (block_cache_ != nullptr) {
    cache::BlockCache::Key cache_key;
    cache_key.file_id = path_.string();
    cache_key.offset = entry.block_offset;
    cache_key.frame_size = entry.block_frame_size;
    if (block_cache_->Get(cache_key, &payload)) {
      have_payload = true;
    } else {
      if (!ReadAndVerifyBlockFrame(entry.block_offset, entry.block_frame_size,
                                   &payload, &error)) {
        error.record_index = block_index.value();
        std::ostringstream message;
        message << "sst=" << path_.string() << " " << error.message;
        error.message = message.str();
        result.error = error;
        return result;
      }
      block_cache_->Put(std::move(cache_key), payload);
      have_payload = true;
    }
  }

  if (!have_payload) {
    if (!ReadAndVerifyBlockFrame(entry.block_offset, entry.block_frame_size,
                                 &payload, &error)) {
      error.record_index = block_index.value();
      std::ostringstream message;
      message << "sst=" << path_.string() << " " << error.message;
      error.message = message.str();
      result.error = error;
      return result;
    }
  }

  const auto parsed = ParseDataBlockGet(key, payload);
  if (!parsed.ok) {
    result.error = MakeError(integrity::IntegrityErrorCode::kInvalidRecord,
                             block_index.value(),
                             "sst=" + path_.string() +
                                 " invalid data block payload encoding");
    return result;
  }
  if (!parsed.found) {
    result.found = false;
    return result;
  }
  result.found = true;
  result.value = parsed.value;
  return result;
}

auto SstReader::ScanAll(std::vector<std::pair<std::string, std::string>>* out,
                        integrity::IntegrityError* error) const -> bool {
  if (out != nullptr) {
    out->clear();
  }
  for (std::size_t i = 0; i < index_.size(); ++i) {
    const auto& entry = index_[i];
    std::vector<std::byte> payload;
    bool have_payload = false;

    if (block_cache_ != nullptr) {
      cache::BlockCache::Key cache_key;
      cache_key.file_id = path_.string();
      cache_key.offset = entry.block_offset;
      cache_key.frame_size = entry.block_frame_size;
      if (block_cache_->Get(cache_key, &payload)) {
        have_payload = true;
      } else {
        integrity::IntegrityError local_error;
        if (!ReadAndVerifyBlockFrame(entry.block_offset, entry.block_frame_size,
                                     &payload, &local_error)) {
          local_error.record_index = i;
          if (error != nullptr) {
            *error = local_error;
          }
          return false;
        }
        block_cache_->Put(std::move(cache_key), payload);
        have_payload = true;
      }
    }

    if (!have_payload) {
      integrity::IntegrityError local_error;
      if (!ReadAndVerifyBlockFrame(entry.block_offset, entry.block_frame_size,
                                   &payload, &local_error)) {
        local_error.record_index = i;
        if (error != nullptr) {
          *error = local_error;
        }
        return false;
      }
    }

    if (!ParseDataBlockAll(payload, out)) {
      if (error != nullptr) {
        *error = MakeError(integrity::IntegrityErrorCode::kInvalidRecord, i,
                           "sst=" + path_.string() +
                               " invalid data block payload encoding");
      }
      return false;
    }
  }
  return true;
}

}  // namespace kvstore::engine
