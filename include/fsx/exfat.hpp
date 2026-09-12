#pragma once
#include "fsx/device.hpp"
#include "fsx/result.hpp"
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace fsx::exfat {

struct Volume {
    std::uint64_t partition_offset{};
    std::uint64_t volume_length{};
    std::uint32_t fat_offset{};
    std::uint32_t fat_length{};
    std::uint32_t cluster_heap_offset{};
    std::uint32_t cluster_count{};
    std::uint32_t root_dir_cluster{};
    std::uint32_t serial{};
    std::uint16_t revision{};
    std::uint16_t flags{};
    std::uint32_t bytes_per_sector{};
    std::uint32_t sectors_per_cluster{};
    std::uint8_t fat_count{};
    std::uint8_t percent_in_use{};
    bool main_boot_checksum_ok{false};
    bool backup_boot_checksum_ok{false};
    bool boot_regions_match{false};
    bool allocation_bitmap_seen{false};
    bool upcase_table_seen{false};
    std::uint32_t allocation_bitmap_cluster{};
    std::uint64_t allocation_bitmap_length{};
    std::uint32_t upcase_cluster{};
    std::uint64_t upcase_length{};
    std::uint32_t upcase_checksum{};
    std::uint32_t root_fat_entry{};
    std::string label;

    std::uint64_t size_bytes() const noexcept {
        if (bytes_per_sector == 0
            || volume_length > std::numeric_limits<std::uint64_t>::max() / bytes_per_sector)
            return std::numeric_limits<std::uint64_t>::max();
        return volume_length * bytes_per_sector;
    }
};

struct CheckReport {
    bool clean{true};
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

Result<bool> probe(BlockDevice& dev);
Result<Volume> read_volume(BlockDevice& dev);
Result<CheckReport> check(BlockDevice& dev);

} // namespace fsx::exfat
