#pragma once
#include "fsx/device.hpp"
#include "fsx/progress.hpp"
#include "fsx/result.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace fsx::ext4 {

struct DeepIntegrityReport {
    bool clean{true};
    bool complete_owner_coverage{true};
    std::uint64_t groups_checked{};
    std::uint64_t allocated_inodes{};
    std::uint64_t extent_inodes{};
    std::uint64_t legacy_inodes{};
    std::uint64_t data_extents{};
    std::uint64_t data_blocks{};
    std::uint64_t extent_tree_blocks{};
    std::uint64_t owner_unallocated_ranges{};
    std::uint64_t metadata_unallocated_ranges{};
    std::uint64_t overlapping_owner_ranges{};
    std::uint64_t owner_metadata_overlaps{};
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

// Expensive structural verification intended before destructive operations or
// release validation. It combines checksum/counter verification with extent
// ownership, allocation-bitmap coverage, fixed-metadata allocation, and
// duplicate physical ownership checks.
Result<DeepIntegrityReport> verify_deep(BlockDevice& dev, Progress* progress = nullptr);

} // namespace fsx::ext4
