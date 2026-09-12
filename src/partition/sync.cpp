#include "fsx/partition_sync.hpp"
#include "fsx/ext4.hpp"
#include "fsx/gpt.hpp"
#include "fsx/journal.hpp"
#include "fsx/safety.hpp"
#include "fsx/uuid.hpp"
#include <algorithm>
#include <limits>
#include <optional>

namespace fsx {
namespace {
Result<const gpt::Partition*> find_partition(const gpt::Table& table, std::uint32_t index) {
    for (const auto& p : table.partitions)
        if (p.index == index) return &p;
    return Error{Errc::not_found, 0, "GPT partition index is unused"};
}

Result<void> validate_journal(TransactionJournal& journal,
                              const ext4::Superblock& sb,
                              std::uint64_t target) {
    auto u = parse_uuid(sb.uuid);
    if (!u) return u.error();
    const auto& st = journal.status();
    if (st.identity.filesystem_uuid != u.value())
        return Error{Errc::unsafe, 0, "journal filesystem UUID does not match partition filesystem"};
    if (st.identity.target_size != target)
        return Error{Errc::unsafe, 0, "journal target size does not match current filesystem size"};
    if (st.phase != OperationPhase::filesystem_shrunk &&
        st.phase != OperationPhase::partition_pending &&
        st.phase != OperationPhase::complete)
        return Error{Errc::unsafe, 0, "journal is not at the partition-resize phase"};
    return {};
}
}

Result<PartitionSyncReport> sync_gpt_partition_to_ext4(BlockDevice& filesystem,
                                                       BlockDevice& disk,
                                                       std::uint32_t partition_index,
                                                       const PartitionSyncOptions& options,
                                                       Progress* progress) {
    auto fs_safe = require_offline_for_write(filesystem);
    if (!fs_safe) return fs_safe.error();
    auto disk_safe = require_disk_offline_for_write(disk);
    if (!disk_safe) return disk_safe.error();
    auto disk_state = inspect_device_safety(disk);
    if (!disk_state) return disk_state.error();
    if (disk_state.value().block_device && !options.allow_block_device)
        return Error{Errc::unsafe, 0, "GPT synchronization on a real disk requires --allow-block-device"};

    auto sb = ext4::read_superblock(filesystem);
    if (!sb) return sb.error();
    if ((sb.value().state & 1U) == 0)
        return Error{Errc::unsafe, 0, "filesystem is not marked clean; partition synchronization refused"};

    auto table = gpt::read_redundant_table(disk);
    if (!table) return table.error();
    if (!table.value().primary_header_crc_ok || !table.value().entries_crc_ok ||
        !table.value().backup_header_crc_ok || !table.value().backup_entries_crc_ok)
        return Error{Errc::corrupt, 0, "both GPT copies must be valid before partition synchronization"};
    auto part = find_partition(table.value(), partition_index);
    if (!part) return part.error();

    const auto sector = static_cast<std::uint64_t>(disk.geometry().logical_sector
                                                   ? disk.geometry().logical_sector : 512U);
    if (filesystem.geometry().logical_sector != 0 &&
        static_cast<std::uint64_t>(filesystem.geometry().logical_sector) != sector &&
        filesystem.geometry().is_block_device)
        return Error{Errc::unsafe, 0, "filesystem and parent disk report different logical sector sizes"};
    const auto sectors = part.value()->last_lba - part.value()->first_lba + 1;
    if (sectors > std::numeric_limits<std::uint64_t>::max() / sector)
        return Error{Errc::corrupt, 0, "GPT partition byte size overflows"};
    const auto partition_bytes = sectors * sector;
    if (filesystem.geometry().size_bytes != partition_bytes)
        return Error{Errc::unsafe, 0,
            "selected GPT partition size does not match filesystem device geometry; "
            "wrong disk/index pair or stale kernel table"
        }
    ;
    const auto filesystem_bytes = sb.value().size_bytes();
    if (filesystem_bytes > partition_bytes)
        return Error{Errc::corrupt, 0, "filesystem is larger than selected GPT partition"};
    if (filesystem_bytes % sector != 0)
        return Error{Errc::unsupported, 0, "filesystem size is not aligned to parent disk logical sectors"};
    const auto new_sectors = filesystem_bytes / sector;
    if (new_sectors == 0)
        return Error{Errc::corrupt, 0, "filesystem has zero partition sectors"};
    if (part.value()->first_lba > std::numeric_limits<std::uint64_t>::max() - (new_sectors - 1))
        return Error{Errc::corrupt, 0, "new GPT partition endpoint overflows"};
    const auto new_last = part.value()->first_lba + new_sectors - 1;
    if (new_last > part.value()->last_lba)
        return Error{
            Errc::unsafe, 0, "partition synchronization only shrinks to the current filesystem size"
        }
    ;

    PartitionSyncReport report;
    report.partition_index = partition_index;
    report.first_lba = part.value()->first_lba;
    report.old_last_lba = part.value()->last_lba;
    report.new_last_lba = new_last;
    report.filesystem_bytes = filesystem_bytes;

    std::optional<TransactionJournal> journal;
    if (!options.journal_path.empty()) {
        auto j = TransactionJournal::open(options.journal_path, true);
        if (!j) return j.error();
        auto valid = validate_journal(j.value(), sb.value(), filesystem_bytes);
        if (!valid) return valid.error();
        journal.emplace(std::move(j.value()));
        if (journal->status().phase == OperationPhase::complete) {
            if (new_last != part.value()->last_lba)
                return Error{
                    Errc::unsafe, 0, "journal says operation is complete but GPT still has old geometry"
                }
            ;
            report.verified = true;
            report.journal_completed = true;
            return report;
        }
    }

    if (new_last == part.value()->last_lba) {
        report.verified = true;
        if (journal) {
            auto done = journal->checkpoint(OperationPhase::complete);
            if (!done) return done.error();
            report.journal_completed = true;
        }
        return report;
    }

    if (journal) {
        if (journal->status().phase != OperationPhase::partition_pending) {
            JournalRecord intent;
            intent.type = JournalRecordType::partition_intent;
            intent.transaction_id = 1;
            intent.source_block = part.value()->last_lba;
            intent.destination_block = new_last;
            intent.block_count = new_sectors;
            intent.inode = partition_index;
            intent.aux = part.value()->first_lba;
            auto ar = journal->append(intent);
            if (!ar) return ar.error();
            auto cp = journal->checkpoint(OperationPhase::partition_pending);
            if (!cp) return cp.error();
        }
    }

    if (progress)
        progress->update({"partition-sync", "writing backup GPT first", 0, 2, 0, 0, 0, 2});
    gpt::MutationOptions mo;
    mo.allow_block_device = options.allow_block_device;
    auto changed = gpt::resize_partition(disk, partition_index,
                                         part.value()->first_lba, new_last,
                                         mo, progress);
    if (!changed) return changed.error();
    if (!changed.value().verified)
        return Error{Errc::io, 0, "GPT resize did not pass post-write verification"};
    report.changed = true;
    report.verified = true;

    if (journal) {
        JournalRecord committed;
        committed.type = JournalRecordType::partition_committed;
        committed.transaction_id = 1;
        committed.source_block = part.value()->last_lba;
        committed.destination_block = new_last;
        committed.block_count = new_sectors;
        committed.inode = partition_index;
        committed.aux = part.value()->first_lba;
        auto ar = journal->append(committed);
        if (!ar) return ar.error();
        auto cp = journal->checkpoint(OperationPhase::complete);
        if (!cp) return cp.error();
        report.journal_completed = true;
    }
    return report;
}

} // namespace fsx
