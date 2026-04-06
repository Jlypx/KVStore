#include "kvstore/engine/wal.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "kvstore/integrity/crc32c.h"

namespace kvstore::engine {
namespace {

constexpr std::size_t kHeaderWithoutChecksumSize = kWalHeaderSize - sizeof(std::uint32_t);

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

auto IsSupportedOperation(std::uint8_t value) -> bool {
  return value == static_cast<std::uint8_t>(WalOperation::kPut) ||
         value == static_cast<std::uint8_t>(WalOperation::kDelete);
}

auto BuildPayload(const WalRecord& record) -> std::vector<std::byte> {
  std::vector<std::byte> payload;
  payload.reserve(record.key.size() + record.value.size() +
                  record.request_id.size());

  for (char ch : record.key) {
    payload.push_back(static_cast<std::byte>(ch));
  }
  for (char ch : record.value) {
    payload.push_back(static_cast<std::byte>(ch));
  }
  for (char ch : record.request_id) {
    payload.push_back(static_cast<std::byte>(ch));
  }

  return payload;
}

auto BuildHeaderWithoutChecksum(const WalRecord& record) -> std::vector<std::byte> {
  std::vector<std::byte> header;
  header.reserve(kHeaderWithoutChecksumSize);

  PutLe32(&header, kWalMagic);
  PutLe16(&header, kWalVersion);
  header.push_back(static_cast<std::byte>(record.operation));
  header.push_back(std::byte{0});
  PutLe32(&header, static_cast<std::uint32_t>(record.key.size()));
  PutLe32(&header, static_cast<std::uint32_t>(record.value.size()));
  PutLe32(&header, static_cast<std::uint32_t>(record.request_id.size()));

  return header;
}

auto BuildSerializedRecord(const WalRecord& record) -> std::vector<std::byte> {
  const auto header_without_checksum = BuildHeaderWithoutChecksum(record);
  const auto payload = BuildPayload(record);

  std::vector<std::byte> checksum_input;
  checksum_input.reserve(header_without_checksum.size() + payload.size());
  checksum_input.insert(checksum_input.end(), header_without_checksum.begin(),
                        header_without_checksum.end());
  checksum_input.insert(checksum_input.end(), payload.begin(), payload.end());

  const auto checksum =
      integrity::ComputeCrc32c(checksum_input.data(), checksum_input.size());

  std::vector<std::byte> encoded;
  encoded.reserve(kWalHeaderSize + payload.size());
  encoded.insert(encoded.end(), header_without_checksum.begin(),
                 header_without_checksum.end());
  PutLe32(&encoded, checksum);
  encoded.insert(encoded.end(), payload.begin(), payload.end());

  return encoded;
}

auto CopyBytesToString(const std::vector<std::byte>& bytes, std::size_t offset,
                       std::size_t size) -> std::string {
  std::string output;
  output.resize(size);
  for (std::size_t index = 0; index < size; ++index) {
    output[index] =
        static_cast<char>(static_cast<std::uint8_t>(bytes[offset + index]));
  }
  return output;
}

auto MakeError(integrity::IntegrityErrorCode code, std::size_t record_index,
               std::string message) -> integrity::IntegrityError {
  return integrity::IntegrityError{
      .code = code,
      .record_index = record_index,
      .message = std::move(message),
  };
}

auto ParseWalGeneration(const std::filesystem::path& wal_path)
    -> std::optional<std::uint64_t> {
  if (wal_path.extension() != ".wal") {
    return std::nullopt;
  }
  const auto stem = wal_path.stem().string();
  if (stem.empty()) {
    return std::nullopt;
  }

  std::uint64_t generation = 0;
  for (char ch : stem) {
    if (ch < '0' || ch > '9') {
      return std::nullopt;
    }
    generation = generation * 10ULL + static_cast<std::uint64_t>(ch - '0');
  }
  return generation;
}

auto SyncWalPath(const std::filesystem::path& wal_path,
                 integrity::IntegrityError* error) -> bool {
#ifdef _WIN32
  int fd = -1;
  errno_t open_result =
      _wsopen_s(&fd, wal_path.c_str(), _O_RDONLY | _O_BINARY, _SH_DENYNO, 0);
  if (open_result != 0 || fd < 0) {
    if (error != nullptr) {
      *error = MakeError(integrity::IntegrityErrorCode::kIoError, 0,
                         "failed to open WAL file for sync");
    }
    return false;
  }
  const int sync_result = _commit(fd);
  _close(fd);
  if (sync_result != 0) {
    if (error != nullptr) {
      *error = MakeError(integrity::IntegrityErrorCode::kIoError, 0,
                         "failed to sync WAL file");
    }
    return false;
  }
  return true;
#else
  const int fd = ::open(wal_path.c_str(), O_RDONLY);
  if (fd < 0) {
    if (error != nullptr) {
      *error = MakeError(integrity::IntegrityErrorCode::kIoError, 0,
                         "failed to open WAL file for sync");
    }
    return false;
  }
  const int sync_result = ::fsync(fd);
  ::close(fd);
  if (sync_result != 0) {
    if (error != nullptr) {
      *error = MakeError(integrity::IntegrityErrorCode::kIoError, 0,
                         "failed to sync WAL file");
    }
    return false;
  }
  return true;
#endif
}

}  // namespace

auto DiscoverWalSegments(const std::filesystem::path& wal_path)
    -> std::vector<std::filesystem::path> {
  std::vector<std::pair<std::uint64_t, std::filesystem::path>> ordered;
  std::error_code ec;
  const auto wal_dir = wal_path.parent_path();
  if (!wal_dir.empty()) {
    for (const auto& entry : std::filesystem::directory_iterator(wal_dir, ec)) {
      if (ec) {
        break;
      }
      if (!entry.is_regular_file()) {
        continue;
      }
      const auto generation = ParseWalGeneration(entry.path());
      if (!generation.has_value()) {
        continue;
      }
      ordered.emplace_back(generation.value(), entry.path());
    }
  }

  if (ordered.empty()) {
    ordered.emplace_back(ParseWalGeneration(wal_path).value_or(1), wal_path);
  }

  std::sort(ordered.begin(), ordered.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

  std::vector<std::filesystem::path> segments;
  segments.reserve(ordered.size());
  for (const auto& [generation, path] : ordered) {
    static_cast<void>(generation);
    segments.push_back(path);
  }
  return segments;
}

auto WalWriter::Open(const std::filesystem::path& wal_path,
                     integrity::IntegrityError* error) -> bool {
  wal_path_ = wal_path;
  if (stream_.is_open()) {
    stream_.close();
    stream_.clear();
  }
  const auto parent = wal_path_.parent_path();
  if (!parent.empty()) {
    std::error_code mkdir_error;
    std::filesystem::create_directories(parent, mkdir_error);
    if (mkdir_error) {
      if (error != nullptr) {
        *error = MakeError(
            integrity::IntegrityErrorCode::kIoError, 0,
            "failed to create WAL parent directory: " + mkdir_error.message());
      }
      return false;
    }
  }

  stream_.open(wal_path_, std::ios::binary | std::ios::app);
  if (!stream_.is_open()) {
    if (error != nullptr) {
      *error = MakeError(integrity::IntegrityErrorCode::kIoError, 0,
                         "failed to open WAL file for append");
    }
    return false;
  }

  return true;
}

auto WalWriter::Append(const WalRecord& record, integrity::IntegrityError* error)
    -> bool {
  if (!stream_.is_open()) {
    if (error != nullptr) {
      *error = MakeError(integrity::IntegrityErrorCode::kIoError, 0,
                         "WAL writer is not open");
    }
    return false;
  }

  const auto encoded = BuildSerializedRecord(record);
  stream_.write(reinterpret_cast<const char*>(encoded.data()),
                static_cast<std::streamsize>(encoded.size()));
  stream_.flush();

  if (!stream_.good()) {
    if (error != nullptr) {
      *error = MakeError(integrity::IntegrityErrorCode::kIoError, 0,
                         "failed to append record to WAL");
    }
    return false;
  }

  if (!SyncWalPath(wal_path_, error)) {
    return false;
  }

  return true;
}

auto WalReader::Replay(const std::filesystem::path& wal_path,
                       const RecordCallback& callback, WalReplayResult* result)
    -> bool {
  WalReplayResult local_result;
  WalReplayResult* out = result != nullptr ? result : &local_result;
  out->stats = WalReplayStats{};
  out->error.reset();

  if (!std::filesystem::exists(wal_path)) {
    return true;
  }

  std::ifstream input(wal_path, std::ios::binary);
  if (!input.is_open()) {
    out->error = MakeError(integrity::IntegrityErrorCode::kIoError, 0,
                           "failed to open WAL file for replay");
    return false;
  }

  std::size_t record_index = 0;
  while (true) {
    std::vector<std::byte> header(kWalHeaderSize);
    input.read(reinterpret_cast<char*>(header.data()),
               static_cast<std::streamsize>(header.size()));
    const auto bytes_read = static_cast<std::size_t>(input.gcount());

    if (bytes_read == 0) {
      return true;
    }

    if (bytes_read != kWalHeaderSize) {
      out->error = MakeError(
          integrity::IntegrityErrorCode::kTruncatedRecord, record_index,
          "truncated WAL header while reading record");
      return false;
    }

    const auto magic = ReadLe32(header, 0);
    if (magic != kWalMagic) {
      out->error = MakeError(integrity::IntegrityErrorCode::kInvalidMagic,
                             record_index, "WAL magic mismatch");
      return false;
    }

    const auto version = ReadLe16(header, 4);
    if (version != kWalVersion) {
      out->error = MakeError(integrity::IntegrityErrorCode::kUnsupportedVersion,
                             record_index, "unsupported WAL version");
      return false;
    }

    const auto operation = static_cast<std::uint8_t>(header[6]);
    if (!IsSupportedOperation(operation)) {
      out->error =
          MakeError(integrity::IntegrityErrorCode::kInvalidOperation,
                    record_index, "unknown WAL operation tag");
      return false;
    }

    const auto key_size = static_cast<std::size_t>(ReadLe32(header, 8));
    const auto value_size = static_cast<std::size_t>(ReadLe32(header, 12));
    const auto request_id_size = static_cast<std::size_t>(ReadLe32(header, 16));
    const auto stored_checksum = ReadLe32(header, 20);

    if (key_size > 1024U || value_size > 1024U * 1024U ||
        request_id_size > 4U * 1024U) {
      out->error = MakeError(integrity::IntegrityErrorCode::kInvalidRecord,
                             record_index,
                             "record sizes exceed v1 contract limits");
      return false;
    }

    if (key_size > std::numeric_limits<std::size_t>::max() - value_size ||
        key_size + value_size >
            std::numeric_limits<std::size_t>::max() - request_id_size) {
      out->error = MakeError(integrity::IntegrityErrorCode::kInvalidRecord,
                             record_index,
                             "record size overflows host size_t limits");
      return false;
    }

    const auto payload_size = key_size + value_size + request_id_size;
    std::vector<std::byte> payload(payload_size);
    input.read(reinterpret_cast<char*>(payload.data()),
               static_cast<std::streamsize>(payload_size));
    if (static_cast<std::size_t>(input.gcount()) != payload_size) {
      out->error =
          MakeError(integrity::IntegrityErrorCode::kTruncatedRecord,
                    record_index, "truncated WAL payload while replaying");
      return false;
    }

    std::vector<std::byte> checksum_input;
    checksum_input.reserve(kHeaderWithoutChecksumSize + payload.size());
    checksum_input.insert(
        checksum_input.end(), header.begin(),
        header.begin() +
            static_cast<std::vector<std::byte>::difference_type>(
                kHeaderWithoutChecksumSize));
    checksum_input.insert(checksum_input.end(), payload.begin(), payload.end());

    const auto computed_checksum =
        integrity::ComputeCrc32c(checksum_input.data(), checksum_input.size());
    if (computed_checksum != stored_checksum) {
      std::ostringstream message;
      message << "WAL checksum mismatch expected=" << stored_checksum
              << " actual=" << computed_checksum;
      out->error = MakeError(integrity::IntegrityErrorCode::kChecksumMismatch,
                             record_index, message.str());
      return false;
    }

    WalRecord record;
    record.operation = static_cast<WalOperation>(operation);
    record.key = CopyBytesToString(payload, 0, key_size);
    record.value = CopyBytesToString(payload, key_size, value_size);
    record.request_id =
        CopyBytesToString(payload, key_size + value_size, request_id_size);

    callback(record);
    out->stats.records_replayed += 1;
    record_index += 1;
  }
}

}  // namespace kvstore::engine
