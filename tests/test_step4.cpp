#include "fsx/allocation.hpp"
#include "fsx/crc.hpp"
#include "fsx/device.hpp"
#include "fsx/endian.hpp"
#include "fsx/ext4.hpp"
#include "fsx/ext4_metadata.hpp"
#include "fsx/ext4_maintenance.hpp"
#include "fsx/ext4_mutate.hpp"
#include "fsx/ext4_resize.hpp"
#include "fsx/ext4_verify.hpp"
#include "fsx/extents.hpp"
#include "fsx/gpt.hpp"
#include "fsx/journal.hpp"
#include "fsx/mbr.hpp"
#include "fsx/partition_sync.hpp"
#include "fsx/relocate.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

namespace {
constexpr std::uint64_t kBlockSize = 4096;
constexpr std::uint64_t kBlocks = 4096;

void set_bitmap(std::vector<std::byte>& bitmap, std::uint64_t bit) {
    bitmap[static_cast<std::size_t>(bit / 8)] |= std::byte(1U << (bit % 8));
}

void make_ext_image(const std::string& path) {
    auto d = fsx::BlockDevice::create_file(path, kBlocks * kBlockSize);
    assert(d);

    constexpr std::uint32_t inodes = 1024;
    constexpr std::uint32_t allocated_metadata_blocks = 68; // super/GDT + bitmaps + 64 inode-table blocks
    constexpr std::uint32_t data_blocks = 16;
    constexpr std::uint32_t free_blocks = static_cast<std::uint32_t>(kBlocks) - allocated_metadata_blocks - data_blocks;

    std::vector<std::byte> sb(1024);
    fsx::write_le32(sb, 0x00, inodes);
    fsx::write_le32(sb, 0x04, static_cast<std::uint32_t>(kBlocks));
    fsx::write_le32(sb, 0x08, 0);
    fsx::write_le32(sb, 0x0c, free_blocks);
    fsx::write_le32(sb, 0x10, inodes - 1);
    fsx::write_le32(sb, 0x14, 0);
    fsx::write_le32(sb, 0x18, 2); // 4096-byte blocks
    fsx::write_le32(sb, 0x20, 32768);
    fsx::write_le32(sb, 0x28, inodes);
    fsx::write_le16(sb, 0x38, 0xef53);
    fsx::write_le16(sb, 0x3a, 1);
    fsx::write_le32(sb, 0x4c, 1);
    fsx::write_le32(sb, 0x54, 11);
    fsx::write_le16(sb, 0x58, 256);
    fsx::write_le32(sb, 0x60, 0x40); // extents
    fsx::write_le16(sb, 0xfe, 32);
    for (unsigned i = 0; i < 16; ++i) sb[0x68 + i] = std::byte(i);
    assert(d.value().write_exact(1024, sb));

    std::vector<std::byte> gd(32);
    fsx::write_le32(gd, 0, 2); // block bitmap
    fsx::write_le32(gd, 4, 3); // inode bitmap
    fsx::write_le32(gd, 8, 4); // inode table
    fsx::write_le16(gd, 12, static_cast<std::uint16_t>(free_blocks));
    fsx::write_le16(gd, 14, static_cast<std::uint16_t>(inodes - 1));
    fsx::write_le16(gd, 16, 1);
    assert(d.value().write_exact(kBlockSize, gd));

    std::vector<std::byte> block_bitmap(kBlockSize);
    for (std::uint64_t i = 0; i < allocated_metadata_blocks; ++i) set_bitmap(block_bitmap, i);
    for (std::uint64_t i = 3800; i < 3816; ++i) set_bitmap(block_bitmap, i);
    assert(d.value().write_exact(2 * kBlockSize, block_bitmap));

    std::vector<std::byte> inode_bitmap(kBlockSize);
    set_bitmap(inode_bitmap, 1); // inode 2
    assert(d.value().write_exact(3 * kBlockSize, inode_bitmap));

    std::vector<std::byte> inode(256);
    fsx::write_le16(inode, 0, 0x81a4);
    fsx::write_le16(inode, 26, 1);
    fsx::write_le32(inode, 32, 0x00080000); // EXTENTS flag
    fsx::write_le32(inode, 100, 0x1234); // generation
    fsx::write_le16(inode, 40, 0xf30a); // extent header
    fsx::write_le16(inode, 42, 1); // entries
    fsx::write_le16(inode, 44, 4); // max
    fsx::write_le16(inode, 46, 0); // depth
    fsx::write_le32(inode, 52, 0); // logical block
    fsx::write_le16(inode, 56, 16); // length
    fsx::write_le16(inode, 58, 0); // physical high
    fsx::write_le32(inode, 60, 3800); // physical low
                                      // inode 2 begins at second 256-byte inode-table slot.
    assert(d.value().write_exact(4 * kBlockSize + 256, inode));

    std::vector<std::byte> payload(16 * kBlockSize);
    for (std::size_t i = 0; i < payload.size(); ++i)
        payload[i] = std::byte(static_cast<unsigned char>((i * 131U + 17U) & 0xffU));
    assert(d.value().write_exact(3800 * kBlockSize, payload));
    assert(d.value().flush());
}

std::array<std::byte, 16> guid(std::uint8_t seed) {
    std::array<std::byte, 16> g{};
    for (std::size_t i = 0; i < g.size(); ++i) g[i] = std::byte(static_cast<std::uint8_t>(seed + i));
    return g;
}

std::vector<std::byte> make_gpt_header(std::uint64_t current,
                                       std::uint64_t backup,
                                       std::uint64_t first_usable,
                                       std::uint64_t last_usable,
                                       std::uint64_t entries_lba,
                                       std::uint32_t entries_crc,
                                       const std::array<std::byte, 16>& disk_guid) {
    std::vector<std::byte> h(512);
    const char sig[] = "EFI PART";
    for (std::size_t i = 0; i < 8; ++i) h[i] = std::byte(static_cast<unsigned char>(sig[i]));
    fsx::write_le32(h, 8, 0x00010000U);
    fsx::write_le32(h, 12, 92);
    fsx::write_le64(h, 24, current);
    fsx::write_le64(h, 32, backup);
    fsx::write_le64(h, 40, first_usable);
    fsx::write_le64(h, 48, last_usable);
    std::copy(disk_guid.begin(), disk_guid.end(), h.begin() + 56);
    fsx::write_le64(h, 72, entries_lba);
    fsx::write_le32(h, 80, 128);
    fsx::write_le32(h, 84, 128);
    fsx::write_le32(h, 88, entries_crc);
    fsx::write_le32(h, 16, 0);
    const auto crc = fsx::crc32_ieee(std::span<const std::byte>(h).first(92)) ^ 0xffffffffU;
    fsx::write_le32(h, 16, crc);
    return h;
}

void make_gpt_image(const std::string& path, std::uint64_t partition_sectors = 6144) {
    constexpr std::uint64_t bytes = 64ULL * 1024ULL * 1024ULL;
    constexpr std::uint64_t sectors = bytes / 512ULL;
    constexpr std::uint64_t last_lba = sectors - 1;
    constexpr std::uint64_t entry_sectors = 32;
    constexpr std::uint64_t first_usable = 34;
    constexpr std::uint64_t last_usable = last_lba - entry_sectors - 1;
    auto d = fsx::BlockDevice::create_file(path, bytes);
    assert(d);

    std::array<std::byte, 512> mbr{};
    mbr[446 + 4] = std::byte{0xee};
    fsx::write_le32(mbr, 446 + 8, 1);
    fsx::write_le32(mbr, 446 + 12, static_cast<std::uint32_t>(sectors - 1));
    mbr[510] = std::byte{0x55};
    mbr[511] = std::byte{0xaa};
    assert(d.value().write_exact(0, mbr));

    std::vector<std::byte> entries(128U * 128U);
    const auto type = guid(0x10);
    const auto unique = guid(0x30);
    std::copy(type.begin(), type.end(), entries.begin());
    std::copy(unique.begin(), unique.end(), entries.begin() + 16);
    fsx::write_le64(entries, 32, 2048);
    fsx::write_le64(entries, 40, 2048 + partition_sectors - 1);
    const auto ecrc = fsx::crc32_ieee(entries) ^ 0xffffffffU;
    const auto disk_guid = guid(0x70);
    const auto primary = make_gpt_header(1, last_lba, first_usable, last_usable, 2, ecrc, disk_guid);
    const auto backup_entries_lba = last_lba - entry_sectors;
    const auto backup = make_gpt_header(last_lba, 1, first_usable, last_usable,
                                        backup_entries_lba, ecrc, disk_guid);
    assert(d.value().write_exact(512, primary));
    assert(d.value().write_exact(2 * 512ULL, entries));
    assert(d.value().write_exact(backup_entries_lba * 512ULL, entries));
    assert(d.value().write_exact(last_lba * 512ULL, backup));
    assert(d.value().flush());
}

void make_mbr_image(const std::string& path) {
    auto d = fsx::BlockDevice::create_file(path, 16ULL * 1024ULL * 1024ULL);
    assert(d);
    std::array<std::byte, 512> mbr{};
    mbr[446] = std::byte{0x80};
    mbr[450] = std::byte{0x83};
    fsx::write_le32(mbr, 454, 2048);
    fsx::write_le32(mbr, 458, 8192);
    fsx::write_le32(mbr, 440, 0x12345678U);
    mbr[510] = std::byte{0x55};
    mbr[511] = std::byte{0xaa};
    assert(d.value().write_exact(0, mbr));
    assert(d.value().flush());
}

} // namespace

int main() {
    // Metadata checksum primitive.
    {
        std::array<std::byte, 1024> sb{};
        fsx::write_le16(sb, 0x38, 0xef53);
        fsx::write_le32(sb, 0x64, 0x0400U);
        assert(fsx::ext4::update_superblock_checksum(sb));
        assert(fsx::ext4::verify_superblock_checksum(sb));
        sb[0x20] = std::byte{0x55};
        assert(!fsx::ext4::verify_superblock_checksum(sb));
    }

    const auto pid = std::to_string(static_cast<long long>(::getpid()));
    const std::string ext_img = "/tmp/fsx-step4-ext-" + pid + ".img";
    const std::string journal = ext_img + ".jnl";
    make_ext_image(ext_img);

    // Deep verification cross-checks extent ownership against allocation and
    // fixed metadata before any destructive operation is attempted.
    {
        auto d = fsx::BlockDevice::open_read(ext_img);
        assert(d);
        auto deep = fsx::ext4::verify_deep(d.value(), nullptr);
        assert(deep && deep.value().clean);
        assert(deep.value().complete_owner_coverage);
        assert(deep.value().data_extents == 1);
        assert(deep.value().overlapping_owner_ranges == 0);
    }

    // Conservative repair rebuilds counters from the allocation bitmaps, then
    // verifies the result. Label tuning updates all superblock copies.
    {
        auto d = fsx::BlockDevice::open_write(ext_img);
        assert(d);
        std::array<std::byte, 2> bogus{std::byte{0x01}, std::byte{0x00}};
        assert(d.value().write_exact(kBlockSize + 12, bogus));
        std::array<std::byte, 4> sb_bogus{};
        assert(d.value().write_exact(1024 + 0x0c, sb_bogus));
        assert(d.value().flush());
        auto bad = fsx::ext4::verify_metadata(d.value(), false, nullptr);
        assert(bad && !bad.value().clean);
        auto fixed = fsx::ext4::repair_metadata_counters(d.value(), {}, nullptr);
        assert(fixed && fixed.value().verified);
        auto clean = fsx::ext4::verify_metadata(d.value(), false, nullptr);
        assert(clean && clean.value().clean);
        assert(fsx::ext4::set_volume_label(d.value(), "FSXSTEP4"));
        auto sb = fsx::ext4::read_superblock(d.value());
        assert(sb && sb.value().volume_name == "FSXSTEP4");
    }

    // End-to-end safe compaction: plan -> staged copy -> authoritative switch.
    std::uint64_t destination = 0;
    {
        auto d = fsx::BlockDevice::open_write(ext_img);
        assert(d);
        auto integrity = fsx::ext4::verify_metadata(d.value(), false, nullptr);
        assert(integrity && integrity.value().clean);

        auto plan = fsx::ext4::build_owner_relocation_plan(d.value(), 3700 * kBlockSize, nullptr);
        assert(plan && plan.value().complete_owner_coverage);
        assert(plan.value().moves.size() == 1);
        destination = plan.value().moves[0].destination_block;
        assert(destination < 3700);

        fsx::ext4::StageOptions so;
        so.journal_path = journal;
        auto staged = fsx::ext4::stage_relocation(d.value(), plan.value(), so, nullptr);
        assert(staged && staged.value().complete);

        fsx::ext4::CommitOptions co;
        co.journal_path = journal;
        auto committed = fsx::ext4::commit_staged_relocation(d.value(), plan.value(), co, nullptr);
        assert(committed && committed.value().complete && committed.value().filesystem_clean);
        assert(committed.value().moves_committed == 1);

        auto owner = fsx::ext4::find_inode_extent(d.value(), 2, 0);
        assert(owner && owner.value().physical_block == destination);
        auto graph = fsx::ext4::build_allocation_graph(d.value(), nullptr);
        assert(graph);
        const auto allocated = [&](std::uint64_t block) {
            return std::any_of(graph.value().allocated.begin(), graph.value().allocated.end(),
                               [block](const fsx::ext4::BlockExtent& e) {
                               return block >= e.start && block < e.end();
                               });
        };
        assert(allocated(destination));
        assert(!allocated(3800));
        integrity = fsx::ext4::verify_metadata(d.value(), false, nullptr);
        assert(integrity && integrity.value().clean);

        // Metadata commit is deliberately idempotent after the compacted checkpoint.
        auto again = fsx::ext4::commit_staged_relocation(d.value(), plan.value(), co, nullptr);
        assert(again && again.value().complete);
    }

    // The synthetic image remains in one block group, so the real finalizer can
    // shorten the ext4 superblock without inode renumbering.
    {
        auto d = fsx::BlockDevice::open_write(ext_img);
        assert(d);
        auto ready = fsx::ext4::analyze_shrink_readiness(d.value(), 3700 * kBlockSize, nullptr);
        assert(ready && ready.value().metadata_only_finalize_possible);
        fsx::ext4::FinalizeOptions fo;
        fo.journal_path = journal;
        auto fin = fsx::ext4::finalize_metadata_only_shrink(d.value(), 3700 * kBlockSize, fo, nullptr);
        assert(fin);
        auto sb = fsx::ext4::read_superblock(d.value());
        assert(sb && sb.value().blocks_count == 3700);
        auto integrity = fsx::ext4::verify_metadata(d.value(), false, nullptr);
        assert(integrity && integrity.value().clean);
        auto j = fsx::TransactionJournal::open(journal, false);
        assert(j && j.value().status().phase == fsx::OperationPhase::filesystem_shrunk);
    }

    // Same-group grow is the inverse of metadata-only shrink. Blocks outside
    // the current filesystem must still be marked unavailable until the size
    // transition exposes them.
    {
        auto d = fsx::BlockDevice::open_write(ext_img);
        assert(d);
        fsx::ext4::GrowOptions go;
        auto grow = fsx::ext4::grow_within_last_group(d.value(), 3900 * kBlockSize, go, nullptr);
        assert(grow && grow.value().verified);
        assert(grow.value().old_blocks == 3700);
        assert(grow.value().new_blocks == 3900);
        auto sb = fsx::ext4::read_superblock(d.value());
        assert(sb && sb.value().blocks_count == 3900);
        auto integrity = fsx::ext4::verify_metadata(d.value(), false, nullptr);
        assert(integrity && integrity.value().clean);

        // A grow beyond the containing device must fail before changing the
        // filesystem. This also checks that a refused grow is side-effect free.
        auto too_far = fsx::ext4::grow_within_last_group(d.value(), 5000 * kBlockSize, go, nullptr);
        assert(!too_far);
        sb = fsx::ext4::read_superblock(d.value());
        assert(sb && sb.value().blocks_count == 3900);
        integrity = fsx::ext4::verify_metadata(d.value(), false, nullptr);
        assert(integrity && integrity.value().clean);
    }

    // Partition synchronization refuses guessing: the selected GPT entry must
    // match the filesystem device's current containing geometry before its end
    // is moved to the filesystem's smaller logical size.
    const std::string wrong_sync_gpt = "/tmp/fsx-step4-wrong-sync-gpt-" + pid + ".img";
    make_gpt_image(wrong_sync_gpt);
    {
        auto fsdev = fsx::BlockDevice::open_read(ext_img);
        auto disk = fsx::BlockDevice::open_write(wrong_sync_gpt);
        assert(fsdev && disk);
        fsx::PartitionSyncOptions so;
        auto refused = fsx::sync_gpt_partition_to_ext4(fsdev.value(), disk.value(), 1, so, nullptr);
        assert(!refused);
        auto table = fsx::gpt::read_redundant_table(disk.value());
        assert(table && table.value().partitions.size() == 1);
        assert(table.value().partitions[0].last_lba == 2048 + 6144 - 1);
    }
    std::filesystem::remove(wrong_sync_gpt);

    const std::string sync_gpt = "/tmp/fsx-step4-sync-gpt-" + pid + ".img";
    make_gpt_image(sync_gpt, (kBlocks * kBlockSize) / 512);
    {
        auto fsdev = fsx::BlockDevice::open_read(ext_img);
        auto disk = fsx::BlockDevice::open_write(sync_gpt);
        assert(fsdev && disk);
        fsx::PartitionSyncOptions so;
        auto sr = fsx::sync_gpt_partition_to_ext4(fsdev.value(), disk.value(), 1, so, nullptr);
        assert(sr && sr.value().verified && sr.value().changed);
        auto table = fsx::gpt::read_redundant_table(disk.value());
        assert(table && table.value().partitions.size() == 1);
        const auto expected_sectors = (3900 * kBlockSize) / 512;
        assert(table.value().partitions[0].last_lba == 2048 + expected_sectors - 1);
    }
    std::filesystem::remove(sync_gpt);

    std::filesystem::remove(ext_img);
    std::filesystem::remove(journal);

    // GPT dual-copy update and one-side recovery.
    const std::string gpt_img = "/tmp/fsx-step4-gpt-" + pid + ".img";
    const std::string gpt_backup = gpt_img + ".fsxgpt";
    make_gpt_image(gpt_img);
    {
        auto d = fsx::BlockDevice::open_write(gpt_img);
        assert(d);
        auto t = fsx::gpt::read_redundant_table(d.value());
        assert(t && t.value().primary_header_crc_ok && t.value().backup_header_crc_ok);
        assert(t.value().entries_crc_ok && t.value().backup_entries_crc_ok);
        assert(t.value().partitions.size() == 1);

        // Never let the legacy MBR mutator operate on a protective/hybrid GPT
        // MBR; doing so can damage GPT discoverability while leaving the GPT
        // itself intact.
        const auto protective_undo = gpt_img + ".protective-mbr";
        fsx::mbr::MutationOptions protective_options;
        protective_options.backup_sector_path = protective_undo;
        auto protective_refused = fsx::mbr::resize_partition(d.value(), 1, 1, 1024, protective_options);
        assert(!protective_refused && protective_refused.error().code == fsx::Errc::unsafe);
        assert(!std::filesystem::exists(protective_undo));

        assert(fsx::gpt::backup_metadata(d.value(), gpt_backup));
        const auto backup_size = std::filesystem::file_size(gpt_backup);
        // Backup publication is no-clobber: retrying the same path must fail
        // without changing the already durable sidecar.
        auto duplicate_backup = fsx::gpt::backup_metadata(d.value(), gpt_backup);
        assert(!duplicate_backup && duplicate_backup.error().code == fsx::Errc::unsafe);
        assert(std::filesystem::file_size(gpt_backup) == backup_size);

        // A damaged sidecar must be rejected before any disk writes occur.
        const std::string bad_backup = gpt_backup + ".bad";
        std::filesystem::copy_file(gpt_backup, bad_backup, std::filesystem::copy_options::overwrite_existing);
        {
            auto bad = fsx::BlockDevice::open_write(bad_backup);
            assert(bad);
            std::array<std::byte, 1> damage{std::byte{0xa5}};
            assert(bad.value().write_exact(128 + 10, damage));
            assert(bad.value().flush());
        }
        auto bad_restore = fsx::gpt::restore_metadata(d.value(), bad_backup);
        assert(!bad_restore);
        std::filesystem::remove(bad_backup);

        // Trailing bytes are also rejected; a sidecar has one canonical size.
        const std::string trailing_backup = gpt_backup + ".trailing";
        std::filesystem::copy_file(gpt_backup, trailing_backup,
                                   std::filesystem::copy_options::overwrite_existing);
        {
            std::ofstream out(trailing_backup, std::ios::binary | std::ios::app);
            out.put('X');
            assert(out.good());
        }
        auto trailing_restore = fsx::gpt::restore_metadata(d.value(), trailing_backup);
        assert(!trailing_restore && trailing_restore.error().code == fsx::Errc::corrupt);
        std::filesystem::remove(trailing_backup);

        auto still_clean = fsx::gpt::read_redundant_table(d.value());
        assert(still_clean && still_clean.value().primary_header_crc_ok
               && still_clean.value().backup_header_crc_ok);

        auto mr = fsx::gpt::resize_partition(d.value(), 1, 2048, 6143);
        assert(mr && mr.value().verified);
        t = fsx::gpt::read_redundant_table(d.value());
        assert(t && t.value().partitions[0].last_lba == 6143);

        // Damage only the primary header CRC domain; backup remains authoritative.
        std::array<std::byte, 1> bad{std::byte{0x7f}};
        assert(d.value().write_exact(512 + 20, bad));
        assert(d.value().flush());
        t = fsx::gpt::read_redundant_table(d.value());
        assert(t);
        assert(!t.value().primary_header_crc_ok && t.value().backup_header_crc_ok);
        auto repair = fsx::gpt::repair_redundancy(d.value());
        assert(repair && repair.value().verified);
        t = fsx::gpt::read_redundant_table(d.value());
        assert(t && t.value().primary_header_crc_ok && t.value().backup_header_crc_ok);

        auto restored = fsx::gpt::restore_metadata(d.value(), gpt_backup);
        assert(restored && restored.value().verified);
        t = fsx::gpt::read_redundant_table(d.value());
        assert(t && t.value().partitions[0].first_lba == 2048);
        assert(t.value().partitions[0].last_lba == 2048 + 6144 - 1);
    }
    std::filesystem::remove(gpt_img);
    std::filesystem::remove(gpt_backup);

    // MBR mutation always creates an explicit sector-level undo file.
    const std::string mbr_img = "/tmp/fsx-step4-mbr-" + pid + ".img";
    const std::string mbr_bak = mbr_img + ".sector0";
    make_mbr_image(mbr_img);
    {
        auto d = fsx::BlockDevice::open_write(mbr_img);
        assert(d);
        auto before = fsx::mbr::read_table(d.value());
        assert(before && before.value().partitions.size() == 1);
        fsx::mbr::MutationOptions mo;
        mo.backup_sector_path = mbr_bak;
        // An existing undo path must stop the operation before sector 0 changes.
        { std::ofstream sentinel(mbr_bak, std::ios::binary);
            sentinel << "keep";
            assert(sentinel.good());
        }
        auto refused = fsx::mbr::resize_partition(d.value(), 1, 2048, 4096, mo);
        assert(!refused && refused.error().code == fsx::Errc::unsafe);
        auto unchanged = fsx::mbr::read_table(d.value());
        assert(unchanged && unchanged.value().partitions[0].sectors == 8192);
        assert(std::filesystem::file_size(mbr_bak) == 4);
        std::filesystem::remove(mbr_bak);

        assert(fsx::mbr::resize_partition(d.value(), 1, 2048, 4096, mo));
        assert(std::filesystem::file_size(mbr_bak) == 512);
        auto changed = fsx::mbr::read_table(d.value());
        assert(changed && changed.value().partitions[0].sectors == 4096);
        assert(fsx::mbr::restore_sector(d.value(), mbr_bak));
        auto restored = fsx::mbr::read_table(d.value());
        assert(restored && restored.value().partitions[0].sectors == 8192);
    }
    std::filesystem::remove(mbr_img);
    std::filesystem::remove(mbr_bak);

    return 0;
}
