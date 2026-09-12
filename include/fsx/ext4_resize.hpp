#pragma once
#include "fsx/device.hpp"
#include "fsx/progress.hpp"
#include "fsx/result.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace fsx::ext4 {
struct ShrinkReadiness {
    std::uint64_t current_blocks{};
    std::uint64_t target_blocks{};
    std::uint64_t current_groups{};
    std::uint64_t target_groups{};
    std::uint64_t allocated_tail_blocks{};
    std::uint64_t allocated_inodes_in_removed_groups{};
    std::uint64_t fixed_metadata_tail_blocks{};
    bool requires_inode_relocation{false};
    bool metadata_only_finalize_possible{false};
    std::vector<std::string> blockers;
};
struct FinalizeOptions {
    std::string journal_path;
    bool allow_block_device{false};
};

struct GrowOptions {
    bool allow_block_device{false};
    bool verify_after{true};
};

struct GrowReport {
    std::uint64_t old_blocks{};
    std::uint64_t new_blocks{};
    std::uint64_t blocks_added{};
    std::uint64_t free_blocks_after{};
    bool verified{false};
};
Result<ShrinkReadiness> analyze_shrink_readiness(BlockDevice& dev,
                                                 std::uint64_t target_bytes, Progress* progress = nullptr);
Result<void> finalize_metadata_only_shrink(BlockDevice& dev,
                                           std::uint64_t target_bytes, const FinalizeOptions& options,
                                           Progress* progress = nullptr);
Result<GrowReport> grow_within_last_group(BlockDevice& dev,
                                          std::uint64_t target_bytes, const GrowOptions& options = {
                                          }
                                          , Progress* progress = nullptr);
}
