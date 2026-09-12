#pragma once
#include "fsx/device.hpp"
#include "fsx/result.hpp"
#include <cstdint>
#include <limits>
#include <string>
#include <vector>
namespace fsx::ntfs {
struct BootSector {
    std::uint16_t bytes_per_sector{};
    std::uint8_t sectors_per_cluster{};
    std::uint64_t total_sectors{};
    std::uint64_t mft_cluster{};
    std::uint64_t mft_mirror_cluster{};
    std::int8_t clusters_per_mft_record{};
    std::uint64_t serial{};
    std::uint64_t cluster_bytes() const noexcept {
        return static_cast<std::uint64_t>(bytes_per_sector)*sectors_per_cluster;
    }
    std::uint64_t size_bytes() const noexcept {
        if (bytes_per_sector == 0
            || total_sectors > std::numeric_limits<std::uint64_t>::max() / bytes_per_sector)
            return std::numeric_limits<std::uint64_t>::max();
        return total_sectors * static_cast<std::uint64_t>(bytes_per_sector);
    }
};
struct CheckReport { bool clean{true};
    std::vector<std::string>warnings;
    std::vector<std::string>errors;
};
Result<BootSector> read_boot_sector(BlockDevice& dev);
Result<CheckReport> check(BlockDevice& dev);
} // namespace fsx::ntfs
