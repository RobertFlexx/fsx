#pragma once
#include "fsx/device.hpp"
#include "fsx/extents.hpp"
#include "fsx/journal.hpp"
#include "fsx/progress.hpp"
#include "fsx/result.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace fsx::ext4 {

enum class RelocationKind : std::uint32_t {
    data_extent = 1,
    extent_tree_block = 2
};

struct OwnerMove {
    RelocationKind kind{RelocationKind::data_extent};
    std::uint64_t transaction_id{};
    std::uint64_t inode{};
    std::uint64_t logical_block{};
    std::uint64_t source_block{};
    std::uint64_t destination_block{};
    std::uint64_t block_count{};
    bool unwritten{false};
    std::uint16_t tree_depth{0};
};

struct OwnerRelocationPlan {
    std::uint64_t block_size{};
    std::uint64_t target_blocks{};
    std::uint64_t target_bytes{};
    std::uint64_t data_blocks_to_move{};
    std::uint64_t tree_blocks_to_move{};
    std::uint64_t unknown_allocated_blocks_above_target{};
    std::uint64_t fixed_metadata_blocks_above_target{};
    std::uint64_t legacy_blockmap_inodes{};
    std::uint64_t allocated_inodes{};
    std::uint64_t extent_inodes{};
    bool complete_owner_coverage{false};
    bool destinations_available{false};
    std::vector<OwnerMove> moves;
};

struct StageOptions {
    std::string journal_path;
    bool allow_block_device{false};
    bool verify_after_write{true};
    std::size_t checkpoint_batch{64};
};

struct StageReport {
    std::uint64_t moves_total{};
    std::uint64_t moves_completed{};
    std::uint64_t bytes_copied{};
    std::uint64_t bytes_verified{};
    bool paused{false};
    bool complete{false};
};

Result<OwnerRelocationPlan> build_owner_relocation_plan(BlockDevice& dev,
                                                        std::uint64_t target_bytes,
                                                        Progress* progress = nullptr);

Result<StageReport> stage_relocation(BlockDevice& dev,
                                     const OwnerRelocationPlan& plan,
                                     const StageOptions& options,
                                     Progress* progress = nullptr);

} // namespace fsx::ext4
