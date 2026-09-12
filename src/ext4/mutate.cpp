#include "fsx/ext4_mutate.hpp"
#include "fsx/cancel.hpp"
#include "fsx/crc.hpp"
#include "fsx/endian.hpp"
#include "fsx/ext4_metadata.hpp"
#include "fsx/extents.hpp"
#include "fsx/journal.hpp"
#include "fsx/safety.hpp"
#include "fsx/uuid.hpp"
#include "internal.hpp"
#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <unordered_map>
#include <vector>

namespace fsx::ext4 {
namespace {
constexpr std::size_t INODE_I_BLOCK_OFF = 40;
constexpr std::size_t EXTENT_START_HI = 6;
constexpr std::size_t EXTENT_START_LO = 8;
constexpr std::size_t INDEX_LEAF_LO = 4;
constexpr std::size_t INDEX_LEAF_HI = 8;

struct RangeState {
    bool all_set{true};
    bool all_clear{true};
};

void set_desc_free_blocks(std::span<std::byte> gd, std::uint32_t v) {
    write_le16(gd, 12, static_cast<std::uint16_t>(v));
    if (gd.size() >= 46) write_le16(gd, 44, static_cast<std::uint16_t>(v>>16U));
}

void set_bitmap_bit(std::span<std::byte> bm, std::uint64_t bit, bool value) {
    const auto byte = static_cast<std::size_t>(bit>>3U);
    const auto shift = static_cast<unsigned>(bit&7U);
    auto v = std::to_integer<std::uint8_t>(bm[byte]);
    if (value)v = static_cast<std::uint8_t>(v|static_cast<std::uint8_t>(1U<<shift));
    else v = static_cast<std::uint8_t>(v&static_cast<std::uint8_t>(~(1U<<shift)));
    bm[byte] = std::byte(v);
}

Result<RangeState> allocation_state(BlockDevice& dev,
                                    const MetadataContext& ctx, std::uint64_t start, std::uint64_t length) {
    if (length == 0 || start >= ctx.sb.blocks_count || length>ctx.sb.blocks_count-start)
        return Error{Errc::invalid_argument, 0,"allocation range lies outside filesystem"};
    RangeState out;
    std::uint64_t pos = start, left = length;
    while (left) {
        if (pos<ctx.sb.first_data_block)return Error{
            Errc::unsafe, 0,"allocation range reaches reserved pre-data blocks"
        }
        ;
        const auto group = (pos-ctx.sb.first_data_block)/ctx.sb.blocks_per_group;
        const auto group_start = detail::group_start_block(ctx.sb, group);
        const auto idx = pos-group_start;
        const auto n = std::min<std::uint64_t>(left, detail::group_block_count(ctx.sb, group)-idx);
        auto gd = detail::read_group_desc(dev, ctx.sb, group);
        if (!gd)return gd.error();
        auto bm = detail::read_block_bitmap(dev, ctx.sb, gd.value());
        if (!bm)return bm.error();
        for (std::uint64_t i = 0; i<n; ++i) {
            const bool set = detail::bit_set(bm.value(), idx+i);
            out.all_set&=set;
            out.all_clear&=!set;
        }
        pos+=n;
        left-=n;
    }
    return out;
}

Result<void> rewrite_group_bitmap(BlockDevice& dev, const MetadataContext& ctx, std::uint64_t group,
                                  std::uint64_t first, std::uint64_t count, bool allocate) {
    auto gd = detail::read_group_desc(dev, ctx.sb, group);
    if (!gd)return gd.error();
    auto gd_raw = read_group_descriptor_raw(dev, ctx, group);
    if (!gd_raw)return gd_raw.error();
    auto gd_ok = verify_group_descriptor_checksum(ctx, group, gd_raw.value());
    if (!gd_ok)return gd_ok.error();
    if (!gd_ok.value())return Error{
        Errc::corrupt, 0,"group descriptor checksum mismatch before allocation update"
    }
    ;
    auto bm = detail::read_block_bitmap(dev, ctx.sb, gd.value());
    if (!bm)return bm.error();
    auto bm_ok = verify_block_bitmap_checksum(ctx, gd_raw.value(), bm.value());
    if (!bm_ok)return bm_ok.error();
    if (!bm_ok.value())return Error{Errc::corrupt, 0,"block bitmap checksum mismatch before allocation update"};
    const auto valid = detail::group_block_count(ctx.sb, group);
    if (first>valid || count>valid-first)return Error{
        Errc::corrupt, 0,"bitmap update exceeds valid group blocks"
    }
    ;
    for (std::uint64_t i = 0; i<count; ++i)set_bitmap_bit(bm.value(), first+i, allocate);
    std::uint64_t allocated = 0;
    for (std::uint64_t i = 0; i<valid; ++i)if (detail::bit_set(bm.value(), i))++allocated;
    const auto free = valid-allocated;
    if (free>std::numeric_limits<std::uint32_t>::max())return Error{
        Errc::internal, 0,"group free count overflow"
    }
    ;
    set_desc_free_blocks(gd_raw.value(), static_cast<std::uint32_t>(free));
    auto bc = update_block_bitmap_checksum(ctx, gd_raw.value(), bm.value());
    if (!bc)return bc.error();
    auto gc = update_group_descriptor_checksum(ctx, group, gd_raw.value());
    if (!gc)return gc.error();
    auto wr = dev.write_exact(gd.value().block_bitmap*ctx.sb.block_size(), bm.value());
    if (!wr)return wr.error();
    auto wg = write_group_descriptor_raw(dev, ctx, group, gd_raw.value());
    if (!wg)return wg.error();
    return {};
}

Result<void> set_allocation_range(BlockDevice& dev, const MetadataContext& ctx,
                                  std::uint64_t start, std::uint64_t length, bool allocate) {
    if (length == 0)return {};
    std::uint64_t pos = start, left = length;
    while (left) {
        if (pos<ctx.sb.first_data_block)return Error{
            Errc::unsafe, 0,"allocation update reaches reserved pre-data blocks"
        }
        ;
        const auto group = (pos-ctx.sb.first_data_block)/ctx.sb.blocks_per_group;
        const auto group_start = detail::group_start_block(ctx.sb, group);
        const auto idx = pos-group_start;
        const auto n = std::min<std::uint64_t>(left, detail::group_block_count(ctx.sb, group)-idx);
        auto r = rewrite_group_bitmap(dev, ctx, group, idx, n, allocate);
        if (!r)return r.error();
        pos+=n;
        left-=n;
    }
    return dev.flush();
}

Result<std::uint32_t> extent_crc(BlockDevice& dev, std::uint64_t block, std::uint64_t count, std::uint64_t bs) {
    constexpr std::size_t chunk = 4U*1024U*1024U;
    if (bs == 0)return Error{Errc::corrupt, 0,"zero block size while checksumming extent"};
    if (count == 0)return crc32c(std::span<const std::byte>{})^0xffffffffU;
    if (count>std::numeric_limits<std::uint64_t>::max()/bs
        || block>std::numeric_limits<std::uint64_t>::max()/bs)
        return Error{Errc::corrupt, 0,"extent byte geometry overflows"};
    const auto total = count*bs;
    const auto start = block*bs;
    const auto dev_size = dev.geometry().size_bytes;
    if (start>dev_size || total>dev_size-start)
        return Error{Errc::corrupt, 0,"extent lies outside containing device"};
    const auto buffer_bytes = static_cast<std::size_t>(std::min<std::uint64_t>(chunk, total));
    if (buffer_bytes == 0)return Error{Errc::internal, 0,"invalid zero-sized extent checksum buffer"};
    std::vector<std::byte> buf(buffer_bytes);
    std::uint64_t done = 0;
    std::uint32_t crc = 0xffffffffU;
    while (done<total) {
        const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(buf.size(), total-done));
        auto sp = std::span<std::byte>(buf).first(n);
        auto rr = dev.read_exact(start+done, sp);
        if (!rr)return rr.error();
        crc = crc32c(sp, crc);
        done+=n;
    }
    return crc^0xffffffffU;
}

Result<void> mark_filesystem_state(BlockDevice& dev, bool clean) {
    auto raw = read_raw_superblock(dev);
    if (!raw)return raw.error();
    auto state = read_le<std::uint16_t>(raw.value(), 0x3a);
    if (clean)state = static_cast<std::uint16_t>(state|0x0001U);
    else state = static_cast<std::uint16_t>(state&~0x0001U);
    write_le16(raw.value(), 0x3a, state);
    auto c = update_superblock_checksum(raw.value());
    if (!c)return c.error();
    auto wr = write_raw_superblock(dev, raw.value());
    if (!wr)return wr.error();
    return dev.flush();
}

Result<void> update_data_owner(BlockDevice& dev,
                               const MetadataContext& ctx, const OwnerMove& move, std::uint64_t new_block) {
    auto found = find_inode_extent(dev, move.inode, move.logical_block);
    if (!found)return found.error();
    const auto e = found.value();
    if (e.logical_block != move.logical_block || e.length != move.block_count)
        return Error{Errc::unsafe, 0,"extent geometry changed since relocation plan"};
    if (e.physical_block == new_block)return {};
    if (e.physical_block != move.source_block)return Error{
        Errc::unsafe, 0,"extent owner points to neither source nor destination"
    }
    ;
    const auto hi = static_cast<std::uint16_t>(new_block>>32U);
    const auto lo = static_cast<std::uint32_t>(new_block);
    if (e.reference.kind == ExtentRefKind::inode_root) {
        auto inode = read_inode_raw(dev, ctx, move.inode);
        if (!inode)return inode.error();
        const auto off = INODE_I_BLOCK_OFF+static_cast<std::size_t>(e.reference.entry_offset);
        if (off+12>inode.value().size())return Error{Errc::corrupt, 0,"extent entry exceeds inode"};
        write_le16(inode.value(), off+EXTENT_START_HI, hi);
        write_le32(inode.value(), off+EXTENT_START_LO, lo);
        auto c = update_inode_checksum(ctx, move.inode, inode.value());
        if (!c)return c.error();
        auto wr = write_inode_raw(dev, ctx, move.inode, inode.value());
        if (!wr)return wr.error();
    } else {
        std::vector<std::byte> block(static_cast<std::size_t>(ctx.sb.block_size()));
        auto rr = dev.read_exact(e.reference.container_block*ctx.sb.block_size(), block);
        if (!rr)return rr.error();
        auto ok = verify_extent_block_checksum(ctx, move.inode, e.inode_generation, block);
        if (!ok)return ok.error();
        if (!ok.value())return Error{
            Errc::corrupt, 0,"extent block checksum mismatch before owner switch"
        }
        ;
        const auto off = static_cast<std::size_t>(e.reference.entry_offset);
        if (off+12>block.size())return Error{
            Errc::corrupt, 0,"extent entry exceeds extent block"
        }
        ;
        write_le16(block, off+EXTENT_START_HI, hi);
        write_le32(block, off+EXTENT_START_LO, lo);
        auto c = update_extent_block_checksum(ctx, move.inode, e.inode_generation, block);
        if (!c)return c.error();
        auto wr = dev.write_exact(e.reference.container_block*ctx.sb.block_size(), block);
        if (!wr)return wr.error();
    }
    return dev.flush();
}

Result<std::uint64_t> current_data_owner(BlockDevice& dev, const OwnerMove& move) {
    auto e = find_inode_extent(dev, move.inode, move.logical_block);
    if (!e)return e.error();
    if (e.value().logical_block != move.logical_block || e.value().length != move.block_count)return Error{
        Errc::unsafe, 0,"extent geometry changed since plan"
    }
    ;
    return e.value().physical_block;
}

struct TreeLocation { ExtentTreeBlock ref{};
    std::uint32_t generation{};
};
Result<TreeLocation> find_tree_location(BlockDevice& dev, const MetadataContext& ctx, const OwnerMove& move) {
    auto scan = scan_inode_extent_tree(dev, move.inode, 0);
    if (!scan)return scan.error();
    for (const auto&t:scan.value().tree_blocks)if (t.physical_block == move.source_block
                                                   || t.physical_block == move.destination_block) {
        auto inode = read_inode_raw(dev, ctx, move.inode);
        if (!inode)return inode.error();
        return TreeLocation{t, read_le<std::uint32_t>(inode.value(), 0x64)};
    }
    return Error{Errc::not_found, 0,"extent tree block is not reachable from inode"};
}

Result<void> update_tree_parent(BlockDevice& dev,
                                const MetadataContext& ctx, const OwnerMove& move, std::uint64_t new_block) {
    auto loc = find_tree_location(dev, ctx, move);
    if (!loc)return loc.error();
    if (loc.value().ref.physical_block == new_block)return {};
    if (loc.value().ref.physical_block != move.source_block)return Error{
        Errc::unsafe, 0,"tree owner points to neither source nor destination"
    }
    ;
    const auto hi = static_cast<std::uint16_t>(new_block>>32U);
    const auto lo = static_cast<std::uint32_t>(new_block);
    const auto bs = ctx.sb.block_size();
    const auto parent_abs = loc.value().ref.parent_container_byte_offset;
    if (parent_abs%bs == 0) {
        const auto block_no = parent_abs/bs;
        std::vector<std::byte> block(static_cast<std::size_t>(bs));
        auto rr = dev.read_exact(parent_abs, block);
        if (!rr)return rr.error();
        auto ok = verify_extent_block_checksum(ctx, move.inode, loc.value().generation, block);
        if (!ok)return ok.error();
        if (!ok.value())return Error{
            Errc::corrupt, 0,"parent extent block checksum mismatch"
        }
        ;
        const auto off = static_cast<std::size_t>(loc.value().ref.parent_entry_offset);
        if (off+12>block.size())return Error{
            Errc::corrupt, 0,"extent index exceeds parent block"
        }
        ;
        write_le32(block, off+INDEX_LEAF_LO, lo);
        write_le16(block, off+INDEX_LEAF_HI, hi);
        auto c = update_extent_block_checksum(ctx, move.inode, loc.value().generation, block);
        if (!c)return c.error();
        auto wr = dev.write_exact(block_no*bs, block);
        if (!wr)return wr.error();
    } else {
        auto inode = read_inode_raw(dev, ctx, move.inode);
        if (!inode)return inode.error();
        const auto off = INODE_I_BLOCK_OFF+static_cast<std::size_t>(loc.value().ref.parent_entry_offset);
        if (off+12>inode.value().size())return Error{
            Errc::corrupt, 0,"extent index exceeds inode root"
        }
        ;
        write_le32(inode.value(), off+INDEX_LEAF_LO, lo);
        write_le16(inode.value(), off+INDEX_LEAF_HI, hi);
        auto c = update_inode_checksum(ctx, move.inode, inode.value());
        if (!c)return c.error();
        auto wr = write_inode_raw(dev, ctx, move.inode, inode.value());
        if (!wr)return wr.error();
    }
    return dev.flush();
}

Result<std::uint64_t> current_tree_owner(BlockDevice& dev, const MetadataContext& ctx, const OwnerMove& move) {
    auto x = find_tree_location(dev, ctx, move);
    if (!x)return x.error();
    return x.value().ref.physical_block;
}

Result<void> refresh_tree_copy(BlockDevice& dev, const MetadataContext& ctx, const OwnerMove& move) {
    std::vector<std::byte> b(static_cast<std::size_t>(ctx.sb.block_size()));
    auto rr = dev.read_exact(move.source_block*ctx.sb.block_size(), b);
    if (!rr)return rr.error();
    auto inode = read_inode_raw(dev, ctx, move.inode);
    if (!inode)return inode.error();
    const auto gen = read_le<std::uint32_t>(inode.value(), 0x64);
    auto ok = verify_extent_block_checksum(ctx, move.inode, gen, b);
    if (!ok)return ok.error();
    if (!ok.value())return Error{
        Errc::corrupt, 0,"source extent tree block checksum mismatch"
    }
    ;
    auto wr = dev.write_exact(move.destination_block*ctx.sb.block_size(), b);
    if (!wr)return wr.error();
    auto fl = dev.flush();
    if (!fl)return fl.error();
    std::vector<std::byte> v(b.size());
    rr = dev.read_exact(move.destination_block*ctx.sb.block_size(), v);
    if (!rr)return rr.error();
    if (v != b)return Error{
        Errc::io, 0,"extent tree block verification mismatch"
    }
    ;
    return {};
}

JournalRecord move_record(JournalRecordType type, const OwnerMove& m) {
    JournalRecord r;
    r.type = type;
    r.transaction_id = m.transaction_id;
    r.source_block = m.source_block;
    r.destination_block = m.destination_block;
    r.block_count = m.block_count;
    r.inode = m.inode;
    r.aux = m.logical_block;
    r.state = static_cast<std::uint32_t>(m.kind);
    return r;
}
}

Result<CommitReport> commit_staged_relocation(BlockDevice& dev,
                                              const OwnerRelocationPlan& plan, const CommitOptions& options,
                                              Progress* progress) {
    if (options.journal_path.empty())return Error{Errc::invalid_argument, 0,"commit requires --journal=FILE"};
    auto safe = require_offline_for_write(dev);
    if (!safe)return safe.error();
    auto ds = inspect_device_safety(dev);
    if (!ds)return ds.error();
    if (ds.value().block_device && !options.allow_block_device)return Error{
        Errc::unsafe, 0,"block-device metadata commit requires explicit --allow-block-device"
    }
    ;
    auto ctxr = load_metadata_context(dev);
    if (!ctxr)return ctxr.error();
    const auto ctx = ctxr.value();
    if ((ctx.sb.feature_ro_compat&detail::RO_COMPAT_BIGALLOC) != 0)return Error{
        Errc::unsupported, 0,"bigalloc metadata mutation is not supported"
    }
    ;
    if ((ctx.sb.feature_incompat&detail::INCOMPAT_META_BG) != 0)return Error{
        Errc::unsupported, 0,"meta_bg metadata mutation is not supported"
    }
    ;
    if ((ctx.sb.feature_incompat&detail::INCOMPAT_RECOVER) != 0)return Error{
        Errc::unsafe, 0,"filesystem journal recovery is required before metadata commit"
    }
    ;
    if (!std::filesystem::exists(options.journal_path))return Error{
        Errc::not_found, 0,"relocation journal does not exist"
    }
    ;
    auto jr = TransactionJournal::open(options.journal_path, true);
    if (!jr)return jr.error();
    auto journal = std::move(jr.value());
    auto fu = parse_uuid(ctx.sb.uuid);
    if (!fu)return fu.error();
    if (journal.status().identity.filesystem_uuid != fu.value())return Error{
        Errc::unsafe, 0,"journal filesystem UUID does not match target"
    }
    ;
    if (journal.status().identity.device_size != dev.geometry().size_bytes)return Error{
        Errc::unsafe, 0,"journal device size does not match target"
    }
    ;
    if (journal.status().identity.target_size != plan.target_bytes)return Error{
        Errc::unsafe, 0,"journal target differs from current plan"
    }
    ;
    if (journal.status().phase != OperationPhase::staged
        && journal.status().phase != OperationPhase::metadata_finalize
        && journal.status().phase != OperationPhase::metadata_paused
        && journal.status().phase != OperationPhase::compacted)
        return Error{Errc::unsafe, 0,"journal is not in a metadata-commit compatible phase"};
    auto records = journal.records();
    if (!records)return records.error();
    std::unordered_map<std::uint64_t, JournalRecord> verified;
    for (const auto&r:records.value())if (r.type == JournalRecordType::copy_verified)verified[r.transaction_id] = r;
    if (verified.size()<plan.moves.size())return Error{
        Errc::unsafe, 0,"not every relocation has a durable verified-copy record"
    }
    ;
    for (const auto&m:plan.moves) {
        auto it = verified.find(m.transaction_id);
        if (it == verified.end() || it->second.source_block != m.source_block
            || it->second.destination_block != m.destination_block
            || it->second.block_count != m.block_count || it->second.inode != m.inode)return Error{
            Errc::unsafe, 0,"journal relocation geometry differs from current plan"
        }
        ;
    }

    if (journal.status().phase == OperationPhase::compacted) {
        CommitReport r;
        r.moves_total = plan.moves.size();
        r.moves_committed = plan.moves.size();
        r.sources_released = plan.moves.size();
        r.owners_switched = plan.moves.size();
        r.destinations_reserved = plan.moves.size();
        for (const auto&m:plan.moves)r.bytes_relocated+=m.block_count*plan.block_size;
        r.complete = true;
        r.filesystem_clean = (ctx.sb.state&1U) != 0;
        return r;
    }

    if ((ctx.sb.state&1U) != 0) {
        auto md = mark_filesystem_state(dev, false);
        if (!md)return md.error();
        auto ar = journal.append(move_record(JournalRecordType::filesystem_marked_dirty, OwnerMove{
                                             }
                                            ));
        if (!ar)return ar.error();
    }
    auto cp = journal.checkpoint(OperationPhase::metadata_finalize);
    if (!cp)return cp.error();

    CommitReport report;
    report.moves_total = plan.moves.size();
    std::vector<OwnerMove> ordered = plan.moves;
    std::stable_sort(ordered.begin(), ordered.end(), [](const OwnerMove&a, const OwnerMove&b) {
                     if (a.kind != b.kind) return a.kind == RelocationKind::data_extent;
                     if (a.kind == RelocationKind::extent_tree_block) return a.tree_depth<b.tree_depth;
                     return a.transaction_id<b.transaction_id;
                     });
    for (const auto&m:ordered) {
        if (Cancellation::requested()) {
            auto c = journal.checkpoint(OperationPhase::metadata_paused);
            if (!c)return c.error();
            report.paused = true;
            return report;
        }
        auto src_state = allocation_state(dev, ctx, m.source_block, m.block_count);
        if (!src_state)return src_state.error();
        auto dst_state = allocation_state(dev, ctx, m.destination_block, m.block_count);
        if (!dst_state)return dst_state.error();
        auto owner = (m.kind == RelocationKind::data_extent)?current_data_owner(dev,
                                                                                m):current_tree_owner(dev, ctx, m);
        if (!owner)return owner.error();
        if (owner.value() != m.source_block && owner.value() != m.destination_block)return Error{
            Errc::unsafe, 0,"owner mapping is inconsistent with relocation journal"
        }
        ;
        if (owner.value() == m.source_block && !src_state.value().all_set)return Error{
            Errc::corrupt, 0,"authoritative source extent is not fully allocated"
        }
        ;
        if (owner.value() == m.destination_block && !dst_state.value().all_set)return Error{
            Errc::corrupt, 0,"authoritative destination extent is not fully allocated"
        }
        ;
        if (!dst_state.value().all_set) {
            if (!dst_state.value().all_clear)return Error{
                Errc::corrupt, 0,"destination allocation range is partially allocated"
            }
            ;
            auto set = set_allocation_range(dev, ctx, m.destination_block, m.block_count, true);
            if (!set)return set.error();
            auto ar = journal.append(move_record(JournalRecordType::destination_reserved, m));
            if (!ar)return ar.error();
            ++report.destinations_reserved;
        }
        else ++report.destinations_reserved;
        if (owner.value() == m.source_block) {
            if (m.kind == RelocationKind::data_extent) {
                if (!m.unwritten) {
                    auto crc = extent_crc(dev, m.destination_block, m.block_count, plan.block_size);
                    if (!crc)return crc.error();
                    if (crc.value() != verified[m.transaction_id].data_crc32c)return Error{
                        Errc::io, 0,"staged destination checksum changed before metadata commit"
                    }
                    ;
                }
                auto sw = update_data_owner(dev, ctx, m, m.destination_block);
                if (!sw)return sw.error();
            }
            else {
                auto rc = refresh_tree_copy(dev, ctx, m);
                if (!rc)return rc.error();
                auto sw = update_tree_parent(dev, ctx, m, m.destination_block);
                if (!sw)return sw.error();
            }
            auto ar = journal.append(move_record(JournalRecordType::owner_switched, m));
            if (!ar)return ar.error();
            ++report.owners_switched;
        } else ++report.owners_switched;
        src_state = allocation_state(dev, ctx, m.source_block, m.block_count);
        if (!src_state)return src_state.error();
        if (!src_state.value().all_clear) {
            if (!src_state.value().all_set)return Error{
                Errc::corrupt, 0,"source allocation range is partially released"
            }
            ;
            auto clr = set_allocation_range(dev, ctx, m.source_block, m.block_count, false);
            if (!clr)return clr.error();
            auto ar = journal.append(move_record(JournalRecordType::source_released, m));
            if (!ar)return ar.error();
            ++report.sources_released;
        }
        else ++report.sources_released;
        auto done = journal.append(move_record(JournalRecordType::metadata_committed, m));
        if (!done)return done.error();
        ++report.moves_committed;
        report.bytes_relocated+=m.block_count*plan.block_size;
        if (progress)progress->update({
                                      "metadata-commit",
                                      "extent " + std::to_string(report.moves_committed) + "/"
                                          + std::to_string(report.moves_total),
                                      report.moves_committed, report.moves_total, report.bytes_relocated,
                                      report.bytes_relocated, report.moves_committed, report.moves_total
                                      }
                                     );
    }

    if (options.verify_metadata_after) {
        auto v = verify_metadata(dev, false, progress);
        if (!v)return v.error();
        if (!v.value().clean)return Error{
            Errc::corrupt, 0,"post-commit metadata verification failed; filesystem intentionally left unclean"
        }
        ;
    }
    if (!options.leave_filesystem_dirty) {
        auto cl = mark_filesystem_state(dev, true);
        if (!cl)return cl.error();
        auto ar = journal.append(move_record(JournalRecordType::filesystem_marked_clean, OwnerMove{
                                             }
                                            ));
        if (!ar)return ar.error();
        report.filesystem_clean = true;
    }
    auto end = journal.checkpoint(OperationPhase::compacted);
    if (!end)return end.error();
    report.complete = true;
    return report;
}

} // namespace fsx::ext4
