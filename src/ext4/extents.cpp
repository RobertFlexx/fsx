#include "fsx/extents.hpp"
#include "fsx/cancel.hpp"
#include "fsx/endian.hpp"
#include "fsx/ext4_metadata.hpp"
#include "internal.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <unordered_set>
#include <vector>

namespace fsx::ext4 {
namespace {
constexpr std::uint32_t EXT4_EXTENTS_FL = 0x00080000U;
constexpr std::uint16_t EXTENT_MAGIC = 0xf30aU;
constexpr std::size_t INODE_I_BLOCK_OFF = 40;
constexpr std::size_t INODE_I_BLOCK_LEN = 60;
constexpr std::size_t INODE_FLAGS_OFF = 32;
constexpr std::size_t INODE_GENERATION_OFF = 100;

struct ParseCtx {
    BlockDevice& dev;
    const Superblock& sb;
    std::uint64_t inode;
    std::uint32_t generation;
    std::uint64_t retain_from;
    OwnerScan& out;
    const MetadataContext* metadata{};
    std::unordered_set<std::uint64_t> visited;
};

Result<void> parse_extent_node(ParseCtx& c,
                               std::span<const std::byte> node,
                               std::uint16_t expected_depth,
                               std::uint64_t container_byte_offset,
                               std::uint64_t container_block,
                               ExtentRefKind kind) {
    if (node.size() < 12) return Error{Errc::corrupt, 0,"extent node is truncated"};
    if (kind == ExtentRefKind::extent_block && c.metadata && c.metadata->metadata_csum) {
        auto ok = verify_extent_block_checksum(*c.metadata, c.inode, c.generation, node);
        if (!ok) return ok.error();
        if (!ok.value())
            return Error{
                Errc::corrupt, 0,"inode " + std::to_string(c.inode) + " extent block checksum mismatch"
            }
        ;
    }
    const auto magic = read_le<std::uint16_t>(node, 0);
    const auto entries = read_le<std::uint16_t>(node, 2);
    const auto max_entries = read_le<std::uint16_t>(node, 4);
    const auto depth = read_le<std::uint16_t>(node, 6);
    if (magic != EXTENT_MAGIC) return Error{
        Errc::corrupt, 0,"inode " + std::to_string(c.inode) + " has invalid extent magic"
    }
    ;
    if (depth != expected_depth) return Error{
        Errc::corrupt, 0,"inode " + std::to_string(c.inode) + " extent depth mismatch"
    }
    ;
    if (depth > 5) return Error{
        Errc::corrupt, 0,"inode " + std::to_string(c.inode) + " extent tree depth is unreasonable"
    }
    ;
    const std::size_t bytes_available = node.size() - 12;
    const std::size_t max_by_buffer = bytes_available / 12;
    if (entries > max_entries || entries > max_by_buffer)
        return Error{
            Errc::corrupt, 0,"inode " + std::to_string(c.inode) + " extent entry count exceeds node capacity"
        }
    ;

    if (depth == 0) {
        std::uint64_t last_logical_end = 0;
        bool first = true;
        for (std::uint16_t i = 0; i<entries; ++i) {
            const std::size_t off = 12U + static_cast<std::size_t>(i)*12U;
            const auto logical = static_cast<std::uint64_t>(read_le<std::uint32_t>(node, off));
            const auto raw_len = read_le<std::uint16_t>(node, off+4);
            if (raw_len == 0) return Error{
                Errc::corrupt, 0,"inode " + std::to_string(c.inode) + " contains zero-length extent"
            }
            ;
            const bool unwritten = raw_len > 32768U;
            const std::uint64_t len = unwritten ? static_cast<std::uint64_t>(raw_len - 32768U) :
                static_cast<std::uint64_t>(raw_len);
            const auto hi = static_cast<std::uint64_t>(read_le<std::uint16_t>(node, off+6));
            const auto lo = static_cast<std::uint64_t>(read_le<std::uint32_t>(node, off+8));
            const auto physical = (hi<<32U) | lo;
            if (physical == 0 || physical >= c.sb.blocks_count || len > c.sb.blocks_count - physical)
                return Error{
                    Errc::corrupt, 0,"inode " + std::to_string(c.inode) + " extent points outside filesystem"
                }
            ;
            if (!first && logical < last_logical_end)
                return Error{
                    Errc::corrupt, 0,"inode " + std::to_string(c.inode) + " extent tree is not logically ordered"
                }
            ;
            first = false;
            last_logical_end = logical+len;
            ++c.out.total_extent_records;
            if (physical + len > c.retain_from) {
                c.out.extents.push_back({c.inode, logical, physical, len, c.generation, unwritten,
                                        {kind, container_byte_offset, container_block,
                                        static_cast<std::uint32_t>(off), depth}});
                c.out.retained_extent_blocks += len;
            }
        }
        return {};
    }

    std::uint64_t last_index = 0;
    bool first = true;
    for (std::uint16_t i = 0; i<entries; ++i) {
        const std::size_t off = 12U + static_cast<std::size_t>(i)*12U;
        const auto logical = static_cast<std::uint64_t>(read_le<std::uint32_t>(node, off));
        const auto leaf_lo = static_cast<std::uint64_t>(read_le<std::uint32_t>(node, off+4));
        const auto leaf_hi = static_cast<std::uint64_t>(read_le<std::uint16_t>(node, off+8));
        const auto child = (leaf_hi<<32U)|leaf_lo;
        if (!first && logical <= last_index)
            return Error{
                Errc::corrupt, 0,"inode " + std::to_string(c.inode) + " extent index is not strictly ordered"
            }
        ;
        first = false;
        last_index = logical;
        if (child == 0 || child >= c.sb.blocks_count)
            return Error{
                Errc::corrupt, 0,"inode " + std::to_string(c.inode) + " extent index points outside filesystem"
            }
        ;
        if (!c.visited.insert(child).second)
            return Error{
                Errc::corrupt, 0,"inode " + std::to_string(c.inode) + " extent tree contains a cycle"
            }
        ;

        if (child >= c.retain_from) {
            c.out.tree_blocks.push_back({
                                        c.inode, child, container_byte_offset,
                                        static_cast<std::uint32_t>(off), static_cast<std::uint16_t>(depth-1)
                                        }
                                       );
            ++c.out.retained_tree_blocks;
        }
        std::vector<std::byte> child_buf(static_cast<std::size_t>(c.sb.block_size()));
        auto rr = c.dev.read_exact(child*c.sb.block_size(), child_buf);
        if (!rr) return rr.error();
        auto pr = parse_extent_node(c, child_buf, static_cast<std::uint16_t>(depth-1),
                                    child*c.sb.block_size(), child, ExtentRefKind::extent_block);
        if (!pr)return pr.error();
    }
    return {};
}

bool inode_allocated(std::span<const std::byte> bm, std::uint64_t index) {
    return detail::bit_set(bm, index);
}

Result<OwnerScan> scan_specific_inode(BlockDevice& dev, const Superblock& sb,
                                      std::uint64_t ino, std::uint64_t retain_from) {
    if (ino == 0 || ino > sb.inodes_count)
        return Error{Errc::invalid_argument, 0,"inode number outside filesystem"};
    const auto z = ino - 1;
    const auto group = z / sb.inodes_per_group;
    const auto idx = z % sb.inodes_per_group;
    auto gd = detail::read_group_desc(dev, sb, group);
    if (!gd) return gd.error();
    if ((gd.value().flags & detail::BG_INODE_UNINIT) != 0)
        return Error{Errc::not_found, 0,"inode group is uninitialized"};
    auto ib = detail::read_inode_bitmap(dev, sb, gd.value());
    if (!ib) return ib.error();
    if (!inode_allocated(ib.value(), idx))
        return Error{Errc::not_found, 0,"inode is not allocated"};

    std::vector<std::byte> inode(sb.inode_size);
    const auto inode_abs = gd.value().inode_table * sb.block_size() + idx * sb.inode_size;
    auto rr = dev.read_exact(inode_abs, inode);
    if (!rr) return rr.error();
    const auto mode = read_le<std::uint16_t>(inode, 0);
    const auto links = read_le<std::uint16_t>(inode, 26);
    if (mode == 0 || links == 0)
        return Error{Errc::not_found, 0,"inode is allocated but inactive"};
    const auto flags = read_le<std::uint32_t>(inode, INODE_FLAGS_OFF);
    auto mctx = load_metadata_context(dev);
    if (!mctx) return mctx.error();
    OwnerScan out;
    out.inodes_examined = 1;
    out.allocated_inodes = 1;
    if ((flags & EXT4_EXTENTS_FL) == 0) {
        out.legacy_blockmap_inodes = 1;
        return out;
    }
    out.extent_inodes = 1;
    const auto generation = read_le<std::uint32_t>(inode, INODE_GENERATION_OFF);
    ParseCtx ctx{dev, sb, ino, generation, retain_from, out, &mctx.value(), {}};
    auto root = std::span<const std::byte>(inode).subspan(INODE_I_BLOCK_OFF, INODE_I_BLOCK_LEN);
    const auto depth = root.size() >= 8 ? read_le<std::uint16_t>(root, 6)
        : std::numeric_limits<std::uint16_t>::max();
    auto pr = parse_extent_node(ctx, root, depth, inode_abs+INODE_I_BLOCK_OFF, 0, ExtentRefKind::inode_root);
    if (!pr) {
        ++out.corrupt_inodes;
        return pr.error();
    }
    std::sort(out.extents.begin(), out.extents.end(), [](const OwnedExtent&a, const OwnedExtent&b) {
              return a.logical_block < b.logical_block;
              });
    return out;
}
}

Result<OwnerScan> scan_extent_owners(BlockDevice& dev, std::uint64_t retain_from, Progress* progress) {
    auto sr = read_superblock(dev);
    if (!sr)return sr.error();
    const auto sb = sr.value();
    auto mctx = load_metadata_context(dev);
    if (!mctx)return mctx.error();
    if (sb.inode_size < 128 || sb.inode_size > sb.block_size())
        return Error{Errc::unsupported, 0,"unsupported ext inode size"};
    if ((sb.feature_incompat & detail::INCOMPAT_META_BG) != 0)
        return Error{Errc::unsupported, 0,"owner scan does not yet support meta_bg descriptor placement"};
    if (retain_from > sb.blocks_count) retain_from = sb.blocks_count;

    OwnerScan out;
    const auto groups = sb.groups_count();
    if (progress)progress->update({"owner-scan","allocated inode and extent trees", 0, groups, 0, 0, 0, groups});

    for (std::uint64_t g = 0; g<groups; ++g) {
        if (Cancellation::requested())return Error{Errc::unsafe, 0,"owner scan cancelled"};
        auto gd = detail::read_group_desc(dev, sb, g);
        if (!gd)return gd.error();
        if ((gd.value().flags&detail::BG_INODE_UNINIT) != 0) {
            if (progress)progress->update({
                                          "owner-scan",
                                          "group "+std::to_string(g+1)+"/"+std::to_string(groups)+" (inode-uninit)",
                                          g+1, groups, 0, 0, out.allocated_inodes, sb.inodes_count
                                          }
                                         );
            continue;
        }
        auto ib = detail::read_inode_bitmap(dev, sb, gd.value());
        if (!ib)return ib.error();
        const auto group_first_inode = g*static_cast<std::uint64_t>(sb.inodes_per_group)+1;
        const auto remaining = sb.inodes_count >= group_first_inode?sb.inodes_count-group_first_inode+1:0;
        const auto count = std::min<std::uint64_t>(sb.inodes_per_group, remaining);
        const auto table_bytes = count*static_cast<std::uint64_t>(sb.inode_size);
        if (table_bytes > std::numeric_limits<std::size_t>::max()) return Error{
            Errc::unsupported, 0,"inode table is too large for address space"
        }
        ;
        std::vector<std::byte> table(static_cast<std::size_t>(table_bytes));
        if (!table.empty()) {
            auto rr = dev.read_exact(gd.value().inode_table*sb.block_size(), table);
            if (!rr)return rr.error();
        }

        for (std::uint64_t idx = 0; idx<count; ++idx) {
            ++out.inodes_examined;
            if (!inode_allocated(ib.value(), idx))continue;
            ++out.allocated_inodes;
            const auto ino = group_first_inode+idx;
            const auto off = idx*static_cast<std::uint64_t>(sb.inode_size);
            const auto inode = std::span<const std::byte>(table).subspan(static_cast<std::size_t>(off),
                                                                         sb.inode_size);
            const auto mode = read_le<std::uint16_t>(inode, 0);
            const auto links = read_le<std::uint16_t>(inode, 26);
            if (mode == 0 || links == 0) continue;
            const auto flags = read_le<std::uint32_t>(inode, INODE_FLAGS_OFF);
            if ((flags&EXT4_EXTENTS_FL) == 0) {
                ++out.legacy_blockmap_inodes;
                continue;
            }
            ++out.extent_inodes;
            const auto generation = read_le<std::uint32_t>(inode, INODE_GENERATION_OFF);
            const auto inode_abs = gd.value().inode_table*sb.block_size()+off;
            ParseCtx ctx{dev, sb, ino, generation, retain_from, out, &mctx.value(), {}};
            auto root = inode.subspan(INODE_I_BLOCK_OFF, INODE_I_BLOCK_LEN);
            const auto root_depth = root.size() >= 8?read_le<std::uint16_t>(root,
                                                                            6):
                std::numeric_limits<std::uint16_t>::max();
            auto pr = parse_extent_node(ctx, root, root_depth,
                                        inode_abs+INODE_I_BLOCK_OFF, 0, ExtentRefKind::inode_root);
            if (!pr) {
                ++out.corrupt_inodes;
                return pr.error();
            }
        }
        if (progress)progress->update({
                                      "owner-scan","group "+std::to_string(g+1)+"/"+std::to_string(groups),
                                      g+1, groups, 0, 0, out.allocated_inodes, sb.inodes_count
                                      }
                                     );
    }
    std::sort(out.extents.begin(), out.extents.end(), [](const OwnedExtent&a, const OwnedExtent&b) {
              return a.physical_block<b.physical_block;
              }
             );
    return out;
}

Result<OwnerScan> scan_inode_extent_tree(BlockDevice& dev, std::uint64_t inode, std::uint64_t retain_from) {
    auto sr = read_superblock(dev);
    if (!sr)return sr.error();
    const auto sb = sr.value();
    if (sb.inode_size < 128 || sb.inode_size > sb.block_size())
        return Error{Errc::unsupported, 0,"unsupported ext inode size"};
    if ((sb.feature_incompat & detail::INCOMPAT_META_BG) != 0)
        return Error{Errc::unsupported, 0,"owner scan does not yet support meta_bg descriptor placement"};
    if (retain_from > sb.blocks_count) retain_from = sb.blocks_count;
    return scan_specific_inode(dev, sb, inode, retain_from);
}

Result<OwnedExtent> find_inode_extent(BlockDevice& dev, std::uint64_t inode, std::uint64_t logical_block) {
    auto scan = scan_inode_extent_tree(dev, inode, 0);
    if (!scan)return scan.error();
    for (const auto& e:scan.value().extents) {
        if (logical_block >= e.logical_block && logical_block<e.logical_block+e.length) return e;
    }
    return Error{Errc::not_found, 0,"logical block is not covered by an extent"};
}

} // namespace fsx::ext4
