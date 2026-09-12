#pragma once
#include "fsx/device.hpp"
#include "fsx/progress.hpp"
#include "fsx/result.hpp"
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace fsx::ext4 {

struct Superblock {
    std::uint64_t inodes_count{};
    std::uint64_t blocks_count{};
    std::uint64_t reserved_blocks{};
    std::uint64_t free_blocks{};
    std::uint64_t free_inodes{};
    std::uint32_t first_data_block{};
    std::uint32_t log_block_size{};
    std::uint32_t blocks_per_group{};
    std::uint32_t inodes_per_group{};
    std::uint16_t state{};
    std::uint16_t errors{};
    std::uint32_t rev_level{};
    std::uint32_t first_inode{};
    std::uint16_t inode_size{};
    std::uint32_t feature_compat{};
    std::uint32_t feature_incompat{};
    std::uint32_t feature_ro_compat{};
    std::uint32_t desc_size{};
    std::uint16_t magic{};
    std::string volume_name;
    std::string uuid;

    std::uint64_t block_size() const noexcept {
        // ext-family block sizes supported by this implementation are 1 KiB
        // through 64 KiB. Guard before shifting so hostile superblocks cannot
        // trigger undefined behaviour.
        return log_block_size <= 6U ? (1024ULL << log_block_size) : 0ULL;
    }
    std::uint64_t size_bytes() const noexcept {
        const auto bs = block_size();
        if (bs == 0 || blocks_count > std::numeric_limits<std::uint64_t>::max() / bs)
            return std::numeric_limits<std::uint64_t>::max();
        return blocks_count * bs;
    }
    std::uint64_t groups_count() const;
};

struct CheckReport {
    bool clean{true};
    std::uint64_t groups_checked{};
    std::uint64_t bitmap_allocated_blocks{};
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

struct ResizePlan {
    std::uint64_t current_bytes{};
    std::uint64_t target_bytes{};
    std::uint64_t target_blocks{};
    std::uint64_t block_size{};
    std::uint64_t allocated_blocks_above_target{};
    std::uint64_t allocated_bytes_above_target{};
    std::uint64_t metadata_blocks_above_target{};
    std::uint64_t free_blocks_below_target{};
    std::uint64_t highest_allocated_block{};
    std::uint64_t allocated_extent_count{};
    std::uint64_t free_extent_count{};
    std::uint64_t relocation_move_count{};
    std::uint64_t largest_relocation_extent_blocks{};
    bool target_possible_by_capacity{false};
    bool requires_relocation{false};
    std::vector<std::string> notes;
};

Result<bool> probe(BlockDevice& dev);
Result<Superblock> read_superblock(BlockDevice& dev);
Result<CheckReport> check(BlockDevice& dev, Progress* progress = nullptr);
Result<ResizePlan> plan_shrink(BlockDevice& dev, std::uint64_t target_bytes, Progress* progress = nullptr);

} // namespace fsx::ext4
