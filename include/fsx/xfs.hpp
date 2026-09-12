#pragma once
#include "fsx/device.hpp"
#include "fsx/result.hpp"
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace fsx::xfs {
struct Superblock {
    std::uint32_t block_size{};
    std::uint64_t data_blocks{};
    std::array<std::byte, 16> uuid{};
    std::uint32_t ag_blocks{};
    std::uint32_t ag_count{};
    std::uint32_t log_blocks{};
    std::uint16_t version{};
    std::uint16_t sector_size{};
    std::uint16_t inode_size{};
    std::uint16_t inodes_per_block{};
    std::string label;
    std::uint64_t size_bytes() const noexcept {
        if (block_size == 0 || data_blocks > std::numeric_limits<std::uint64_t>::max() / block_size)
            return std::numeric_limits<std::uint64_t>::max();
        return data_blocks * static_cast<std::uint64_t>(block_size);
    }
};
struct CheckReport {
    bool clean{true};
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};
Result<Superblock> read_superblock(BlockDevice& dev);
Result<CheckReport> check(BlockDevice& dev);
std::string uuid_string(const std::array<std::byte, 16>& uuid);
} // namespace fsx::xfs
