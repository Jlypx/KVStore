#include "kvstore/engine/wal.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "kvstore/integrity/crc32c.h"
#include "kvstore/integrity/integrity_error.h"

namespace {

auto MakeTempDirectory(const std::string& suffix) -> std::filesystem::path {
  const auto path = std::filesystem::temp_directory_path() /
                    ("kvstore_task3_" + suffix + "_test");
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
  std::filesystem::create_directories(path, ec);
  return path;
}

auto ReadLe16(const std::array<std::uint8_t, kvstore::engine::kWalHeaderSize>& data,
              std::size_t offset) -> std::uint16_t {
  return static_cast<std::uint16_t>(data[offset] |
                                    (static_cast<std::uint16_t>(data[offset + 1]) <<
                                     8U));
}

auto ReadLe32(const std::array<std::uint8_t, kvstore::engine::kWalHeaderSize>& data,
              std::size_t offset) -> std::uint32_t {
  return static_cast<std::uint32_t>(
      data[offset] |
      (static_cast<std::uint32_t>(data[offset + 1]) << 8U) |
      (static_cast<std::uint32_t>(data[offset + 2]) << 16U) |
      (static_cast<std::uint32_t>(data[offset + 3]) << 24U));
}

auto Expect(bool condition, const std::string& message) -> bool {
  if (!condition) {
    std::cerr << "ASSERTION FAILED: " << message << '\n';
    return false;
  }
  return true;
}

auto CorruptByte(const std::filesystem::path& file, std::streamoff offset) -> bool {
  std::fstream stream(file, std::ios::binary | std::ios::in | std::ios::out);
  if (!stream.is_open()) {
    return false;
  }

  stream.seekg(offset, std::ios::beg);
  char value = 0;
  stream.read(&value, 1);
  if (stream.gcount() != 1) {
    return false;
  }

  value = static_cast<char>(value ^ 0x1);
  stream.seekp(offset, std::ios::beg);
  stream.write(&value, 1);
  stream.flush();
  return stream.good();
}

auto TestWalRoundTripAndHeader() -> bool {
  const auto dir = MakeTempDirectory("wal_roundtrip");
  const auto wal = dir / "000001.wal";

  kvstore::engine::WalWriter writer;
  kvstore::integrity::IntegrityError error;
  if (!Expect(writer.Open(wal, &error), "writer open should succeed")) {
    return false;
  }

  kvstore::engine::WalRecord put;
  put.operation = kvstore::engine::WalOperation::kPut;
  put.key = "alpha";
  put.value = "one";
  put.request_id = "rid-1";

  kvstore::engine::WalRecord del;
  del.operation = kvstore::engine::WalOperation::kDelete;
  del.key = "alpha";
  del.request_id = "rid-2";

  if (!Expect(writer.Append(put, &error), "append put should succeed") ||
      !Expect(writer.Append(del, &error), "append delete should succeed")) {
    return false;
  }

  std::vector<kvstore::engine::WalRecord> replayed;
  kvstore::engine::WalReplayResult replay_result;
  const bool replay_ok = kvstore::engine::WalReader::Replay(
      wal, [&replayed](const kvstore::engine::WalRecord& record) {
        replayed.push_back(record);
      },
      &replay_result);

  if (!Expect(replay_ok, "wal replay should succeed") ||
      !Expect(replay_result.Ok(), "replay result should be ok") ||
      !Expect(replay_result.stats.records_replayed == 2,
              "replayed record count should be 2") ||
      !Expect(replayed.size() == 2, "replayed vector size should be 2") ||
      !Expect(replayed[0].key == "alpha", "first key should be alpha") ||
      !Expect(replayed[0].value == "one", "first value should be one") ||
      !Expect(replayed[1].operation == kvstore::engine::WalOperation::kDelete,
              "second operation should be delete")) {
    return false;
  }

  std::ifstream input(wal, std::ios::binary);
  std::array<std::uint8_t, kvstore::engine::kWalHeaderSize> header{};
  input.read(reinterpret_cast<char*>(header.data()),
             static_cast<std::streamsize>(header.size()));
  if (!Expect(input.gcount() ==
                  static_cast<std::streamsize>(kvstore::engine::kWalHeaderSize),
              "first header should be readable")) {
    return false;
  }

  const auto magic = ReadLe32(header, 0);
  const auto version = ReadLe16(header, 4);
  const auto operation = header[6];
  const auto key_size = ReadLe32(header, 8);
  const auto value_size = ReadLe32(header, 12);
  const auto request_id_size = ReadLe32(header, 16);
  const auto checksum = ReadLe32(header, 20);

  if (!Expect(magic == kvstore::engine::kWalMagic, "magic should match") ||
      !Expect(version == kvstore::engine::kWalVersion, "version should match") ||
      !Expect(operation ==
                  static_cast<std::uint8_t>(kvstore::engine::WalOperation::kPut),
              "operation should be put") ||
      !Expect(key_size == 5, "key size should be 5") ||
      !Expect(value_size == 3, "value size should be 3") ||
      !Expect(request_id_size == 5, "request_id size should be 5")) {
    return false;
  }

  std::vector<std::byte> payload(key_size + value_size + request_id_size);
  input.read(reinterpret_cast<char*>(payload.data()),
             static_cast<std::streamsize>(payload.size()));
  if (!Expect(static_cast<std::size_t>(input.gcount()) == payload.size(),
              "payload should be readable")) {
    return false;
  }

  std::vector<std::byte> checksum_input;
  checksum_input.reserve(20U + payload.size());
  for (std::size_t i = 0; i < 20U; ++i) {
    checksum_input.push_back(static_cast<std::byte>(header[i]));
  }
  checksum_input.insert(checksum_input.end(), payload.begin(), payload.end());

  const auto computed =
      kvstore::integrity::ComputeCrc32c(checksum_input.data(), checksum_input.size());
  return Expect(computed == checksum, "header checksum must match payload");
}

auto TestWalChecksumCorruptionDetection() -> bool {
  const auto dir = MakeTempDirectory("wal_corruption");
  const auto wal = dir / "000001.wal";

  kvstore::engine::WalWriter writer;
  kvstore::integrity::IntegrityError error;
  if (!Expect(writer.Open(wal, &error), "writer open for corruption test")) {
    return false;
  }

  kvstore::engine::WalRecord record;
  record.operation = kvstore::engine::WalOperation::kPut;
  record.key = "k";
  record.value = "v";
  record.request_id = "rid-corrupt";
  if (!Expect(writer.Append(record, &error), "append before corruption")) {
    return false;
  }

  if (!Expect(CorruptByte(wal, static_cast<std::streamoff>(kvstore::engine::kWalHeaderSize)),
              "file byte corruption should succeed")) {
    return false;
  }

  kvstore::engine::WalReplayResult replay_result;
  const bool replay_ok = kvstore::engine::WalReader::Replay(
      wal, [](const kvstore::engine::WalRecord&) {}, &replay_result);

  return Expect(!replay_ok, "replay should fail on corruption") &&
         Expect(replay_result.error.has_value(), "integrity error expected") &&
         Expect(replay_result.error->code ==
                    kvstore::integrity::IntegrityErrorCode::kChecksumMismatch,
                "checksum mismatch error expected");
}

}  // namespace

int main() {
  if (!TestWalRoundTripAndHeader()) {
    return 1;
  }
  if (!TestWalChecksumCorruptionDetection()) {
    return 1;
  }
  return 0;
}
