#pragma once
#include "fsx/device.hpp"
#include "fsx/progress.hpp"
#include "fsx/result.hpp"
#include <cstdint>
#include <string>

namespace fsx {

struct PartitionSyncOptions {
    std::string journal_path;
    bool allow_block_device{false};
};

struct PartitionSyncReport {
    std::uint32_t partition_index{};
    std::uint64_t first_lba{};
    std::uint64_t old_last_lba{};
    std::uint64_t new_last_lba{};
    std::uint64_t filesystem_bytes{};
    bool changed{false};
    bool verified{false};
    bool journal_completed{false};
};

// Shrink a GPT partition boundary to exactly match the ext filesystem size.
// The filesystem itself is never modified here. The function refuses a
// partition/device geometry mismatch so an incorrect disk/index pair cannot
// silently truncate an unrelated partition.
Result<PartitionSyncReport> sync_gpt_partition_to_ext4(BlockDevice& filesystem,
                                                       BlockDevice& disk,
                                                       std::uint32_t partition_index,
                                                       const PartitionSyncOptions& options = {},
                                                       Progress* progress = nullptr);

} // namespace fsx
