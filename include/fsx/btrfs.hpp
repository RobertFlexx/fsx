#pragma once
#include "fsx/device.hpp"
#include "fsx/result.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace fsx::btrfs {

enum class ChecksumType : std::uint16_t { crc32c = 0, xxhash64 = 1, sha256 = 2, blake2b = 3 };

struct Superblock {
    std::uint64_t mirror_offset{};
    std::array<std::byte, 16> fsid{};
    std::array<std::byte, 16> metadata_uuid{};
    std::uint64_t bytenr{};
    std::uint64_t flags{};
    std::uint64_t generation{};
    std::uint64_t root{};
    std::uint64_t chunk_root{};
    std::uint64_t log_root{};
    std::uint64_t total_bytes{};
    std::uint64_t bytes_used{};
    std::uint64_t num_devices{};
    std::uint64_t device_id{};
    std::uint64_t device_total_bytes{};
    std::uint64_t device_bytes_used{};
    std::uint32_t sectorsize{};
    std::uint32_t nodesize{};
    std::uint32_t stripesize{};
    std::uint32_t sys_chunk_array_size{};
    std::uint64_t compat_flags{};
    std::uint64_t compat_ro_flags{};
    std::uint64_t incompat_flags{};
    ChecksumType checksum_type{ChecksumType::crc32c};
    std::uint8_t root_level{};
    std::uint8_t chunk_root_level{};
    std::uint8_t log_root_level{};
    std::string label;
    bool checksum_supported{false};
    bool checksum_valid{false};
};

struct CheckReport {
    bool clean{true};
    std::uint32_t mirrors_present{};
    std::uint32_t mirrors_valid{};
    std::uint64_t selected_generation{};
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

Result<bool> probe(BlockDevice& dev);
Result<Superblock> read_best_superblock(BlockDevice& dev);
Result<CheckReport> check(BlockDevice& dev);
std::string uuid_string(const std::array<std::byte, 16>& uuid);
const char* checksum_name(ChecksumType type) noexcept;

} // namespace fsx::btrfs
