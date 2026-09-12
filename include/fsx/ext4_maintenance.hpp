#pragma once
#include "fsx/device.hpp"
#include "fsx/progress.hpp"
#include "fsx/result.hpp"
#include <cstdint>
#include <string>

namespace fsx::ext4 {

struct RepairOptions {
    bool allow_block_device{false};
    bool verify_after{true};
};

struct RepairReport {
    std::uint64_t groups_rewritten{};
    std::uint64_t free_blocks{};
    std::uint64_t free_inodes{};
    bool superblock_rewritten{false};
    bool verified{false};
};

// Conservative repair: allocation bitmaps are treated as authoritative and
// only redundant counters/checksums are rebuilt. It does not reconstruct
// bitmaps, directory trees, orphan lists, journals, or inode ownership.
Result<RepairReport> repair_metadata_counters(BlockDevice& dev,
                                              const RepairOptions& options = {},
                                              Progress* progress = nullptr);

// Change only the ext volume label and checksum-correct superblock copies.
// UUID changes are intentionally not bundled here because metadata_csum UUID
// changes require rewriting checksum domains throughout the filesystem.
Result<void> set_volume_label(BlockDevice& dev,
                              const std::string& label,
                              bool allow_block_device = false);

} // namespace fsx::ext4
