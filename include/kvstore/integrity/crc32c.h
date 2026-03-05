#ifndef KVSTORE_INTEGRITY_CRC32C_H
#define KVSTORE_INTEGRITY_CRC32C_H

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace kvstore::integrity {

auto ComputeCrc32c(const std::byte* data, std::size_t size) -> std::uint32_t;

auto ComputeCrc32c(std::string_view data) -> std::uint32_t;

}  // namespace kvstore::integrity

#endif  // KVSTORE_INTEGRITY_CRC32C_H
