#include "fsx/ext4_verify.hpp"
#include "fsx/allocation.hpp"
#include "fsx/cancel.hpp"
#include "fsx/ext4_metadata.hpp"
#include "fsx/extents.hpp"
#include "internal.hpp"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace fsx::ext4 {
namespace {
struct Range {
    std::uint64_t start{};
    std::uint64_t end{};
    std::uint64_t inode{};
    bool metadata{false};
};

bool add_overflow(std::uint64_t a, std::uint64_t b, std::uint64_t& out) {
    if (b > std::numeric_limits<std::uint64_t>::max() - a) return true;
    out = a + b;
    return false;
}

bool fully_allocated(const std::vector<BlockExtent>& allocated,
                     std::uint64_t start,
                     std::uint64_t length) {
    if (length == 0) return true;
    std::uint64_t end = 0;
    if (add_overflow(start, length, end)) return false;
    auto it = std::lower_bound(allocated.begin(), allocated.end(), start,
                               [](const BlockExtent& e, std::uint64_t b) {
                               return e.end() <= b;
                               });
    auto covered = start;
    while (it != allocated.end() && it->start <= covered) {
        covered = std::max(covered, it->end());
        if (covered >= end) return true;
        ++it;
    }
    return false;
}

std::uint64_t count_overlaps(std::vector<Range> ranges) {
    if (ranges.empty()) return 0;
    std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) {
              if (a.start != b.start) return a.start < b.start;
              if (a.end != b.end) return a.end < b.end;
              return a.inode < b.inode;
              });
    std::uint64_t count = 0;
    std::uint64_t furthest_end = ranges.front().end;
    for (std::size_t i = 1; i < ranges.size(); ++i) {
        if (ranges[i].start < furthest_end) ++count;
        furthest_end = std::max(furthest_end, ranges[i].end);
    }
    return count;
}

bool overlaps_any(const std::vector<Range>& sorted, std::uint64_t start, std::uint64_t end) {
    if (start >= end) return false;
    auto it = std::lower_bound(sorted.begin(), sorted.end(), start,
                               [](const Range& r, std::uint64_t b) {
                               return r.end <= b;
                               });
    return it != sorted.end() && it->start < end;
}
}

Result<DeepIntegrityReport> verify_deep(BlockDevice& dev, Progress* progress) {
    DeepIntegrityReport out;
    auto basic = verify_metadata(dev, true, progress);
    if (!basic) return basic.error();
    out.groups_checked = basic.value().groups_checked;
    out.warnings = basic.value().warnings;
    out.errors = basic.value().errors;

    auto sb = read_superblock(dev);
    if (!sb) return sb.error();
    auto graph = build_allocation_graph(dev, progress, nullptr);
    if (!graph) return graph.error();
    auto owners = scan_extent_owners(dev, 0, progress);
    if (!owners) return owners.error();
    out.allocated_inodes = owners.value().allocated_inodes;
    out.extent_inodes = owners.value().extent_inodes;
    out.legacy_inodes = owners.value().legacy_blockmap_inodes;
    out.data_extents = owners.value().extents.size();
    out.extent_tree_blocks = owners.value().tree_blocks.size();
    if (out.legacy_inodes != 0) {
        out.complete_owner_coverage = false;
        out.warnings.push_back(std::to_string(out.legacy_inodes) +
                               " allocated inodes use legacy indirect block maps; full owner coverage is unavailable");
    }

    std::vector<Range> owner_ranges;
    owner_ranges.reserve(owners.value().extents.size() + owners.value().tree_blocks.size());
    for (const auto& e : owners.value().extents) {
        std::uint64_t end = 0;
        if (add_overflow(e.physical_block, e.length, end) || end > sb.value().blocks_count) {
            out.errors.push_back("inode " + std::to_string(e.inode) + " extent overflows filesystem geometry");
            continue;
        }
        owner_ranges.push_back({e.physical_block, end, e.inode, false});
        out.data_blocks += e.length;
        if (!fully_allocated(graph.value().allocated, e.physical_block, e.length)) {
            ++out.owner_unallocated_ranges;
            out.errors.push_back("inode " + std::to_string(e.inode) +
                                 " extent is not fully allocated in the block bitmap");
        }
    }
    for (const auto& t : owners.value().tree_blocks) {
        owner_ranges.push_back({t.physical_block, t.physical_block + 1, t.inode, false});
        if (!fully_allocated(graph.value().allocated, t.physical_block, 1)) {
            ++out.owner_unallocated_ranges;
            out.errors.push_back("inode " + std::to_string(t.inode) +
                                 " extent-tree block is not allocated in the block bitmap");
        }
    }
    out.overlapping_owner_ranges = count_overlaps(owner_ranges);
    if (out.overlapping_owner_ranges != 0)
        out.errors.push_back(std::to_string(out.overlapping_owner_ranges) +
                             " overlapping physical owner ranges detected");

    std::sort(owner_ranges.begin(), owner_ranges.end(), [](const Range& a, const Range& b) {
              if (a.start != b.start) return a.start < b.start;
              return a.end < b.end;
              });

    const auto inode_table_blocks =
        (static_cast<std::uint64_t>(sb.value().inodes_per_group) * sb.value().inode_size +
         sb.value().block_size() - 1) / sb.value().block_size();
    for (std::uint64_t g = 0; g < sb.value().groups_count(); ++g) {
        if (Cancellation::requested()) return Error{Errc::unsafe, 0, "deep verification cancelled"};
        auto gd = detail::read_group_desc(dev, sb.value(), g);
        if (!gd) return gd.error();
        const Range fixed[] = {
            {gd.value().block_bitmap, gd.value().block_bitmap + 1, 0, true},
            {gd.value().inode_bitmap, gd.value().inode_bitmap + 1, 0, true},
            {gd.value().inode_table, gd.value().inode_table + inode_table_blocks, 0, true}
        };
        for (const auto& r : fixed) {
            if (r.end > sb.value().blocks_count) {
                ++out.metadata_unallocated_ranges;
                out.errors.push_back("group " + std::to_string(g) + " fixed metadata exceeds filesystem geometry");
                continue;
            }
            if (!fully_allocated(graph.value().allocated, r.start, r.end - r.start)) {
                ++out.metadata_unallocated_ranges;
                out.errors.push_back("group " + std::to_string(g) +
                                     " fixed metadata is not fully allocated in block bitmap");
            }
            if (overlaps_any(owner_ranges, r.start, r.end)) {
                ++out.owner_metadata_overlaps;
                out.errors.push_back("group " + std::to_string(g) +
                                     " fixed metadata overlaps inode-owned blocks");
            }
        }
        if (progress)
            progress->update({"deep-check", "group " + std::to_string(g + 1) + "/" +
                             std::to_string(sb.value().groups_count()),
                             g + 1, sb.value().groups_count(), 0, 0,
                             g + 1, sb.value().groups_count()});
    }

    out.clean = out.errors.empty();
    return out;
}

} // namespace fsx::ext4
