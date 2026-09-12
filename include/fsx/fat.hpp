#pragma once
#include "fsx/device.hpp"
#include "fsx/progress.hpp"
#include "fsx/result.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace fsx::fat {

enum class Kind { fat12, fat16, fat32 };

struct Volume {
    Kind kind{Kind::fat12};
    std::uint16_t bytes_per_sector{};
    std::uint8_t sectors_per_cluster{};
    std::uint16_t reserved_sectors{};
    std::uint8_t fat_count{};
    std::uint32_t sectors_per_fat{};
    std::uint32_t total_sectors{};
    std::uint32_t root_dir_sectors{};
    std::uint32_t first_data_sector{};
    std::uint32_t data_sectors{};
    std::uint32_t cluster_count{};
    std::uint32_t root_cluster{};
    std::uint8_t media{};
    std::uint32_t serial{};
    std::string label;

    std::uint64_t size_bytes() const noexcept {
        return static_cast<std::uint64_t>(total_sectors) * bytes_per_sector;
    }
    std::uint32_t cluster_bytes() const noexcept {
        return static_cast<std::uint32_t>(bytes_per_sector) * sectors_per_cluster;
    }
};

struct CheckReport {
    bool clean{true};
    std::uint64_t fat_bytes_compared{};
    std::uint64_t mirrored_fat_mismatches{};
    std::uint64_t invalid_cluster_entries{};
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

const char* kind_name(Kind k) noexcept;
Result<bool> probe(BlockDevice& dev);
Result<Volume> read_volume(BlockDevice& dev);
Result<CheckReport> check(BlockDevice& dev, Progress* progress = nullptr);

} // namespace fsx::fat
