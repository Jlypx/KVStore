#include "kvstore/integrity/crc32c.h"

#include <array>

namespace kvstore::integrity {
namespace {

constexpr std::uint32_t kCrc32cPolynomial = 0x82f63b78;

constexpr auto BuildTable() -> std::array<std::uint32_t, 256> {
  std::array<std::uint32_t, 256> table{};
  for (std::size_t i = 0; i < table.size(); ++i) {
    std::uint32_t value = static_cast<std::uint32_t>(i);
    for (std::uint32_t j = 0; j < 8; ++j) {
      if ((value & 1U) != 0U) {
        value = kCrc32cPolynomial ^ (value >> 1U);
      } else {
        value >>= 1U;
      }
    }
    table[i] = value;
  }
  return table;
}

constexpr auto kCrcTable = BuildTable();

}  // namespace

auto ComputeCrc32c(const std::byte* data, std::size_t size) -> std::uint32_t {
  std::uint32_t crc = 0xffffffffU;
  for (std::size_t index = 0; index < size; ++index) {
    const auto byte_value = static_cast<std::uint8_t>(data[index]);
    const auto table_index = static_cast<std::uint8_t>((crc ^ byte_value) & 0xffU);
    crc = kCrcTable[table_index] ^ (crc >> 8U);
  }
  return crc ^ 0xffffffffU;
}

auto ComputeCrc32c(std::string_view data) -> std::uint32_t {
  const auto* bytes = reinterpret_cast<const std::byte*>(data.data());
  return ComputeCrc32c(bytes, data.size());
}

}  // namespace kvstore::integrity
