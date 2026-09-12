#pragma once
#include "fsx/ext4.hpp"
#include "fsx/io_scheduler.hpp"
#include "fsx/progress.hpp"
#include "fsx/result.hpp"
#include <cstdint>
#include <vector>

namespace fsx::ext4 {

struct BlockExtent {
    std::uint64_t start{};
    std::uint64_t length{};
    std::uint64_t end() const noexcept { return start + length;
    }
};

struct AllocationGraph {
    std::uint64_t block_size{};
    std::uint64_t total_blocks{};
    std::uint64_t allocated_blocks{};
    std::uint64_t free_blocks{};
    std::vector<BlockExtent> allocated;
    std::vector<BlockExtent> free;
};

struct RelocationGeometry {
    struct Move {
        BlockExtent source{};
        BlockExtent destination{};
    };
    std::uint64_t target_blocks{};
    std::uint64_t source_blocks{};
    std::uint64_t destination_blocks{};
    std::uint64_t largest_source_extent{};
    std::uint64_t largest_destination_extent{};
    std::vector<Move> moves;
    bool capacity_ok{false};
};

Result<AllocationGraph> build_allocation_graph(BlockDevice& dev,
                                               Progress* progress = nullptr,
                                               const IoTuning* tuning = nullptr);
Result<RelocationGeometry> build_relocation_geometry(const AllocationGraph& graph,
                                                     std::uint64_t target_blocks);

} // namespace fsx::ext4
