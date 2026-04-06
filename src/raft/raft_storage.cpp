#include "kvstore/raft/raft_storage.h"

#include <cstdint>
#include <fstream>

namespace kvstore::raft {
namespace {

constexpr std::uint32_t kMetaMagic = 0x4b524d54U;
constexpr std::uint16_t kMetaVersion = 1;
constexpr std::uint32_t kLogMagic = 0x4b524c47U;
constexpr std::uint16_t kLogVersion = 1;

auto WriteU16(std::ofstream* out, std::uint16_t value) -> void {
  out->put(static_cast<char>(value & 0xffU));
  out->put(static_cast<char>((value >> 8U) & 0xffU));
}

auto WriteU32(std::ofstream* out, std::uint32_t value) -> void {
  out->put(static_cast<char>(value & 0xffU));
  out->put(static_cast<char>((value >> 8U) & 0xffU));
  out->put(static_cast<char>((value >> 16U) & 0xffU));
  out->put(static_cast<char>((value >> 24U) & 0xffU));
}

auto WriteU64(std::ofstream* out, std::uint64_t value) -> void {
  for (int i = 0; i < 8; ++i) {
    out->put(static_cast<char>((value >> (8U * i)) & 0xffU));
  }
}

auto ReadU16(std::ifstream* in, std::uint16_t* out) -> bool {
  unsigned char bytes[2];
  in->read(reinterpret_cast<char*>(bytes), 2);
  if (in->gcount() != 2) {
    return false;
  }
  *out = static_cast<std::uint16_t>(bytes[0]) |
         (static_cast<std::uint16_t>(bytes[1]) << 8U);
  return true;
}

auto ReadU32(std::ifstream* in, std::uint32_t* out) -> bool {
  unsigned char bytes[4];
  in->read(reinterpret_cast<char*>(bytes), 4);
  if (in->gcount() != 4) {
    return false;
  }
  *out = static_cast<std::uint32_t>(bytes[0]) |
         (static_cast<std::uint32_t>(bytes[1]) << 8U) |
         (static_cast<std::uint32_t>(bytes[2]) << 16U) |
         (static_cast<std::uint32_t>(bytes[3]) << 24U);
  return true;
}

auto ReadU64(std::ifstream* in, std::uint64_t* out) -> bool {
  unsigned char bytes[8];
  in->read(reinterpret_cast<char*>(bytes), 8);
  if (in->gcount() != 8) {
    return false;
  }
  *out = 0;
  for (int i = 0; i < 8; ++i) {
    *out |= static_cast<std::uint64_t>(bytes[i]) << (8U * i);
  }
  return true;
}

}  // namespace

RaftStorage::RaftStorage(std::filesystem::path dir) : dir_(std::move(dir)) {}

auto RaftStorage::EnsureDirectory() -> bool {
  std::error_code ec;
  std::filesystem::create_directories(dir_, ec);
  return !ec;
}

auto RaftStorage::meta_path() const -> std::filesystem::path {
  return dir_ / "meta.bin";
}

auto RaftStorage::log_path() const -> std::filesystem::path {
  return dir_ / "log.bin";
}

auto RaftStorage::Load(PersistentRaftState* out) -> bool {
  if (out == nullptr) {
    return false;
  }
  out->current_term = 0;
  out->voted_for = kNoVote;
  out->log.clear();

  if (!EnsureDirectory()) {
    return false;
  }

  if (std::filesystem::exists(meta_path())) {
    std::ifstream meta(meta_path(), std::ios::binary);
    if (!meta.is_open()) {
      return false;
    }
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t reserved = 0;
    std::uint64_t term = 0;
    std::uint32_t voted_for = 0;
    if (!ReadU32(&meta, &magic) || !ReadU16(&meta, &version) ||
        !ReadU16(&meta, &reserved) || !ReadU64(&meta, &term) ||
        !ReadU32(&meta, &voted_for)) {
      return false;
    }
    if (magic != kMetaMagic || version != kMetaVersion) {
      return false;
    }
    out->current_term = term;
    out->voted_for = static_cast<NodeId>(voted_for);
  }

  if (std::filesystem::exists(log_path())) {
    std::ifstream log(log_path(), std::ios::binary);
    if (!log.is_open()) {
      return false;
    }
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t reserved = 0;
    std::uint64_t count = 0;
    if (!ReadU32(&log, &magic) || !ReadU16(&log, &version) ||
        !ReadU16(&log, &reserved) || !ReadU64(&log, &count)) {
      return false;
    }
    if (magic != kLogMagic || version != kLogVersion) {
      return false;
    }
    out->log.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
      std::uint64_t term = 0;
      std::uint32_t size = 0;
      if (!ReadU64(&log, &term) || !ReadU32(&log, &size)) {
        return false;
      }
      std::string command(size, '\0');
      log.read(command.data(), static_cast<std::streamsize>(size));
      if (static_cast<std::uint32_t>(log.gcount()) != size) {
        return false;
      }
      out->log.push_back(LogEntry{.term = term, .command = std::move(command)});
    }
  }

  return true;
}

auto RaftStorage::StoreMetadata(Term term, NodeId voted_for) -> bool {
  if (!EnsureDirectory()) {
    return false;
  }
  std::ofstream meta(meta_path(), std::ios::binary | std::ios::trunc);
  if (!meta.is_open()) {
    return false;
  }
  WriteU32(&meta, kMetaMagic);
  WriteU16(&meta, kMetaVersion);
  WriteU16(&meta, 0);
  WriteU64(&meta, term);
  WriteU32(&meta, voted_for);
  meta.flush();
  return meta.good();
}

auto RaftStorage::StoreLog(const std::vector<LogEntry>& log_entries) -> bool {
  if (!EnsureDirectory()) {
    return false;
  }
  std::ofstream log(log_path(), std::ios::binary | std::ios::trunc);
  if (!log.is_open()) {
    return false;
  }
  WriteU32(&log, kLogMagic);
  WriteU16(&log, kLogVersion);
  WriteU16(&log, 0);
  WriteU64(&log, static_cast<std::uint64_t>(log_entries.size()));
  for (const auto& entry : log_entries) {
    WriteU64(&log, entry.term);
    WriteU32(&log, static_cast<std::uint32_t>(entry.command.size()));
    log.write(entry.command.data(),
              static_cast<std::streamsize>(entry.command.size()));
  }
  log.flush();
  return log.good();
}

}  // namespace kvstore::raft
