#include "fsx/relocate.hpp"
#include "fsx/allocation.hpp"
#include "fsx/cancel.hpp"
#include "fsx/crc.hpp"
#include "fsx/io_scheduler.hpp"
#include "fsx/ext4_metadata.hpp"
#include "fsx/safety.hpp"
#include "fsx/task_pool.hpp"
#include "fsx/uuid.hpp"
#include "internal.hpp"
#include <algorithm>
#include <filesystem>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace fsx::ext4 {
namespace {
struct FreeSlot { std::uint64_t start{};
    std::uint64_t length{};
};

bool allocate_best_fit(std::vector<FreeSlot>& slots, std::uint64_t length, std::uint64_t& out) {
    std::size_t best = slots.size();
    std::uint64_t waste = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t i = 0; i<slots.size(); ++i) {
        if (slots[i].length<length)continue;
        const auto w = slots[i].length-length;
        if (w<waste) {
            best = i;
            waste = w;
            if (w == 0)break;
        }
    }
    if (best == slots.size())return false;
    out = slots[best].start;
    slots[best].start+=length;
    slots[best].length-=length;
    if (slots[best].length == 0)slots.erase(slots.begin()+static_cast<std::ptrdiff_t>(best));
    return true;
}

struct CopyResult { std::uint32_t crc{};
    std::uint64_t bytes{};
};

Result<std::pair<std::uint64_t, std::uint64_t>> byte_range(const BlockDevice& dev,
                                                           std::uint64_t block,
                                                           std::uint64_t count,
                                                           std::uint64_t bs) {
    if (bs == 0 || count == 0)return Error{Errc::invalid_argument, 0,"invalid zero-sized relocation range"};
    if (block>std::numeric_limits<std::uint64_t>::max()/bs
        || count>std::numeric_limits<std::uint64_t>::max()/bs)
        return Error{Errc::corrupt, 0,"relocation byte geometry overflows"};
    const auto start = block*bs;
    const auto total = count*bs;
    const auto size = dev.geometry().size_bytes;
    if (start>size || total>size-start)return Error{
        Errc::corrupt, 0,"relocation range lies outside containing device"
    }
    ;
    return std::pair<std::uint64_t, std::uint64_t>{start, total};
}

Result<CopyResult> copy_one(BlockDevice& dev, const OwnerMove& m, std::uint64_t bs, std::size_t chunk_bytes) {
    if (m.unwritten) return CopyResult{0, 0};
    auto src_range = byte_range(dev, m.source_block, m.block_count, bs);
    if (!src_range)return src_range.error();
    auto dst_range = byte_range(dev, m.destination_block, m.block_count, bs);
    if (!dst_range)return dst_range.error();
    const auto total = src_range.value().second;
    const auto src = src_range.value().first;
    const auto dst = dst_range.value().first;
    std::uint64_t done = 0;
    std::uint32_t crc = 0xffffffffU;
    std::vector<std::byte> buf(std::max<std::size_t>(bs, chunk_bytes));
    while (done<total) {
        if (Cancellation::requested()) return Error{Errc::unsafe, 0,"relocation cancellation requested"};
        const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(buf.size(), total-done));
        auto span = std::span<std::byte>(buf).first(n);
        auto rr = dev.read_exact(src+done, span);
        if (!rr)return rr.error();
        crc = crc32c(span, crc);
        auto wr = dev.write_exact(dst+done, std::span<const std::byte>(span));
        if (!wr)return wr.error();
        done+=n;
    }
    return CopyResult{crc^0xffffffffU, total};
}

Result<std::uint32_t> verify_one(BlockDevice& dev, const OwnerMove&m, std::uint64_t bs, std::size_t chunk_bytes) {
    if (m.unwritten)return std::uint32_t{0};
    auto dst_range = byte_range(dev, m.destination_block, m.block_count, bs);
    if (!dst_range)return dst_range.error();
    const auto total = dst_range.value().second;
    const auto dst = dst_range.value().first;
    std::uint64_t done = 0;
    std::uint32_t crc = 0xffffffffU;
    std::vector<std::byte> buf(std::max<std::size_t>(bs, chunk_bytes));
    while (done<total) {
        const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(buf.size(), total-done));
        auto span = std::span<std::byte>(buf).first(n);
        auto rr = dev.read_exact(dst+done, span);
        if (!rr)return rr.error();
        crc = crc32c(span, crc);
        done+=n;
    }
    return crc^0xffffffffU;
}

std::uint64_t sum_tail_overlap(const std::vector<OwnedExtent>& extents, std::uint64_t target) {
    std::uint64_t n = 0;
    for (const auto&e:extents) {
        if (e.end() <= target)continue;
        n+=e.end()-std::max(e.physical_block, target);
    }
    return n;
}


bool range_fully_allocated(const std::vector<BlockExtent>& allocated,
                           std::uint64_t start,
                           std::uint64_t length) {
    if (length == 0) return true;
    if (length > std::numeric_limits<std::uint64_t>::max() - start) return false;
    const auto end = start + length;
    auto it = std::lower_bound(allocated.begin(), allocated.end(), start,
                               [](const BlockExtent& e, std::uint64_t block) {
                               return e.end() <= block;
                               });
    std::uint64_t covered = start;
    while (it != allocated.end() && it->start <= covered) {
        if (it->end() > covered) covered = it->end();
        if (covered >= end) return true;
        ++it;
    }
    return false;
}

Result<void> validate_owner_ranges(const OwnerScan& owners) {
    struct R { std::uint64_t start;
        std::uint64_t end;
        std::uint64_t inode;
        bool tree;
    };
    std::vector<R> ranges;
    ranges.reserve(owners.extents.size() + owners.tree_blocks.size());
    for (const auto& e : owners.extents)
        ranges.push_back({e.physical_block, e.end(), e.inode, false});
    for (const auto& t : owners.tree_blocks)
        ranges.push_back({t.physical_block, t.physical_block + 1, t.inode, true});
    std::sort(ranges.begin(), ranges.end(), [](const R& a, const R& b) {
              if (a.start != b.start) return a.start < b.start;
              if (a.end != b.end) return a.end < b.end;
              if (a.inode != b.inode) return a.inode < b.inode;
              return a.tree < b.tree;
              });
    for (std::size_t i = 1; i < ranges.size(); ++i) {
        if (ranges[i].start < ranges[i - 1].end)
            return Error{Errc::corrupt, 0,
                "physical block ownership overlaps between inode " +
                    std::to_string(ranges[i - 1].inode) + " and inode " +
                    std::to_string(ranges[i].inode)};
    }
    return {};
}
}

Result<OwnerRelocationPlan> build_owner_relocation_plan(BlockDevice& dev,
                                                        std::uint64_t target_bytes, Progress* progress) {
    auto sr = read_superblock(dev);
    if (!sr)return sr.error();
    const auto sb = sr.value();
    if (target_bytes >= sb.size_bytes())return Error{
        Errc::invalid_argument, 0,"target is not smaller than filesystem"
    }
    ;
    const auto target_blocks = target_bytes/sb.block_size();
    if (target_blocks <= sb.first_data_block)return Error{Errc::invalid_argument, 0,"target is too small"};

    auto graph = build_allocation_graph(dev, progress, nullptr);
    if (!graph)return graph.error();
    auto owners = scan_extent_owners(dev, target_blocks, progress);
    if (!owners)return owners.error();
    auto ownership_ok = validate_owner_ranges(owners.value());
    if (!ownership_ok)return ownership_ok.error();

    OwnerRelocationPlan out;
    out.block_size = sb.block_size();
    out.target_blocks = target_blocks;
    out.target_bytes = target_blocks*sb.block_size();
    const auto groups = sb.groups_count();
    for (std::uint64_t g = 0; g<groups; ++g) {
        auto gd = detail::read_group_desc(dev, sb, g);
        if (!gd)return gd.error();
        auto mark = [&](std::uint64_t block) {
            if (block >= target_blocks)++out.fixed_metadata_blocks_above_target;
        };
        mark(gd.value().block_bitmap);
        mark(gd.value().inode_bitmap);
        const std::uint64_t it_blocks =
            (static_cast<std::uint64_t>(sb.inodes_per_group) * sb.inode_size + sb.block_size() - 1)
            / sb.block_size();
        if (!range_fully_allocated(graph.value().allocated, gd.value().block_bitmap, 1) ||
            !range_fully_allocated(graph.value().allocated, gd.value().inode_bitmap, 1) ||
            !range_fully_allocated(graph.value().allocated, gd.value().inode_table, it_blocks))
            return Error{
                Errc::corrupt, 0,"group "+std::to_string(g)+" fixed metadata is not fully allocated in block bitmap"
            }
        ;
        if (gd.value().inode_table >= target_blocks)out.fixed_metadata_blocks_above_target+=it_blocks;
        else if (gd.value().inode_table + it_blocks > target_blocks) {
            out.fixed_metadata_blocks_above_target += gd.value().inode_table + it_blocks - target_blocks;
        }
    }
    out.legacy_blockmap_inodes = owners.value().legacy_blockmap_inodes;
    out.allocated_inodes = owners.value().allocated_inodes;
    out.extent_inodes = owners.value().extent_inodes;

    std::vector<FreeSlot> free;
    for (const auto&e:graph.value().free) {
        if (e.start >= target_blocks)break;
        const auto end = std::min(e.end(), target_blocks);
        if (end>e.start)free.push_back({
                                       e.start, end-e.start
                                       }
                                      );
    }
    std::sort(free.begin(), free.end(), [](const FreeSlot&a, const FreeSlot&b) {
              return a.length<b.length; });

    std::vector<OwnedExtent> data;
    for (const auto&e:owners.value().extents)if (e.end()>target_blocks)data.push_back(e);
    std::sort(data.begin(), data.end(), [](const OwnedExtent&a, const OwnedExtent&b) {
              return a.length>b.length; });
    std::vector<ExtentTreeBlock> trees;
    for (const auto&t:owners.value().tree_blocks)if (t.physical_block >= target_blocks)trees.push_back(t);

    std::uint64_t txn = 1;
    bool all_placed = true;
    for (const auto&e:data) {
        std::uint64_t dest = 0;
        if (!allocate_best_fit(free, e.length, dest)) {
            all_placed = false;
            break;
        }
        out.moves.push_back({
                            RelocationKind::data_extent, txn++, e.inode, e.logical_block, e.physical_block,
                            dest, e.length, e.unwritten
                            }
                           );
        out.data_blocks_to_move+=e.length;
    }
    if (all_placed) {
        for (const auto&t:trees) {
            std::uint64_t dest = 0;
            if (!allocate_best_fit(free, 1, dest)) {
                all_placed = false;
                break;
            }
            out.moves.push_back({
                                RelocationKind::extent_tree_block, txn++, t.inode, 0, t.physical_block, dest,
                                1, false, t.depth
                                }
                               );
            ++out.tree_blocks_to_move;
        }
    }
    out.destinations_available = all_placed;

    std::uint64_t allocated_tail = 0;
    for (const auto&e:graph.value().allocated) {
        if (e.end()>target_blocks)allocated_tail+=e.end()-std::max(e.start, target_blocks);
    }
    const auto data_tail = sum_tail_overlap(owners.value().extents, target_blocks);
    const auto tree_tail = static_cast<std::uint64_t>(trees.size());
    const auto known = std::min<std::uint64_t>(allocated_tail,
                                               data_tail+tree_tail+out.fixed_metadata_blocks_above_target);
    out.unknown_allocated_blocks_above_target = allocated_tail-known;
    out.complete_owner_coverage = out.destinations_available && out.legacy_blockmap_inodes == 0
        && owners.value().corrupt_inodes == 0 && out.unknown_allocated_blocks_above_target == 0;
    return out;
}

Result<StageReport> stage_relocation(BlockDevice& dev,
                                     const OwnerRelocationPlan& plan, const StageOptions& options, Progress* progress) {
    if (options.journal_path.empty())return Error{Errc::invalid_argument, 0,"staging requires --journal=FILE"};
    auto safe = require_offline_for_write(dev);
    if (!safe)return safe.error();
    auto ds = inspect_device_safety(dev);
    if (!ds)return ds.error();
    if (ds.value().block_device && !options.allow_block_device)return Error{
        Errc::unsafe, 0,"block-device staging requires explicit --allow-block-device"
    }
    ;
    if (plan.moves.empty())return StageReport{0, 0, 0, 0, false, true};

    auto sr = read_superblock(dev);
    if (!sr)return sr.error();
    const auto sb = sr.value();
    if (sb.state != 1)return Error{
        Errc::unsafe, 0,"filesystem is not marked clean; relocation staging is refused"
    }
    ;
    if ((sb.feature_incompat&detail::INCOMPAT_RECOVER) != 0)return Error{
        Errc::unsafe, 0,"filesystem journal requires recovery; replay it before offline staging"
    }
    ;
    if ((sb.feature_ro_compat&detail::RO_COMPAT_METADATA_CSUM) != 0) {
        auto integrity = verify_metadata(dev, false, progress);
        if (!integrity)return integrity.error();
        if (!integrity.value().clean)return Error{
            Errc::corrupt, 0,"metadata checksum/count verification failed before staging"
        }
        ;
    }
    auto fu = parse_uuid(sb.uuid);
    if (!fu)return fu.error();
    auto prof = profile_device(dev);
    if (!prof)return prof.error();
    const auto workers = std::max<std::size_t>(1, prof.value().tuning.workers);
    const auto chunk = std::max<std::size_t>(static_cast<std::size_t>(plan.block_size),
                                             prof.value().tuning.large_io_bytes);
    TaskPool pool(workers, std::max<std::size_t>(workers * 2U, 4U));

    TransactionJournal journal;
    std::unordered_set<std::uint64_t> verified;
    bool existing = false;
    {
        if (std::filesystem::exists(options.journal_path)) {
            auto open = TransactionJournal::open(options.journal_path, true);
            if (!open)return open.error();
            journal = std::move(open.value());
            existing = true;
        } else {
            JournalIdentity id;
            id.operation_uuid = TransactionJournal::random_uuid();
            id.filesystem_uuid = fu.value();
            id.device_size = dev.geometry().size_bytes;
            id.target_size = plan.target_bytes;
            const auto move_count = static_cast<std::uint64_t>(plan.moves.size());
            if (move_count>(std::numeric_limits<std::uint64_t>::max()-4096ULL)/8ULL)
                return Error{Errc::unsupported, 0,"relocation plan is too large for journal accounting"};
            const std::uint64_t records_needed = move_count*8ULL+4096ULL;
            if (records_needed >
                (std::numeric_limits<std::uint64_t>::max() - TransactionJournal::records_offset) /
                TransactionJournal::record_bytes)
                return Error{Errc::unsupported, 0, "relocation journal size overflows"};
            const std::uint64_t required =
                TransactionJournal::records_offset + records_needed * TransactionJournal::record_bytes;
            const std::uint64_t cap = std::max<std::uint64_t>(16ULL<<20U, required);
            auto cr = TransactionJournal::create(options.journal_path, id, cap);
            if (!cr)return cr.error();
            journal = std::move(cr.value());
        }
    }
    if (journal.status().identity.filesystem_uuid != fu.value())return Error{
        Errc::unsafe, 0,"journal filesystem UUID does not match target"
    }
    ;
    if (journal.status().identity.device_size != dev.geometry().size_bytes)return Error{
        Errc::unsafe, 0,"journal device size does not match target"
    }
    ;
    if (journal.status().identity.target_size != plan.target_bytes)return Error{
        Errc::unsafe, 0,"journal target size does not match plan"
    }
    ;

    if (existing) {
        const auto phase = journal.status().phase;
        if (phase != OperationPhase::prepared && phase != OperationPhase::relocating
            && phase != OperationPhase::paused && phase != OperationPhase::staged)
            return Error{Errc::unsafe, 0,"journal phase is not valid for relocation staging"};
        auto rr = journal.records();
        if (!rr)return rr.error();
        std::unordered_map<std::uint64_t, JournalRecord> intents;
        std::unordered_map<std::uint64_t, JournalRecord> verified_records;
        for (const auto&r:rr.value()) {
            if (r.type == JournalRecordType::relocation_intent)intents[r.transaction_id] = r;
            else if (r.type == JournalRecordType::copy_verified)verified_records[r.transaction_id] = r;
        }
        if (intents.size() != plan.moves.size())return Error{
            Errc::unsafe, 0,"resume journal relocation count differs from current plan"
        }
        ;
        for (const auto&m:plan.moves) {
            auto it = intents.find(m.transaction_id);
            if (it == intents.end())return Error{Errc::unsafe, 0,"resume journal is missing relocation intent"};
            if (it->second.source_block != m.source_block || it->second.destination_block != m.destination_block
                || it->second.block_count != m.block_count || it->second.inode != m.inode)
                return Error{Errc::unsafe, 0,"resume plan differs from journaled relocation geometry"};
            auto vr = verified_records.find(m.transaction_id);
            if (vr != verified_records.end()) {
                auto crc = verify_one(dev, m, plan.block_size, chunk);
                if (!crc)return crc.error();
                if (crc.value() == vr->second.data_crc32c)verified.insert(m.transaction_id);
            }
        }
    } else {
        std::vector<JournalRecord> intents;
        intents.reserve(plan.moves.size());
        for (const auto&m:plan.moves) {
            JournalRecord r;
            r.type = JournalRecordType::relocation_intent;
            r.transaction_id = m.transaction_id;
            r.source_block = m.source_block;
            r.destination_block = m.destination_block;
            r.block_count = m.block_count;
            r.inode = m.inode;
            r.aux = m.logical_block;
            r.state = static_cast<std::uint32_t>(m.kind);
            intents.push_back(r);
        }
        auto ar = journal.append_batch(intents);
        if (!ar)return ar.error();
        auto ck = journal.checkpoint(OperationPhase::relocating);
        if (!ck)return ck.error();
    }

    StageReport report;
    report.moves_total = plan.moves.size();
    for (const auto&m:plan.moves)if (verified.count(m.transaction_id)) {
        ++report.moves_completed;
        report.bytes_copied+=m.unwritten?0:m.block_count*plan.block_size;
        report.bytes_verified+=m.unwritten?0:m.block_count*plan.block_size;
    }
    if (progress)progress->update({
                                  "relocating","verified staging copies", report.moves_completed, report.moves_total,
                                  report.bytes_verified, report.bytes_copied, report.moves_completed, report.moves_total
                                  }
                                 );

    std::size_t pos = 0;
    while (pos<plan.moves.size()) {
        while (pos<plan.moves.size() && verified.count(plan.moves[pos].transaction_id))++pos;
        if (pos >= plan.moves.size())break;
        if (Cancellation::requested()) {
            auto ck = journal.checkpoint(OperationPhase::paused);
            if (!ck)return ck.error();
            report.paused = true;
            return report;
        }

        std::vector<const OwnerMove*> batch;
        batch.reserve(workers);
        std::size_t cursor = pos;
        while (cursor<plan.moves.size() && batch.size()<workers) {
            if (!verified.count(plan.moves[cursor].transaction_id))batch.push_back(&plan.moves[cursor]);
            ++cursor;
        }
        std::vector<std::future<Result<CopyResult>>> copies;
        copies.reserve(batch.size());
        for (const auto* m : batch)
            copies.push_back(pool.submit([&dev, m, &plan, chunk] {
                                         return copy_one(dev, *m, plan.block_size, chunk);
                                         }
                                        ));
        std::vector<CopyResult> copied;
        copied.reserve(batch.size());
        for (auto&f:copies) {
            auto r = f.get();
            if (!r) {
                auto ck = journal.checkpoint(OperationPhase::paused);
                (void)ck;
                return r.error();
            }
            copied.push_back(r.value());
        }
        auto fl = dev.flush();
        if (!fl)return fl.error();

        std::vector<std::future<Result<std::uint32_t>>> checks;
        checks.reserve(batch.size());
        if (options.verify_after_write) {
            for (const auto* m : batch)
                checks.push_back(pool.submit([&dev, m, &plan, chunk] {
                                             return verify_one(dev, *m, plan.block_size, chunk);
                                             }
                                            ));
        }
        std::vector<JournalRecord> verified_records;
        verified_records.reserve(batch.size());
        for (std::size_t i = 0; i<batch.size(); ++i) {
            std::uint32_t dst_crc = copied[i].crc;
            if (options.verify_after_write) {
                auto vr = checks[i].get();
                if (!vr)return vr.error();
                dst_crc = vr.value();
                if (dst_crc != copied[i].crc)return Error{
                    Errc::io,
                    0,
                    "relocation verification checksum mismatch for transaction "
                        + std::to_string(batch[i]->transaction_id)
                }
                ;
            }
            JournalRecord r;
            r.type = JournalRecordType::copy_verified;
            r.transaction_id = batch[i]->transaction_id;
            r.source_block = batch[i]->source_block;
            r.destination_block = batch[i]->destination_block;
            r.block_count = batch[i]->block_count;
            r.inode = batch[i]->inode;
            r.aux = batch[i]->logical_block;
            r.data_crc32c = dst_crc;
            r.state = static_cast<std::uint32_t>(batch[i]->kind);
            verified_records.push_back(r);
            verified.insert(batch[i]->transaction_id);
            ++report.moves_completed;
            report.bytes_copied+=copied[i].bytes;
            report.bytes_verified+=copied[i].bytes;
        }
        auto ar = journal.append_batch(verified_records);
        if (!ar)return ar.error();
        if (progress)progress->update({
                                      "relocating","verified staging copies", report.moves_completed,
                                      report.moves_total,
                                      report.bytes_verified, report.bytes_copied, report.moves_completed,
                                      report.moves_total
                                      }
                                     );
        pos = cursor;
    }
    auto ck = journal.checkpoint(OperationPhase::staged);
    if (!ck)return ck.error();
    report.complete = true;
    return report;
}

} // namespace fsx::ext4
