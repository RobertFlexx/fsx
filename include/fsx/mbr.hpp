#pragma once
#include "fsx/device.hpp"
#include "fsx/result.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace fsx::mbr {
struct Partition {
    std::uint32_t index{};
    bool bootable{false};
    std::uint8_t type{};
    std::uint32_t first_lba{};
    std::uint32_t sectors{};
    std::uint64_t last_lba() const noexcept {
        return sectors?static_cast<std::uint64_t>(first_lba)+sectors-1:first_lba;
    }
};
struct Table {
    bool signature_ok{false};
    bool protective{false};
    bool hybrid{false};
    std::uint32_t disk_signature{};
    std::vector<Partition> partitions;
};
struct MutationOptions {
    bool allow_block_device{false};
    std::string backup_sector_path;
};
Result<Table> read_table(BlockDevice& dev);
Result<void> resize_partition(BlockDevice& dev, std::uint32_t index,
                              std::uint32_t first_lba, std::uint32_t sectors, const MutationOptions& options);
Result<void> restore_sector(BlockDevice& dev,
                            const std::string& backup_sector_path, bool allow_block_device = false);
}
