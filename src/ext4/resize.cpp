#include "fsx/ext4_resize.hpp"
#include "fsx/allocation.hpp"
#include "fsx/cancel.hpp"
#include "fsx/endian.hpp"
#include "fsx/ext4_metadata.hpp"
#include "fsx/journal.hpp"
#include "fsx/safety.hpp"
#include "fsx/uuid.hpp"
#include "internal.hpp"
#include <algorithm>
#include <array>
#include <limits>

namespace fsx::ext4 {
namespace {
constexpr std::uint32_t COMPAT_SPARSE_SUPER2 = 0x0200U;
constexpr std::uint32_t RO_SPARSE_SUPER = 0x0001U;

bool is_power(std::uint64_t n, std::uint64_t base) {
    if (n<1)return false;
    while (n%base == 0)n/=base;
    return n == 1;
}
bool group_has_super(std::span<const std::byte> sb, std::uint64_t group) {
    if (group == 0)return true;
    const auto compat = read_le<std::uint32_t>(sb, 0x5c);
    const auto ro = read_le<std::uint32_t>(sb, 0x64);
    if ((compat&COMPAT_SPARSE_SUPER2) != 0) {
        const auto a = read_le<std::uint32_t>(sb, 0x24c);
        const auto b = read_le<std::uint32_t>(sb, 0x250);
        return group == a || group == b;
    }
    if ((ro&RO_SPARSE_SUPER) == 0)return true;
    if (group == 1)return true;
    return is_power(group, 3) || is_power(group, 5) || is_power(group, 7);
}
void set_bit(std::span<std::byte> bm, std::uint64_t bit) {
    const auto by = static_cast<std::size_t>(bit>>3U);
    const auto sh = static_cast<unsigned>(bit&7U);
    bm[by] = std::byte(std::to_integer<std::uint8_t>(bm[by])|static_cast<std::uint8_t>(1U<<sh));
}
void clear_bit(std::span<std::byte> bm, std::uint64_t bit) {
    const auto by = static_cast<std::size_t>(bit>>3U);
    const auto sh = static_cast<unsigned>(bit&7U);
    bm[by] = std::byte(std::to_integer<std::uint8_t>(bm[by])&static_cast<std::uint8_t>(~(1U<<sh)));
}
std::uint32_t desc_free(std::span<const std::byte>gd) {
    std::uint32_t v = read_le<std::uint16_t>(gd, 12);
    if (gd.size() >= 46)v|=static_cast<std::uint32_t>(read_le<std::uint16_t>(gd, 44))<<16U;
    return v;
}
void set_desc_free(std::span<std::byte>gd, std::uint32_t v) {
    write_le16(gd, 12, static_cast<std::uint16_t>(v));
    if (gd.size() >= 46)write_le16(gd, 44, static_cast<std::uint16_t>(v>>16U));
}
void set_u64_lohi(std::span<std::byte>b, std::size_t lo, std::size_t hi, std::uint64_t v) {
    write_le32(b, lo, static_cast<std::uint32_t>(v));
    write_le32(b, hi, static_cast<std::uint32_t>(v>>32U));
}

Result<void> write_super_copies(BlockDevice&dev, const MetadataContext&ctx, std::span<const std::byte>primary) {
    const auto bs = ctx.sb.block_size();
    if (bs == 0)return Error{Errc::corrupt, 0,"invalid ext4 block size while updating backup superblocks"};
    auto p = std::vector<std::byte>(primary.begin(), primary.end());
    auto wr = write_raw_superblock(dev, p);
    if (!wr)return wr.error();
    for (std::uint64_t g = 1; g<ctx.sb.groups_count(); ++g) {
        if (!group_has_super(primary, g))continue;
        const auto start = detail::group_start_block(ctx.sb, g);
        if (start >= ctx.sb.blocks_count || start>std::numeric_limits<std::uint64_t>::max()/bs)
            return Error{Errc::corrupt, 0,"backup superblock address is outside filesystem"};
        auto copy = p;
        write_le16(copy, 0x5a, static_cast<std::uint16_t>(g));
        auto c = update_superblock_checksum(copy);
        if (!c)return c.error();
        wr = dev.write_exact(start*bs, copy);
        if (!wr)return wr.error();
    }
    return dev.flush();
}
Result<void> write_group_desc_backups(BlockDevice&dev, const MetadataContext&ctx,
                                      std::span<const std::byte>super, std::uint64_t desc_group,
                                      std::span<const std::byte>desc) {
    const auto bs = ctx.sb.block_size();
    const auto ds = static_cast<std::uint64_t>(ctx.sb.desc_size >= 32?ctx.sb.desc_size:32);
    if (bs == 0 || ds == 0 || ds>bs)return Error{Errc::corrupt, 0,"invalid ext4 group descriptor geometry"};
    if (desc_group>std::numeric_limits<std::uint64_t>::max()/ds)return Error{
        Errc::corrupt, 0,"backup descriptor offset overflows"
    }
    ;
    const auto desc_off = desc_group*ds;
    for (std::uint64_t g = 1; g<ctx.sb.groups_count(); ++g) {
        if (!group_has_super(super, g))continue;
        const auto start = detail::group_start_block(ctx.sb, g);
        if (start >= ctx.sb.blocks_count-1 || start+1>std::numeric_limits<std::uint64_t>::max()/bs)
            return Error{Errc::corrupt, 0,"backup descriptor table address is outside filesystem"};
        const auto base = (start+1)*bs;
        if (desc_off>std::numeric_limits<std::uint64_t>::max()-base)
            return Error{Errc::corrupt, 0,"backup descriptor address overflows"};
        auto wr = dev.write_exact(base+desc_off, desc);
        if (!wr)return wr.error();
    }
    return {};
}
}

Result<ShrinkReadiness> analyze_shrink_readiness(BlockDevice&dev,
                                                 std::uint64_t target_bytes, Progress*progress) {
    auto sr = read_superblock(dev);
    if (!sr)return sr.error();
    const auto sb = sr.value();
    if (target_bytes >= sb.size_bytes())return Error{
        Errc::invalid_argument, 0,"target is not smaller than filesystem"
    }
    ;
    if (target_bytes%sb.block_size() != 0)return Error{
        Errc::invalid_argument, 0,"target must be aligned to filesystem block size"
    }
    ;
    ShrinkReadiness r;
    r.current_blocks = sb.blocks_count;
    r.target_blocks = target_bytes/sb.block_size();
    r.current_groups = sb.groups_count();
    const auto data = r.target_blocks>sb.first_data_block?r.target_blocks-sb.first_data_block:0;
    r.target_groups = sb.blocks_per_group?(data+sb.blocks_per_group-1)/sb.blocks_per_group:0;
    if (r.target_groups<r.current_groups) {
        r.requires_inode_relocation = true;
        const auto first_removed_inode = r.target_groups*static_cast<std::uint64_t>(sb.inodes_per_group)+1;
        for (std::uint64_t g = r.target_groups; g<r.current_groups; ++g) {
            auto gd = detail::read_group_desc(dev, sb, g);
            if (!gd)return gd.error();
            if ((gd.value().flags&detail::BG_INODE_UNINIT) != 0)continue;
            auto ib = detail::read_inode_bitmap(dev, sb, gd.value());
            if (!ib)return ib.error();
            const auto base = g*static_cast<std::uint64_t>(sb.inodes_per_group)+1;
            const auto remain = sb.inodes_count >= base?sb.inodes_count-base+1:0;
            const auto n = std::min<std::uint64_t>(sb.inodes_per_group, remain);
            for (std::uint64_t i = 0; i<n; ++i)if (detail::bit_set(ib.value(),
                                                                   i))++r.allocated_inodes_in_removed_groups;
        }
        if (r.allocated_inodes_in_removed_groups) {
            r.blockers.push_back(
                std::to_string(r.allocated_inodes_in_removed_groups)
                + " allocated inodes must be renumbered out of removed block groups");
        }
        else r.blockers.push_back("target removes block groups; inode-table/group removal engine is not enabled yet");
        (void)first_removed_inode;
    }
    auto graph = build_allocation_graph(dev, progress, nullptr);
    if (!graph)return graph.error();
    for (const auto& e : graph.value().allocated) {
        if (e.end() > r.target_blocks) {
            r.allocated_tail_blocks += e.end() - std::max(e.start, r.target_blocks);
        }
    }
    for (std::uint64_t g = 0; g<r.current_groups; ++g) {
        auto gd = detail::read_group_desc(dev, sb, g);
        if (!gd)return gd.error();
        auto mark = [&](std::uint64_t b) {
            if (b >= r.target_blocks && b<sb.blocks_count)++r.fixed_metadata_tail_blocks;
        }
        ;
        mark(gd.value().block_bitmap);
        mark(gd.value().inode_bitmap);
        const auto it =
            (static_cast<std::uint64_t>(sb.inodes_per_group) * sb.inode_size + sb.block_size() - 1)
            / sb.block_size();
        const auto a = std::max(gd.value().inode_table, r.target_blocks),
              z = std::min(gd.value().inode_table+it, sb.blocks_count);
        if (z>a)r.fixed_metadata_tail_blocks+=z-a;
    }
    if (r.allocated_tail_blocks)
        r.blockers.push_back(std::to_string(r.allocated_tail_blocks)+" allocated blocks remain at or above target");
    if (r.fixed_metadata_tail_blocks)
        r.blockers.push_back(std::to_string(r.fixed_metadata_tail_blocks) +
                             " fixed metadata blocks remain at or above target");
    r.metadata_only_finalize_possible = r.target_groups == r.current_groups &&
        r.allocated_tail_blocks == 0 &&
        r.fixed_metadata_tail_blocks == 0;
    return r;
}

Result<void> finalize_metadata_only_shrink(BlockDevice&dev,
                                           std::uint64_t target_bytes, const FinalizeOptions&options,
                                           Progress*progress) {
    if (options.journal_path.empty())return Error{
        Errc::invalid_argument, 0,"shrink finalization requires --journal=FILE"
    }
    ;
    auto safe = require_offline_for_write(dev);
    if (!safe)return safe.error();
    auto ds = inspect_device_safety(dev);
    if (!ds)return ds.error();
    if (ds.value().block_device && !options.allow_block_device)return Error{
        Errc::unsafe, 0,"block-device shrink requires --allow-block-device"
    }
    ;
    auto ready = analyze_shrink_readiness(dev, target_bytes, progress);
    if (!ready)return ready.error();
    if (!ready.value().metadata_only_finalize_possible)
        return Error{
            Errc::unsupported, 0, ready.value().blockers.empty()
                ?"target requires unsupported block-group relocation":ready.value().blockers.front()
        }
    ;
    auto ctxr = load_metadata_context(dev);
    if (!ctxr)return ctxr.error();
    const auto ctx = ctxr.value();
    if ((ctx.sb.feature_incompat&detail::INCOMPAT_META_BG) != 0
        || (ctx.sb.feature_ro_compat&detail::RO_COMPAT_BIGALLOC) != 0)
        return Error{Errc::unsupported, 0,"metadata-only shrink refuses meta_bg and bigalloc"};
    auto j = TransactionJournal::open(options.journal_path, true);
    if (!j)return j.error();
    auto fu = parse_uuid(ctx.sb.uuid);
    if (!fu)return fu.error();
    if (j.value().status().identity.filesystem_uuid != fu.value()
        || j.value().status().identity.target_size != target_bytes)
        return Error{Errc::unsafe, 0,"journal identity does not match shrink target"};
    if (j.value().status().phase != OperationPhase::compacted &&
        j.value().status().phase != OperationPhase::staged)
        return Error{Errc::unsafe, 0, "journal is not ready for filesystem-size finalization"};
    auto sbraw = read_raw_superblock(dev);
    if (!sbraw)
        return sbraw.error();
    auto state = read_le<std::uint16_t>(sbraw.value(), 0x3a);
    write_le16(sbraw.value(), 0x3a, static_cast<std::uint16_t>(state & ~1U));
    auto cs = update_superblock_checksum(sbraw.value());
    if (!cs)return cs.error();
    auto ws = write_super_copies(dev, ctx, sbraw.value());
    if (!ws)return ws.error();
    JournalRecord dirty_record;
    dirty_record.type = JournalRecordType::filesystem_marked_dirty;
    auto dirty_append = j.value().append(dirty_record);
    if (!dirty_append)return dirty_append.error();
    auto cp = j.value().checkpoint(OperationPhase::metadata_finalize);
    if (!cp)return cp.error();
    const auto target_blocks = target_bytes/ctx.sb.block_size();
    const auto removed = ctx.sb.blocks_count-target_blocks;
    const auto last_group = ctx.sb.groups_count()-1;
    auto gd = detail::read_group_desc(dev, ctx.sb, last_group);
    if (!gd)return gd.error();
    auto gdr = read_group_descriptor_raw(dev, ctx, last_group);
    if (!gdr)return gdr.error();
    auto bm = detail::read_block_bitmap(dev, ctx.sb, gd.value());
    if (!bm)return bm.error();
    const auto gs = detail::group_start_block(ctx.sb, last_group);
    for (std::uint64_t b = target_blocks; b<ctx.sb.blocks_count; ++b) {
        if (b<gs)continue;
        const auto bit = b-gs;
        if (detail::bit_set(bm.value(), bit))return Error{
            Errc::corrupt, 0,"tail block became allocated after shrink readiness check"
        }
        ;
        set_bit(bm.value(), bit);
    }
    const auto oldfree = desc_free(gdr.value());
    if (removed>oldfree)return Error{
        Errc::corrupt, 0,"last group free count is smaller than removed tail"
    }
    ;
    set_desc_free(gdr.value(), static_cast<std::uint32_t>(oldfree-removed));
    auto bc = update_block_bitmap_checksum(ctx, gdr.value(), bm.value());
    if (!bc)return bc.error();
    auto gc = update_group_descriptor_checksum(ctx, last_group, gdr.value());
    if (!gc)return gc.error();
    auto wb = dev.write_exact(gd.value().block_bitmap*ctx.sb.block_size(), bm.value());
    if (!wb)return wb.error();
    auto wg = write_group_descriptor_raw(dev, ctx, last_group, gdr.value());
    if (!wg)return wg.error();
    auto bg = write_group_desc_backups(dev, ctx, sbraw.value(), last_group, gdr.value());
    if (!bg)return bg.error();
    auto fl = dev.flush();
    if (!fl)return fl.error();
    const auto old_free = ctx.sb.free_blocks;
    if (removed>old_free)return Error{
        Errc::corrupt, 0,"superblock free count smaller than shrink tail"
    }
    ;
    set_u64_lohi(sbraw.value(), 0x04, 0x150, target_blocks);
    set_u64_lohi(sbraw.value(), 0x0c, 0x158, old_free-removed);
    auto reserved = ctx.sb.reserved_blocks;
    if (reserved>target_blocks)reserved = target_blocks/20;
    set_u64_lohi(sbraw.value(), 0x08, 0x154, reserved);
    write_le16(sbraw.value(), 0x3a, static_cast<std::uint16_t>(state&~1U));
    cs = update_superblock_checksum(sbraw.value());
    if (!cs)return cs.error();
    ws = write_super_copies(dev, ctx, sbraw.value());
    if (!ws)return ws.error();
    auto vr = verify_metadata(dev, false, progress);
    if (!vr)return vr.error();
    if (!vr.value().clean)return Error{
        Errc::corrupt, 0,"post-shrink metadata verification failed; filesystem intentionally left unclean"
    }
    ;
    write_le16(sbraw.value(), 0x3a, static_cast<std::uint16_t>(state|1U));
    cs = update_superblock_checksum(sbraw.value());
    if (!cs)return cs.error();
    ws = write_super_copies(dev, ctx, sbraw.value());
    if (!ws)return ws.error();
    JournalRecord clean_record;
    clean_record.type = JournalRecordType::filesystem_marked_clean;
    auto ar = j.value().append(clean_record);
    if (!ar)return ar.error();
    cp = j.value().checkpoint(OperationPhase::filesystem_shrunk);
    if (!cp)return cp.error();
    if (progress)progress->update({
                                  "shrink-finalize","filesystem metadata", 1, 1, target_bytes, target_bytes, 1, 1
                                  }
                                 );
    return {};
}

Result<GrowReport> grow_within_last_group(BlockDevice&dev,
                                          std::uint64_t target_bytes, const GrowOptions&options, Progress*progress) {
    auto safe = require_offline_for_write(dev);
    if (!safe)return safe.error();
    auto ds = inspect_device_safety(dev);
    if (!ds)return ds.error();
    if (ds.value().block_device && !options.allow_block_device)return Error{
        Errc::unsafe, 0,"block-device grow requires --allow-block-device"
    }
    ;
    auto ctxr = load_metadata_context(dev);
    if (!ctxr)return ctxr.error();
    const auto ctx = ctxr.value();
    if ((ctx.sb.feature_incompat&detail::INCOMPAT_META_BG) != 0)return Error{
        Errc::unsupported, 0,"same-group grow refuses meta_bg"
    }
    ;
    if ((ctx.sb.feature_ro_compat&detail::RO_COMPAT_BIGALLOC) != 0)return Error{
        Errc::unsupported, 0,"same-group grow refuses bigalloc"
    }
    ;
    if ((ctx.sb.feature_incompat&detail::INCOMPAT_RECOVER) != 0)return Error{
        Errc::unsafe, 0,"filesystem journal recovery is required before grow"
    }
    ;
    if ((ctx.sb.state&1U) == 0)return Error{
        Errc::unsafe, 0,"filesystem is not marked clean; run a check before grow"
    }
    ;
    const auto bs = ctx.sb.block_size();
    if (target_bytes <= ctx.sb.size_bytes())return Error{
        Errc::invalid_argument, 0,"grow target must be larger than filesystem"
    }
    ;
    if (target_bytes>dev.geometry().size_bytes)return Error{
        Errc::invalid_argument, 0,"grow target exceeds containing device"
    }
    ;
    if (target_bytes%bs != 0)return Error{
        Errc::invalid_argument, 0,"grow target must be aligned to filesystem block size"
    }
    ;
    const auto target_blocks = target_bytes/bs;
    const auto data = target_blocks>ctx.sb.first_data_block?target_blocks-ctx.sb.first_data_block:0;
    const auto target_groups = ctx.sb.blocks_per_group?(data+ctx.sb.blocks_per_group-1)/ctx.sb.blocks_per_group:0;
    if (target_groups != ctx.sb.groups_count())return Error{
        Errc::unsupported, 0,"grow would add ext4 block groups; this release only grows within the existing last group"
    }
    ;
    const auto last_group = ctx.sb.groups_count()-1;
    const auto gs = detail::group_start_block(ctx.sb, last_group);
    const auto group_end = gs+ctx.sb.blocks_per_group;
    if (target_blocks>group_end)return Error{
        Errc::invalid_argument, 0,"grow target exceeds existing last block group"
    }
    ;
    auto gd = detail::read_group_desc(dev, ctx.sb, last_group);
    if (!gd)return gd.error();
    auto gdr = read_group_descriptor_raw(dev, ctx, last_group);
    if (!gdr)return gdr.error();
    auto bm = detail::read_block_bitmap(dev, ctx.sb, gd.value());
    if (!bm)return bm.error();
    const auto inode_table_blocks = (static_cast<std::uint64_t>(ctx.sb.inodes_per_group)*ctx.sb.inode_size+bs-1)/bs;
    auto is_fixed = [&](std::uint64_t b) {
        return b == gd.value().block_bitmap || b == gd.value().inode_bitmap
            || (b >= gd.value().inode_table && b<gd.value().inode_table+inode_table_blocks);
    }
    ;
    for (std::uint64_t b = ctx.sb.blocks_count; b<target_blocks; ++b) {
        if (b<gs)return Error{Errc::corrupt, 0,"grow range precedes last block group"};
        if (is_fixed(b))return Error{Errc::unsafe, 0,"grow range intersects fixed ext4 metadata"};
        const auto bit = b-gs;
        if (!detail::bit_set(bm.value(), bit))return Error{
            Errc::corrupt, 0,"block beyond current filesystem is not marked unavailable in last-group bitmap"
        }
        ;
    }
    auto sbraw = read_raw_superblock(dev);
    if (!sbraw)return sbraw.error();
    const auto original_state = read_le<std::uint16_t>(sbraw.value(), 0x3a);
    write_le16(sbraw.value(), 0x3a, static_cast<std::uint16_t>(original_state&~1U));
    auto c = update_superblock_checksum(sbraw.value());
    if (!c)return c.error();
    auto ws = write_super_copies(dev, ctx, sbraw.value());
    if (!ws)return ws.error();

    const auto added = target_blocks-ctx.sb.blocks_count;
    const auto old_desc_free = static_cast<std::uint64_t>(desc_free(gdr.value()));
    if (old_desc_free+added>std::numeric_limits<std::uint32_t>::max())return Error{
        Errc::corrupt, 0,"group free-block counter would overflow"
    }
    ;
    for (std::uint64_t b = ctx.sb.blocks_count; b<target_blocks; ++b)clear_bit(bm.value(), b-gs);
    set_desc_free(gdr.value(), static_cast<std::uint32_t>(old_desc_free+added));
    auto bc = update_block_bitmap_checksum(ctx, gdr.value(), bm.value());
    if (!bc)return bc.error();
    auto gc = update_group_descriptor_checksum(ctx, last_group, gdr.value());
    if (!gc)return gc.error();
    auto wb = dev.write_exact(gd.value().block_bitmap*bs, bm.value());
    if (!wb)return wb.error();
    auto wg = write_group_descriptor_raw(dev, ctx, last_group, gdr.value());
    if (!wg)return wg.error();
    auto bg = write_group_desc_backups(dev, ctx, sbraw.value(), last_group, gdr.value());
    if (!bg)return bg.error();
    auto fl = dev.flush();
    if (!fl)return fl.error();

    if (ctx.sb.free_blocks>std::numeric_limits<std::uint64_t>::max()-added)return Error{
        Errc::corrupt, 0,"superblock free-block counter would overflow"
    }
    ;
    set_u64_lohi(sbraw.value(), 0x04, 0x150, target_blocks);
    set_u64_lohi(sbraw.value(), 0x0c, 0x158, ctx.sb.free_blocks+added);
    write_le16(sbraw.value(), 0x3a, static_cast<std::uint16_t>(original_state&~1U));
    c = update_superblock_checksum(sbraw.value());
    if (!c)return c.error();
    ws = write_super_copies(dev, ctx, sbraw.value());
    if (!ws)return ws.error();

    GrowReport report;
    report.old_blocks = ctx.sb.blocks_count;
    report.new_blocks = target_blocks;
    report.blocks_added = added;
    report.free_blocks_after = ctx.sb.free_blocks+added;
    if (options.verify_after) {
        auto vr = verify_metadata(dev, false, progress);
        if (!vr)return vr.error();
        if (!vr.value().clean)return Error{
            Errc::corrupt, 0,"post-grow metadata verification failed; filesystem intentionally left unclean"
        }
        ;
        report.verified = true;
    }
    write_le16(sbraw.value(), 0x3a, static_cast<std::uint16_t>(original_state|1U));
    c = update_superblock_checksum(sbraw.value());
    if (!c)return c.error();
    ws = write_super_copies(dev, ctx, sbraw.value());
    if (!ws)return ws.error();
    if (progress)progress->update({"grow-finalize","filesystem metadata", 1, 1, target_bytes, target_bytes, 1, 1});
    return report;
}

}
