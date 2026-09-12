#include "fsx/ext4.hpp"
#include "fsx/endian.hpp"
#include "fsx/size.hpp"
#include "fsx/allocation.hpp"
#include "fsx/cancel.hpp"
#include "internal.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <sstream>

namespace fsx::ext4 {
using namespace detail;

std::uint64_t Superblock::groups_count() const {
    const auto data_blocks = blocks_count > first_data_block ? blocks_count-first_data_block : 0;
    if (blocks_per_group == 0) return 0;
    return data_blocks / blocks_per_group + (data_blocks % blocks_per_group != 0 ? 1U : 0U);
}

Result<bool> probe(BlockDevice& dev) {
    std::array<std::byte, 2> b{};
    auto r = dev.read_exact(1024+0x38, b);
    if (!r) return r.error();
    return read_le<std::uint16_t>(b, 0) == EXT4_MAGIC;
}

Result<Superblock> read_superblock(BlockDevice& dev) {
    std::array<std::byte, 1024> b{};
    auto r = dev.read_exact(1024, b);
    if (!r) return r.error();
    Superblock s;
    s.inodes_count = read_le<std::uint32_t>(b, 0x00);
    auto blocks_lo = read_le<std::uint32_t>(b, 0x04);
    auto rblocks_lo = read_le<std::uint32_t>(b, 0x08);
    auto free_lo = read_le<std::uint32_t>(b, 0x0c);
    s.free_inodes = read_le<std::uint32_t>(b, 0x10);
    s.first_data_block = read_le<std::uint32_t>(b, 0x14);
    s.log_block_size = read_le<std::uint32_t>(b, 0x18);
    s.blocks_per_group = read_le<std::uint32_t>(b, 0x20);
    s.inodes_per_group = read_le<std::uint32_t>(b, 0x28);
    s.magic = read_le<std::uint16_t>(b, 0x38);
    s.state = read_le<std::uint16_t>(b, 0x3a);
    s.errors = read_le<std::uint16_t>(b, 0x3c);
    s.rev_level = read_le<std::uint32_t>(b, 0x4c);
    s.first_inode = read_le<std::uint32_t>(b, 0x54);
    s.inode_size = read_le<std::uint16_t>(b, 0x58);
    s.feature_compat = read_le<std::uint32_t>(b, 0x5c);
    s.feature_incompat = read_le<std::uint32_t>(b, 0x60);
    s.feature_ro_compat = read_le<std::uint32_t>(b, 0x64);
    s.uuid = uuid_string(std::span<const std::byte>(b).subspan(0x68, 16));
    s.volume_name = fixed_string(std::span<const std::byte>(b).subspan(0x78, 16));
    s.desc_size = read_le<std::uint16_t>(b, 0xfe);
    std::uint64_t blocks_hi = 0, rblocks_hi = 0, free_hi = 0;
    if (s.feature_incompat & INCOMPAT_64BIT) {
        blocks_hi = read_le<std::uint32_t>(b, 0x150);
        rblocks_hi = read_le<std::uint32_t>(b, 0x154);
        free_hi = read_le<std::uint32_t>(b, 0x158);
    }
    s.blocks_count = blocks_lo | (blocks_hi<<32U);
    s.reserved_blocks = rblocks_lo|(rblocks_hi<<32U);
    s.free_blocks = free_lo|(free_hi<<32U);
    if (s.magic != EXT4_MAGIC) return Error{Errc::unsupported, 0,"not an ext2/3/4 filesystem"};
    const auto bs = s.block_size();
    if (bs<1024 || bs>65536 || (bs&(bs-1)) != 0) return Error{Errc::corrupt, 0,"invalid ext block size"};
    if (s.blocks_per_group == 0 || s.inodes_per_group == 0 || s.blocks_count == 0 || s.inodes_count == 0)
        return Error{Errc::corrupt, 0, "invalid ext geometry"};
    if (s.first_data_block >= s.blocks_count)
        return Error{Errc::corrupt, 0, "ext first data block is outside filesystem"};
    const auto bitmap_capacity = bs*8ULL;
    if (s.blocks_per_group>bitmap_capacity || s.inodes_per_group>bitmap_capacity)
        return Error{Errc::corrupt, 0,"ext group geometry exceeds one-block bitmap capacity"};
    if (s.inode_size<128 || s.inode_size>bs || (s.inode_size&(s.inode_size-1U)) != 0)
        return Error{Errc::corrupt, 0,"invalid ext inode size"};
    if (s.free_blocks>s.blocks_count || s.free_inodes>s.inodes_count || s.reserved_blocks>s.blocks_count)
        return Error{Errc::corrupt, 0,"ext free/reserved counters exceed filesystem geometry"};
    if ((s.feature_incompat&INCOMPAT_64BIT) != 0) {
        const auto ds = s.desc_size >= 32?s.desc_size:32;
        if (ds<64 || ds>bs || (ds&7U) != 0) return Error{
            Errc::corrupt, 0,"invalid 64-bit ext group descriptor size"
        }
        ;
    } else if (s.desc_size != 0 && (s.desc_size<32 || s.desc_size>bs || (s.desc_size&7U) != 0)) {
        return Error{Errc::corrupt, 0,"invalid ext group descriptor size"};
    }
    const auto groups = s.groups_count();
    if (groups == 0) return Error{Errc::corrupt, 0,"ext filesystem has zero block groups"};
    const auto inode_groups = s.inodes_count/s.inodes_per_group + (s.inodes_count%s.inodes_per_group != 0?1U:0U);
    if (inode_groups>groups) return Error{
        Errc::corrupt, 0,"ext inode geometry requires more groups than block geometry provides"
    }
    ;
    const auto dev_bytes = dev.geometry().size_bytes;
    if (dev_bytes != 0 && s.blocks_count>dev_bytes/bs)
        return Error{Errc::corrupt, 0,"filesystem is larger than containing device"};
    return s;
}

Result<CheckReport> check(BlockDevice& dev, Progress* progress) {
    auto sr = read_superblock(dev);
    if (!sr) return sr.error();
    auto sb = sr.value();
    CheckReport report;
    auto groups = sb.groups_count();
    report.groups_checked = groups;
    if (sb.state != 1) report.warnings.push_back("filesystem superblock is not marked clean");
    if ((sb.feature_ro_compat & RO_COMPAT_METADATA_CSUM) != 0) {
        report.warnings.push_back(
            "metadata_csum present; use check metadata or check deep for checksum coverage");
    }
    if ((sb.feature_incompat & INCOMPAT_META_BG) != 0) {
        report.warnings.push_back(
            "meta_bg present; deep group scan is limited until extended descriptor placement support lands");
    }
    if (progress) {
        progress->update({
                         "checking","group descriptors and allocation bitmaps", 0, groups, 0, 0, 0, groups
                         }
                        );
    }
    std::uint64_t allocated = 0;
    for (std::uint64_t g = 0; g<groups; ++g) {
        if (Cancellation::requested()) return Error{Errc::unsafe, 0,"check cancelled"};
        auto gd = read_group_desc(dev, sb, g);
        if (!gd) {
            report.warnings.push_back("deep scan stopped: "+gd.error().message);
            break;
        }
        if (gd.value().block_bitmap >= sb.blocks_count
            || gd.value().inode_bitmap >= sb.blocks_count || gd.value().inode_table >= sb.blocks_count)
            report.errors.push_back("group "+std::to_string(g)+" has metadata pointer outside filesystem");
        auto bm = read_block_bitmap(dev, sb, gd.value());
        if (!bm) {
            report.errors.push_back("group "+std::to_string(g)+": "+bm.error().message);
            break;
        }
        std::uint64_t group_start = sb.first_data_block+g*sb.blocks_per_group;
        std::uint64_t group_blocks = std::min<std::uint64_t>(sb.blocks_per_group, sb.blocks_count-group_start);
        for (std::uint64_t i = 0; i<group_blocks; ++i) if (bit_set(bm.value(), i)) ++allocated;
        if (progress) progress->update({
                                       "checking","group "+std::to_string(g+1)+"/"+std::to_string(groups),
                                       g+1, groups, 0, 0, g+1, groups
                                       }
                                      );
    }
    report.bitmap_allocated_blocks = allocated;
    report.clean = report.errors.empty();
    return report;
}

Result<ResizePlan> plan_shrink(BlockDevice& dev, std::uint64_t target_bytes, Progress* progress) {
    auto sr = read_superblock(dev);
    if (!sr) return sr.error();
    const auto sb = sr.value();
    ResizePlan p;
    p.current_bytes = sb.size_bytes();
    p.target_bytes = target_bytes;
    const auto bs = sb.block_size();
    p.block_size = bs;
    if (target_bytes >= p.current_bytes) return Error{
        Errc::invalid_argument, 0,"target is not smaller than filesystem"
    }
    ;
    p.target_blocks = target_bytes/bs;
    if (p.target_blocks <= sb.first_data_block) return Error{Errc::invalid_argument, 0,"target is too small"};

    auto graph_result = build_allocation_graph(dev, progress, nullptr);
    if (!graph_result) return graph_result.error();
    const auto& graph = graph_result.value();
    p.allocated_extent_count = graph.allocated.size();
    p.free_extent_count = graph.free.size();
    std::uint64_t allocated_above = 0, free_below = 0, highest = 0;
    for (const auto&e:graph.allocated) {
        if (e.length) highest = std::max(highest, e.end()-1);
        if (e.end()>p.target_blocks) {
            const auto begin = std::max(e.start, p.target_blocks);
            allocated_above+=e.end()-begin;
        }
    }
    for (const auto&e:graph.free) {
        if (e.start >= p.target_blocks) break;
        free_below+=std::min(e.end(), p.target_blocks)-e.start;
    }

    std::uint64_t metadata_above = 0;
    const auto groups = sb.groups_count();
    for (std::uint64_t g = 0; g<groups; ++g) {
        if (Cancellation::requested()) return Error{Errc::unsafe, 0,"resize plan cancelled"};
        auto gd = read_group_desc(dev, sb, g);
        if (!gd) return gd.error();
        auto mark = [&](std::uint64_t block) {
            if (block >= p.target_blocks)++metadata_above;
        }
        ;
        mark(gd.value().block_bitmap);
        mark(gd.value().inode_bitmap);
        const std::uint64_t it_blocks = (static_cast<std::uint64_t>(sb.inodes_per_group)*sb.inode_size+bs-1)/bs;
        if (gd.value().inode_table >= p.target_blocks)metadata_above+=it_blocks;
        else if (gd.value().inode_table + it_blocks > p.target_blocks) {
            metadata_above += gd.value().inode_table + it_blocks - p.target_blocks;
        }
    }

    auto geometry = build_relocation_geometry(graph, p.target_blocks);
    if (!geometry) return geometry.error();
    p.allocated_blocks_above_target = allocated_above;
    p.allocated_bytes_above_target = allocated_above*bs;
    p.metadata_blocks_above_target = metadata_above;
    p.free_blocks_below_target = free_below;
    p.highest_allocated_block = highest;
    p.requires_relocation = allocated_above>0 || metadata_above>0;
    p.target_possible_by_capacity = geometry.value().capacity_ok && p.target_blocks >= graph.allocated_blocks;
    p.relocation_move_count = geometry.value().moves.size();
    p.largest_relocation_extent_blocks = geometry.value().largest_source_extent;
    if ((sb.feature_incompat & INCOMPAT_META_BG) != 0) {
        p.notes.push_back(
            "meta_bg requires extended descriptor relocation support before destructive shrink can be enabled");
    }
    p.notes.push_back("geometry plan only; use plan relocate for owner-aware relocation and shrink readiness");
    return p;
}

}
