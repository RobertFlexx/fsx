#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace fsx {

std::uint64_t xxhash64(std::span<const std::byte> data, std::uint64_t seed = 0) noexcept;
std::array<std::byte, 32> sha256(std::span<const std::byte> data) noexcept;
std::array<std::byte, 32> blake2b_256(std::span<const std::byte> data) noexcept;

} // namespace fsx
