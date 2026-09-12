#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace fsx {
std::uint32_t crc32_ieee(std::span<const std::byte> data, std::uint32_t seed = 0xffffffffU);
std::uint32_t crc32c(std::span<const std::byte> data, std::uint32_t seed = 0xffffffffU);
}
