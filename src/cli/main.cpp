#include "fsx/allocation.hpp"
#include "fsx/benchmark.hpp"
#include "fsx/cancel.hpp"
#include "fsx/device.hpp"
#include "fsx/ext4.hpp"
#include "fsx/ext4_metadata.hpp"
#include "fsx/ext4_maintenance.hpp"
#include "fsx/ext4_mutate.hpp"
#include "fsx/ext4_resize.hpp"
#include "fsx/ext4_verify.hpp"
#include "fsx/extents.hpp"
#include "fsx/fat.hpp"
#include "fsx/exfat.hpp"
#include "fsx/probe.hpp"
#include "fsx/image.hpp"
#include "fsx/safety.hpp"
#include "fsx/gpt.hpp"
#include "fsx/io_scheduler.hpp"
#include "fsx/journal.hpp"
#include "fsx/journal_validate.hpp"
#include "fsx/mbr.hpp"
#include "fsx/partition_sync.hpp"
#include "fsx/progress.hpp"
#include "fsx/recovery.hpp"
#include "fsx/relocate.hpp"
#include "fsx/size.hpp"
#include "fsx/scrub.hpp"
#include "fsx/clone.hpp"
#include "fsx/xfs.hpp"
#include "fsx/ntfs.hpp"
#include "fsx/btrfs.hpp"
#include "fsx/iso9660.hpp"
#include "fsx/ufs.hpp"
#include "fsx/uuid.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using namespace fsx;

int exit_code_for(const Error& e) {
    switch (e.code) {
    case Errc::usage:
    case Errc::invalid_argument: return 1;
    case Errc::corrupt: return 3;
    case Errc::unsafe: return 7;
    case Errc::unsupported: return 6;
    default: return 2;
    }
}

int fail(const Error& e) {
    std::cerr << "fsx: " << errc_name(e.code) << ": " << e.message << "\n";
    return exit_code_for(e);
}

void usage() {
    std::cout <<
        "fsx 1.0.0\n"
        "usage:\n"
        "  fsx info DEVICE\n"
        "  fsx probe DEVICE\n"
        "  fsx capabilities DEVICE\n"
        "  fsx check DEVICE [--progress=auto|live|plain|none]\n"
        "  fsx check metadata DEVICE [--quick] [--progress=...]\n"
        "  fsx check deep DEVICE [--progress=...]\n"
        "  fsx repair counters DEVICE --write [--allow-block-device] [--progress=...]\n"
        "  fsx tune label DEVICE LABEL --write [--allow-block-device]\n"
        "  fsx allocation DEVICE [--progress=...]\n"
        "  fsx owner-map DEVICE [FROM_SIZE] [--progress=...]\n"
        "  fsx plan resize DEVICE SIZE [--journal=FILE] [--progress=...]\n"
        "  fsx plan relocate DEVICE SIZE [--progress=...]\n"
        "  fsx analyze shrink DEVICE SIZE [--progress=...]\n"
        "  fsx stage resize DEVICE SIZE --journal=FILE [--write] [--allow-block-device] [--progress=...]\n"
        "  fsx commit resize DEVICE SIZE --journal=FILE --write [--allow-block-device] [--progress=...]\n"
        "  fsx compact resize DEVICE SIZE --journal=FILE --write [--allow-block-device] [--progress=...]\n"
        "  fsx finalize shrink DEVICE SIZE --journal=FILE --write [--allow-block-device] [--progress=...]\n"
        "  fsx resize DEVICE SIZE [--journal=FILE] --write [--allow-block-device] [--progress=...]\n"
        "             [--partition-disk=DISK --partition-index=N]\n"
        "  fsx grow DEVICE SIZE --write [--allow-block-device] [--progress=...]\n"
        "\n"
        "  fsx partition list DISK\n"
        "  fsx partition resize DISK INDEX FIRST_LBA LAST_LBA --write [--allow-block-device]\n"
        "  fsx partition sync DISK INDEX FILESYSTEM --write [--journal=FILE] [--allow-block-device]\n"
        "  fsx partition repair DISK --write [--allow-block-device]\n"
        "  fsx partition backup DISK FILE\n"
        "  fsx partition restore DISK FILE --write [--allow-block-device]\n"
        "  fsx mbr list DISK\n"
        "  fsx mbr resize DISK INDEX FIRST_LBA SECTORS --backup-sector=FILE --write [--allow-block-device]\n"
        "  fsx mbr restore DISK --backup-sector=FILE --write [--allow-block-device]\n"
        "\n"
        "  fsx io-profile DEVICE\n"
        "  fsx benchmark DEVICE [SIZE] [--progress=...]\n"
        "  fsx scrub DEVICE [SIZE] [--passes=N] [--chunk=SIZE] [--progress=...]\n"
        "  fsx clone SOURCE TARGET --write [--create-file] [--chunk=SIZE]\n"
        "            [--allow-block-device] [--allow-live-source] [--progress=...]\n"
        "  fsx journal inspect FILE\n"
        "  fsx journal verify FILE\n"
        "  fsx journal dump FILE [LIMIT]\n"
        "\n"
        "  fsx image create SOURCE FILE [--chunk=SIZE] [--allow-live-source] [--progress=...]\n"
        "  fsx image info FILE\n"
        "  fsx image verify FILE [--progress=...]\n"
        "  fsx image restore FILE TARGET --write [--create-file] [--allow-block-device] [--progress=...]\n"
        "\n"
        "Destructive block-device operations require explicit --write and\n"
        "--allow-block-device. FSX refuses mounted/swap/held devices and, on\n"
        "Linux, whole disks with active child partitions.\n"
        "\n"
        "FSX 1.0 combines the ext transaction engine with generic filesystem\n"
        "inspection, FAT/exFAT/XFS/NTFS/Btrfs/ISO9660/UFS validation, raw\n"
        "scrub/clone, and checksummed sparse imaging/restore.\n"
        "Filesystem-size finalization is deliberately limited to targets that\n"
        "do not remove ext4 block groups. Arbitrary group-removing shrink is\n"
        "reported as unsupported rather than risking inode renumbering errors.\n";
}

bool has_flag(const std::vector<std::string>& args, std::string_view flag) {
    return std::find(args.begin(), args.end(), flag) != args.end();
}

ProgressMode get_progress(const std::vector<std::string>& args) {
    for (const auto& s : args)
        if (s.rfind("--progress=", 0) == 0)
            return parse_progress_mode(s.substr(11));
    return ProgressMode::automatic;
}

std::optional<std::string> option_value(const std::vector<std::string>& args,
                                        std::string_view prefix) {
    for (const auto& s : args)
        if (s.rfind(prefix, 0) == 0)
            return s.substr(prefix.size());
    return std::nullopt;
}

Result<std::uint64_t> parse_u64(std::string_view s, std::string_view what) {
    if (s.empty()) return Error{Errc::invalid_argument, 0, std::string(what) + " is empty"};
    errno = 0;
    char* end = nullptr;
    const std::string owned(s);
    const auto value = std::strtoull(owned.c_str(), &end, 10);
    if (errno != 0 || !end || *end != '\0')
        return Error{Errc::invalid_argument, 0, "invalid " + std::string(what) + ": " + std::string(s)};
    return static_cast<std::uint64_t>(value);
}

Result<std::uint32_t> parse_u32(std::string_view s, std::string_view what) {
    auto n = parse_u64(s, what);
    if (!n) return n.error();
    if (n.value() > std::numeric_limits<std::uint32_t>::max())
        return Error{Errc::invalid_argument, 0, std::string(what) + " is too large"};
    return static_cast<std::uint32_t>(n.value());
}

Result<void> require_write_flag(const std::vector<std::string>& args) {
    if (!has_flag(args, "--write"))
        return Error{Errc::unsafe, 0, "destructive command requires explicit --write"};
    return {};
}

bool allow_block_device(const std::vector<std::string>& args) {
    return has_flag(args, "--allow-block-device");
}

int cmd_part_sync(const std::string& disk_path,
                  std::uint32_t index,
                  const std::string& fs_path,
                  ProgressMode mode,
                  const std::vector<std::string>& args);

int cmd_probe(const std::string& path) {
    auto d = BlockDevice::open_read(path);
    if (!d) return fail(d.error());
    auto p = probe_filesystem(d.value());
    if (!p) return fail(p.error());
    const auto& x = p.value();
    std::cout << "filesystem:   " << x.name << "\n"
        << "confidence:   "
        << (x.kind == FilesystemKind::unknown ? "none" : (x.confident ? "high" : "heuristic")) << "\n";
    if (!x.label.empty()) std::cout << "label:        " << x.label << "\n";
    if (!x.uuid.empty()) std::cout << "uuid:         " << x.uuid << "\n";
    if (x.size_bytes) std::cout << "fs size:      " << human_bytes(x.size_bytes) << "\n";
    if (x.block_size) std::cout << "block/cluster:" << " " << x.block_size << "\n";
    for (const auto& n : x.notes) std::cout << "note:         " << n << "\n";
    return x.kind == FilesystemKind::unknown ? 6 : 0;
}

int cmd_capabilities(const std::string& path) {
    auto d = BlockDevice::open_read(path);
    if (!d) return fail(d.error());
    auto p = probe_filesystem(d.value());
    if (!p) return fail(p.error());
    std::cout << "filesystem:   " << p.value().name << "\n";
    switch (p.value().kind) {
    case FilesystemKind::ext:
        std::cout << "probe:        yes\n"
            << "info:         yes\n"
            << "check:        yes\n"
            << "deep check:   yes\n"
            << "repair:       counters\n"
            << "label tune:   yes\n"
            << "grow:         conditional\n"
            << "shrink:       conditional\n"
            << "format:       not yet\n";
        break;
    case FilesystemKind::fat12:
    case FilesystemKind::fat16:
    case FilesystemKind::fat32:
        std::cout << "probe:        yes\n"
            << "info:         yes\n"
            << "check:        yes\n"
            << "repair:       no\n"
            << "grow:         no\n"
            << "shrink:       no\n"
            << "format:       not yet\n";
        break;
    case FilesystemKind::exfat:
        std::cout << "probe:        yes\n"
            << "info:         yes\n"
            << "check:        yes\n"
            << "boot verify:  yes\n"
            << "repair:       no\n"
            << "grow:         no\n"
            << "shrink:       no\n"
            << "format:       not yet\n";
        break;
    case FilesystemKind::ufs1:
    case FilesystemKind::ufs2:
        std::cout << "probe:        yes\n"
            << "info:         superblock geometry\n"
            << "check:        geometry\n"
            << "repair:       no\n"
            << "resize:       no\n"
            << "format:       no\n";
        break;
    case FilesystemKind::xfs:
        std::cout << "probe:        yes\n"
            << "info:         superblock\n"
            << "check:        geometry\n"
            << "repair:       no\n"
            << "grow:         no\n"
            << "shrink:       no\n"
            << "format:       no\n";
        break;
    case FilesystemKind::ntfs:
        std::cout << "probe:        yes\n"
            << "info:         boot geometry\n"
            << "check:        geometry\n"
            << "repair:       no\n"
            << "resize:       no\n"
            << "format:       no\n";
        break;
    case FilesystemKind::btrfs:
        std::cout << "probe:        yes\n"
            << "info:         superblock + mirrors\n"
            << "check:        checksum + geometry\n"
            << "repair:       no\n"
            << "resize:       no\n"
            << "format:       no\n";
        break;
    case FilesystemKind::iso9660:
        std::cout << "probe:        yes\n"
            << "info:         volume descriptors\n"
            << "check:        geometry + descriptors\n"
            << "repair:       no\n"
            << "resize:       no\n"
            << "format:       no\n";
        break;
    default:
        std::cout << "probe:        no recognized filesystem\n";
        break;
    }
    std::cout << "image create: yes\nimage restore:yes\nraw scrub:    yes\nraw clone:    yes\n";
    return 0;
}

int cmd_info(const std::string& path) {
    auto d = BlockDevice::open_read(path);
    if (!d) return fail(d.error());
    auto& dev = d.value();
    std::cout << "device:       " << path << "\n"
        << "size:         " << human_bytes(dev.geometry().size_bytes) << "\n"
        << "sector:       " << dev.geometry().logical_sector << "\n"
        << "physical:     " << dev.geometry().physical_sector << "\n";
    auto prof = profile_device(dev);
    if (prof) {
        std::cout << "device class: " << device_class_name(prof.value().device_class) << "\n"
            << "io workers:   " << prof.value().tuning.workers << "\n"
            << "queue depth:  " << prof.value().tuning.queue_depth << "\n";
    }
    auto pr = probe_filesystem(dev);
    if (!pr) return fail(pr.error());
    const auto& fp = pr.value();
    std::cout << "filesystem:   " << fp.name << "\n";
    if (fp.kind == FilesystemKind::unknown) return 0;
    if (!fp.label.empty()) std::cout << "volume:       " << fp.label << "\n";
    if (!fp.uuid.empty()) std::cout << "uuid:         " << fp.uuid << "\n";
    if (fp.size_bytes) std::cout << "fs size:      " << human_bytes(fp.size_bytes) << "\n";
    if (fp.block_size) std::cout << "block/cluster:" << " " << fp.block_size << "\n";

    if (fp.kind == FilesystemKind::ext) {
        auto sb = ext4::read_superblock(dev);
        if (!sb) return fail(sb.error());
        const auto& x = sb.value();
        std::cout << "blocks:       " << x.blocks_count << "\n"
            << "free blocks:  " << x.free_blocks << "\n"
            << "groups:       " << x.groups_count() << "\n"
            << "state:        " << ((x.state & 1U) ? "clean" : "not clean") << "\n";
    }
    else if (fp.kind == FilesystemKind::fat12
             || fp.kind == FilesystemKind::fat16 || fp.kind == FilesystemKind::fat32) {
        auto v = fat::read_volume(dev);
        if (!v) return fail(v.error());
        std::cout << "clusters:     " << v.value().cluster_count << "\n"
            << "FAT copies:   " << static_cast<unsigned>(v.value().fat_count) << "\n"
            << "serial:       0x" << std::hex << std::setw(8)
            << std::setfill('0') << v.value().serial << std::dec << std::setfill(' ') << "\n";
    } else if (fp.kind == FilesystemKind::exfat) {
        auto v = exfat::read_volume(dev);
        if (!v) return fail(v.error());
        std::cout << "clusters:     " << v.value().cluster_count << "\n"
            << "FAT copies:   " << static_cast<unsigned>(v.value().fat_count) << "\n"
            << "boot main:    " << (v.value().main_boot_checksum_ok ? "ok" : "BAD") << "\n"
            << "boot backup:  " << (v.value().backup_boot_checksum_ok ? "ok" : "BAD") << "\n";
    } else if (fp.kind == FilesystemKind::xfs) {
        auto v = xfs::read_superblock(dev);
        if (!v) return fail(v.error());
        std::cout << "data blocks:  " << v.value().data_blocks << "\n"
            << "AG count:     " << v.value().ag_count << "\n"
            << "inode size:   " << v.value().inode_size << "\n"
            << "sector size:  " << v.value().sector_size << "\n";
    } else if (fp.kind == FilesystemKind::ntfs) {
        auto v = ntfs::read_boot_sector(dev);
        if (!v) return fail(v.error());
        std::cout << "sectors:      " << v.value().total_sectors << "\n"
            << "cluster size: " << human_bytes(v.value().cluster_bytes()) << "\n"
            << "MFT cluster:  " << v.value().mft_cluster << "\n"
            << "serial:       0x" << std::hex << v.value().serial << std::dec << "\n";
    } else if (fp.kind == FilesystemKind::btrfs) {
        auto v = btrfs::read_best_superblock(dev);
        if (!v)return fail(v.error());
        std::cout << "generation:   " << v.value().generation << "\n"
            << "devices:      " << v.value().num_devices << "\n"
            << "nodesize:     " << v.value().nodesize << "\n"
            << "checksum:     " << btrfs::checksum_name(v.value().checksum_type) << "\n"
            << "super csum:   " << (v.value().checksum_valid?"ok":"BAD") << "\n";
    } else if (fp.kind == FilesystemKind::iso9660) {
        auto v = iso9660::read_primary(dev);
        if (!v)return fail(v.error());
        std::cout << "volume blocks:" << " " << v.value().volume_blocks << "\n"
            << "root extent:  " << v.value().root_extent << "\n"
            << "root size:    " << v.value().root_size << "\n"
            << "descriptors:  " << v.value().descriptor_count << "\n";
    } else if (fp.kind == FilesystemKind::ufs1 || fp.kind == FilesystemKind::ufs2) {
        auto v = ufs::read_superblock(dev);
        if (!v)return fail(v.error());
        std::cout << "super offset: " << v.value().offset << "\n"
            << "byte order:   " << (v.value().byte_order == ufs::ByteOrder::little?"little":"big") << "\n"
            << "frag size:    " << v.value().fragment_size << "\n"
            << "frags/block:  " << v.value().fragments_per_block << "\n"
            << "cyl groups:   " << v.value().cylinder_groups << "\n";
    }
    for (const auto& n : fp.notes) std::cout << "note:         " << n << "\n";
    return 0;
}

int cmd_check(const std::string& path, ProgressMode mode) {
    auto d = BlockDevice::open_read(path);
    if (!d) return fail(d.error());
    auto p = probe_filesystem(d.value());
    if (!p) return fail(p.error());
    Progress pg(mode);
    pg.start();
    if (p.value().kind == FilesystemKind::ext) {
        auto r = ext4::check(d.value(), &pg);
        pg.stop(static_cast<bool>(r) && r.value().clean);
        if (!r) return fail(r.error());
        const auto& x = r.value();
        std::cout << "filesystem:   ext2/3/4\nresult:       " << (x.clean ? "clean" : "errors found")
            << "\ngroups:       " << x.groups_checked
            << "\nallocated:    " << x.bitmap_allocated_blocks << " blocks\n";
        for (const auto& w : x.warnings) std::cout << "warning:      " << w << "\n";
        for (const auto& e : x.errors) std::cout << "error:        " << e << "\n";
        return x.clean ? 0 : 3;
    }
    if (p.value().kind == FilesystemKind::fat12
        || p.value().kind == FilesystemKind::fat16 || p.value().kind == FilesystemKind::fat32) {
        auto r = fat::check(d.value(), &pg);
        pg.stop(static_cast<bool>(r) && r.value().clean);
        if (!r) return fail(r.error());
        std::cout << "filesystem:   " << p.value().name
            << "\nresult:       " << (r.value().clean ? "clean" : "errors found")
            << "\ninvalid FAT:  " << r.value().invalid_cluster_entries
            << "\nmirror diffs: " << r.value().mirrored_fat_mismatches << "\n";
        for (const auto& w : r.value().warnings) std::cout << "warning:      " << w << "\n";
        for (const auto& e : r.value().errors) std::cout << "error:        " << e << "\n";
        return r.value().clean ? 0 : 3;
    }
    if (p.value().kind == FilesystemKind::exfat) {
        auto r = exfat::check(d.value());
        pg.stop(static_cast<bool>(r) && r.value().clean);
        if (!r) return fail(r.error());
        std::cout << "filesystem:   exFAT\nresult:       "
            << (r.value().clean ? "clean" : "errors found") << "\n";
        for (const auto& w : r.value().warnings) std::cout << "warning:      " << w << "\n";
        for (const auto& e : r.value().errors) std::cout << "error:        " << e << "\n";
        return r.value().clean ? 0 : 3;
    }
    if (p.value().kind == FilesystemKind::xfs) {
        auto r = xfs::check(d.value());
        pg.stop(static_cast<bool>(r) && r.value().clean);
        if (!r) return fail(r.error());
        std::cout << "filesystem:   XFS\nresult:       "
            << (r.value().clean ? "clean" : "errors found") << "\n";
        for (const auto& w : r.value().warnings) std::cout << "warning:      " << w << "\n";
        for (const auto& e : r.value().errors) std::cout << "error:        " << e << "\n";
        return r.value().clean ? 0 : 3;
    }
    if (p.value().kind == FilesystemKind::ntfs) {
        auto r = ntfs::check(d.value());
        pg.stop(static_cast<bool>(r) && r.value().clean);
        if (!r) return fail(r.error());
        std::cout << "filesystem:   NTFS\nresult:       "
            << (r.value().clean ? "clean" : "errors found") << "\n";
        for (const auto& w : r.value().warnings) std::cout << "warning:      " << w << "\n";
        for (const auto& e : r.value().errors) std::cout << "error:        " << e << "\n";
        return r.value().clean ? 0 : 3;
    }
    if (p.value().kind == FilesystemKind::btrfs) {
        auto r = btrfs::check(d.value());
        pg.stop(static_cast<bool>(r) && r.value().clean);
        if (!r)return fail(r.error());
        std::cout<<"filesystem:   Btrfs\nresult:       "<<(r.value().clean?"clean":"errors found")<<"\nmirrors:      "
            <<r.value().mirrors_valid<<"/"<<r.value().mirrors_present
            <<" valid\ngeneration:   "<<r.value().selected_generation<<"\n";
        for (const auto&w:r.value().warnings)std::cout<<"warning:      "<<w<<"\n";
        for (const auto&e:r.value().errors)std::cout<<"error:        "<<e<<"\n";
        return r.value().clean?0:3;
    }
    if (p.value().kind == FilesystemKind::iso9660) {
        auto r = iso9660::check(d.value());
        pg.stop(static_cast<bool>(r) && r.value().clean);
        if (!r)return fail(r.error());
        std::cout<<"filesystem:   ISO9660\nresult:       "<<(r.value().clean?"clean":"errors found")<<"\n";
        for (const auto&w:r.value().warnings)std::cout<<"warning:      "<<w<<"\n";
        for (const auto&e:r.value().errors)std::cout<<"error:        "<<e<<"\n";
        return r.value().clean?0:3;
    }
    if (p.value().kind == FilesystemKind::ufs1 || p.value().kind == FilesystemKind::ufs2) {
        auto r = ufs::check(d.value());
        pg.stop(static_cast<bool>(r) && r.value().clean);
        if (!r)return fail(r.error());
        std::cout<<"filesystem:   "<<p.value().name<<"\nresult:       "
            <<(r.value().clean?"clean":"errors found")<<"\n";
        for (const auto&w:r.value().warnings)std::cout<<"warning:      "<<w<<"\n";
        for (const auto&e:r.value().errors)std::cout<<"error:        "<<e<<"\n";
        return r.value().clean?0:3;
    }
    pg.stop(false);
    return fail(Error{Errc::unsupported, 0,"check is not yet implemented for "+p.value().name});
}

int cmd_check_metadata(const std::string& path, ProgressMode mode, bool quick) {
    auto d = BlockDevice::open_read(path);
    if (!d) return fail(d.error());
    Progress pg(mode);
    pg.start();
    auto r = ext4::verify_metadata(d.value(), !quick, &pg);
    pg.stop(static_cast<bool>(r) && r.value().clean);
    if (!r) return fail(r.error());
    const auto& x = r.value();
    std::cout << "metadata:          " << (x.clean ? "clean" : "errors found") << "\n"
        << "groups checked:    " << x.groups_checked << "\n"
        << "super checksum:    "
        << (x.superblock_checksum_present ? (x.superblock_checksum_valid ? "ok" : "BAD") : "not present") << "\n"
        << "group checksum:    " << x.group_desc_checksum_failures << " failures\n"
        << "block bitmap csum: " << x.block_bitmap_checksum_failures << " failures\n"
        << "inode bitmap csum: " << x.inode_bitmap_checksum_failures << " failures\n"
        << "block counts:      " << x.block_count_mismatches << " mismatches\n"
        << "inode counts:      " << x.inode_count_mismatches << " mismatches\n"
        << "inode checksums:   " << x.inode_checksums_checked
        << " checked, " << x.inode_checksum_failures << " failures\n";
    for (const auto& s : x.warnings) std::cout << "warning:           " << s << "\n";
    for (const auto& s : x.errors) std::cout << "error:             " << s << "\n";
    return x.clean ? 0 : 3;
}


int cmd_check_deep(const std::string& path, ProgressMode mode) {
    auto d = BlockDevice::open_read(path);
    if (!d) return fail(d.error());
    Progress pg(mode);
    pg.start();
    auto r = ext4::verify_deep(d.value(), &pg);
    pg.stop(static_cast<bool>(r) && r.value().clean);
    if (!r) return fail(r.error());
    const auto& x = r.value();
    std::cout << "deep result:      " << (x.clean ? "clean" : "ERRORS") << "\n"
        << "groups:           " << x.groups_checked << "\n"
        << "allocated inodes: " << x.allocated_inodes << "\n"
        << "extent inodes:    " << x.extent_inodes << "\n"
        << "legacy inodes:    " << x.legacy_inodes << "\n"
        << "data extents:     " << x.data_extents << "\n"
        << "data blocks:      " << x.data_blocks << "\n"
        << "tree blocks:      " << x.extent_tree_blocks << "\n"
        << "owner gaps:       " << x.owner_unallocated_ranges << "\n"
        << "metadata gaps:    " << x.metadata_unallocated_ranges << "\n"
        << "owner overlaps:   " << x.overlapping_owner_ranges << "\n"
        << "metadata overlap: " << x.owner_metadata_overlaps << "\n"
        << "owner coverage:   " << (x.complete_owner_coverage ? "complete" : "partial") << "\n";
    for (const auto& w : x.warnings) std::cout << "warning:          " << w << "\n";
    for (const auto& e : x.errors) std::cout << "error:            " << e << "\n";
    return x.clean ? 0 : 3;
}

int cmd_repair_counters(const std::string& path,
                        ProgressMode mode,
                        const std::vector<std::string>& args) {
    auto wr = require_write_flag(args);
    if (!wr) return fail(wr.error());
    auto d = BlockDevice::open_write(path);
    if (!d) return fail(d.error());
    ext4::RepairOptions o;
    o.allow_block_device = allow_block_device(args);
    Progress pg(mode);
    pg.start();
    auto r = ext4::repair_metadata_counters(d.value(), o, &pg);
    pg.stop(static_cast<bool>(r) && r.value().verified);
    if (!r) return fail(r.error());
    std::cout << "repair:            complete\n"
        << "groups rewritten:  " << r.value().groups_rewritten << "\n"
        << "free blocks:       " << r.value().free_blocks << "\n"
        << "free inodes:       " << r.value().free_inodes << "\n"
        << "superblock:        " << (r.value().superblock_rewritten ? "rewritten" : "unchanged") << "\n"
        << "verification:      " << (r.value().verified ? "clean" : "not requested") << "\n";
    return 0;
}

int cmd_tune_label(const std::string& path,
                   const std::string& label,
                   const std::vector<std::string>& args) {
    auto wr = require_write_flag(args);
    if (!wr) return fail(wr.error());
    auto d = BlockDevice::open_write(path);
    if (!d) return fail(d.error());
    auto r = ext4::set_volume_label(d.value(), label, allow_block_device(args));
    if (!r) return fail(r.error());
    std::cout << "label:        " << (label.empty() ? "-" : label) << "\n"
        << "status:       updated\n";
    return 0;
}

int cmd_allocation(const std::string& path, ProgressMode mode) {
    auto d = BlockDevice::open_read(path);
    if (!d) return fail(d.error());
    Progress pg(mode);
    pg.start();
    auto r = ext4::build_allocation_graph(d.value(), &pg);
    pg.stop(static_cast<bool>(r));
    if (!r) return fail(r.error());
    const auto& g = r.value();
    std::cout << "block size:        " << g.block_size << "\n"
        << "total blocks:      " << g.total_blocks << "\n"
        << "allocated blocks:  " << g.allocated_blocks << "\n"
        << "free blocks:       " << g.free_blocks << "\n"
        << "allocated extents: " << g.allocated.size() << "\n"
        << "free extents:      " << g.free.size() << "\n";
    return 0;
}

int write_plan_journal(const std::string& file,
                       BlockDevice& dev,
                       const ext4::ResizePlan& plan) {
    auto sb = ext4::read_superblock(dev);
    if (!sb) return fail(sb.error());
    auto fu = parse_uuid(sb.value().uuid);
    if (!fu) return fail(fu.error());
    JournalIdentity id;
    id.operation_uuid = TransactionJournal::random_uuid();
    id.filesystem_uuid = fu.value();
    id.device_size = dev.geometry().size_bytes;
    id.target_size = plan.target_bytes;
    auto jr = TransactionJournal::create(file, id);
    if (!jr) return fail(jr.error());
    JournalRecord rec;
    rec.type = JournalRecordType::note;
    rec.source_block = plan.target_blocks;
    rec.destination_block = plan.free_blocks_below_target;
    rec.block_count = plan.allocated_blocks_above_target;
    rec.aux = plan.relocation_move_count;
    auto ar = jr.value().append(rec);
    if (!ar) return fail(ar.error());
    auto ck = jr.value().checkpoint(OperationPhase::prepared);
    if (!ck) return fail(ck.error());
    std::cout << "journal:      " << file << "\n";
    return 0;
}

int cmd_plan_resize(const std::string& path,
                    const std::string& size_text,
                    ProgressMode mode,
                    const std::optional<std::string>& journal) {
    auto size = parse_size(size_text);
    if (!size) return fail(size.error());
    auto d = BlockDevice::open_read(path);
    if (!d) return fail(d.error());
    Progress pg(mode);
    pg.start();
    auto r = ext4::plan_shrink(d.value(), size.value(), &pg);
    pg.stop(static_cast<bool>(r));
    if (!r) return fail(r.error());
    const auto& x = r.value();
    std::cout << "operation:    shrink plan\n"
        << "current:      " << human_bytes(x.current_bytes) << "\n"
        << "target:       " << human_bytes(x.target_bytes) << "\n"
        << "move blocks:  " << x.allocated_blocks_above_target << "\n"
        << "move bytes:   " << human_bytes(x.allocated_bytes_above_target) << "\n"
        << "metadata:     " << x.metadata_blocks_above_target << " blocks above target\n"
        << "free below:   " << human_bytes(x.free_blocks_below_target * x.block_size) << "\n"
        << "alloc runs:   " << x.allocated_extent_count << "\n"
        << "free runs:    " << x.free_extent_count << "\n"
        << "copy moves:   " << x.relocation_move_count << "\n"
        << "largest move: " << human_bytes(x.largest_relocation_extent_blocks * x.block_size) << "\n"
        << "possible:     " << (x.target_possible_by_capacity ? "yes" : "no") << "\n"
        << "relocation:   " << (x.requires_relocation ? "required" : "not required") << "\n";
    for (const auto& s : x.notes) std::cout << "note:         " << s << "\n";
    if (journal) {
        const int rc = write_plan_journal(*journal, d.value(), x);
        if (rc != 0) return rc;
    }
    return x.target_possible_by_capacity ? 0 : 4;
}

int cmd_owner_map(const std::string& path,
                  const std::optional<std::string>& from_text,
                  ProgressMode mode) {
    auto d = BlockDevice::open_read(path);
    if (!d) return fail(d.error());
    std::uint64_t from = 0;
    if (from_text) {
        auto z = parse_size(*from_text);
        if (!z) return fail(z.error());
        auto sb = ext4::read_superblock(d.value());
        if (!sb) return fail(sb.error());
        from = z.value() / sb.value().block_size();
    }
    Progress pg(mode);
    pg.start();
    auto r = ext4::scan_extent_owners(d.value(), from, &pg);
    pg.stop(static_cast<bool>(r));
    if (!r) return fail(r.error());
    const auto& x = r.value();
    std::uint64_t blocks = 0;
    for (const auto& e : x.extents) blocks += e.length;
    std::cout << "inodes examined:   " << x.inodes_examined << "\n"
        << "allocated inodes:  " << x.allocated_inodes << "\n"
        << "extent inodes:     " << x.extent_inodes << "\n"
        << "legacy inodes:     " << x.legacy_blockmap_inodes << "\n"
        << "extent records:    " << x.total_extent_records << "\n"
        << "retained extents:  " << x.extents.size() << "\n"
        << "retained blocks:   " << blocks << "\n"
        << "tree blocks:       " << x.tree_blocks.size() << "\n";
    return 0;
}

int print_relocation_plan(const ext4::OwnerRelocationPlan& x) {
    std::cout << "target:          " << human_bytes(x.target_bytes) << "\n"
        << "data blocks:     " << x.data_blocks_to_move << "\n"
        << "tree blocks:     " << x.tree_blocks_to_move << "\n"
        << "moves:           " << x.moves.size() << "\n"
        << "fixed metadata:  " << x.fixed_metadata_blocks_above_target << " blocks\n"
        << "unknown tail:    " << x.unknown_allocated_blocks_above_target << " blocks\n"
        << "legacy inodes:   " << x.legacy_blockmap_inodes << "\n"
        << "destinations:    "
        << (x.destinations_available ? "available" : "insufficient contiguous space") << "\n"
        << "owner coverage:  " << (x.complete_owner_coverage ? "complete" : "incomplete") << "\n";
    return x.destinations_available ? 0 : 4;
}

Result<ext4::OwnerRelocationPlan> relocation_plan(BlockDevice& dev,
                                                  std::uint64_t bytes,
                                                  Progress* pg) {
    return ext4::build_owner_relocation_plan(dev, bytes, pg);
}

int cmd_plan_relocate(const std::string& path,
                      const std::string& size_text,
                      ProgressMode mode) {
    auto z = parse_size(size_text);
    if (!z) return fail(z.error());
    auto d = BlockDevice::open_read(path);
    if (!d) return fail(d.error());
    Progress pg(mode);
    pg.start();
    auto r = relocation_plan(d.value(), z.value(), &pg);
    pg.stop(static_cast<bool>(r));
    if (!r) return fail(r.error());
    return print_relocation_plan(r.value());
}

int cmd_analyze_shrink(const std::string& path,
                       const std::string& size_text,
                       ProgressMode mode) {
    auto z = parse_size(size_text);
    if (!z) return fail(z.error());
    auto d = BlockDevice::open_read(path);
    if (!d) return fail(d.error());
    Progress pg(mode);
    pg.start();
    auto r = ext4::analyze_shrink_readiness(d.value(), z.value(), &pg);
    pg.stop(static_cast<bool>(r));
    if (!r) return fail(r.error());
    const auto& x = r.value();
    std::cout << "current blocks:    " << x.current_blocks << "\n"
        << "target blocks:     " << x.target_blocks << "\n"
        << "current groups:    " << x.current_groups << "\n"
        << "target groups:     " << x.target_groups << "\n"
        << "tail allocated:    " << x.allocated_tail_blocks << " blocks\n"
        << "tail metadata:     " << x.fixed_metadata_tail_blocks << " blocks\n"
        << "removed inodes:    " << x.allocated_inodes_in_removed_groups << "\n"
        << "inode relocation:  " << (x.requires_inode_relocation ? "required" : "not required") << "\n"
        << "finalize support:  " << (x.metadata_only_finalize_possible ? "ready" : "blocked") << "\n";
    for (const auto& b : x.blockers) std::cout << "blocker:           " << b << "\n";
    return x.metadata_only_finalize_possible ? 0 : 6;
}

int cmd_stage_resize(const std::string& path,
                     const std::string& size_text,
                     ProgressMode mode,
                     const std::vector<std::string>& args,
                     bool require_write) {
    auto j = option_value(args, "--journal=");
    if (!j) return fail(Error{Errc::usage, 0, "stage resize requires --journal=FILE"});
    if (require_write) {
        auto wr = require_write_flag(args);
        if (!wr) return fail(wr.error());
    }
    auto z = parse_size(size_text);
    if (!z) return fail(z.error());
    auto d = BlockDevice::open_write(path);
    if (!d) return fail(d.error());
    Progress pg(mode);
    pg.start();
    auto plan = relocation_plan(d.value(), z.value(), &pg);
    if (!plan) {
        pg.stop(false);
        return fail(plan.error());
    }
    if (!plan.value().destinations_available) {
        pg.stop(false);
        return fail(Error{Errc::unsafe, 0, "relocation destinations are insufficient"});
    }
    if (!plan.value().complete_owner_coverage) {
        pg.stop(false);
        return fail(Error{
                    Errc::unsupported, 0, "tail allocation owner coverage is incomplete; staging refused"
                    }
                   );
    }
    ext4::StageOptions o;
    o.journal_path = *j;
    o.allow_block_device = allow_block_device(args);
    auto r = ext4::stage_relocation(d.value(), plan.value(), o, &pg);
    pg.stop(static_cast<bool>(r) && r.value().complete);
    if (!r) return fail(r.error());
    std::cout << "stage:            "
        << (r.value().complete ? "complete" : (r.value().paused ? "paused" : "incomplete")) << "\n"
        << "moves:            " << r.value().moves_completed << " / " << r.value().moves_total << "\n"
        << "copied:           " << human_bytes(r.value().bytes_copied) << "\n"
        << "verified:         " << human_bytes(r.value().bytes_verified) << "\n"
        << "journal:          " << *j << "\n"
        << "filesystem meta:  unchanged\n";
    return r.value().paused ? 5 : 0;
}

int cmd_commit_resize(const std::string& path,
                      const std::string& size_text,
                      ProgressMode mode,
                      const std::vector<std::string>& args) {
    auto wr = require_write_flag(args);
    if (!wr) return fail(wr.error());
    auto j = option_value(args, "--journal=");
    if (!j) return fail(Error{Errc::usage, 0, "commit resize requires --journal=FILE"});
    auto z = parse_size(size_text);
    if (!z) return fail(z.error());
    auto d = BlockDevice::open_write(path);
    if (!d) return fail(d.error());
    Progress pg(mode);
    pg.start();
    auto plan = relocation_plan(d.value(), z.value(), &pg);
    if (!plan) {
        pg.stop(false);
        return fail(plan.error());
    }
    if (!plan.value().complete_owner_coverage) {
        pg.stop(false);
        return fail(Error{
                    Errc::unsupported, 0, "tail allocation owner coverage is incomplete; metadata commit refused"
                    }
                   );
    }
    ext4::CommitOptions o;
    o.journal_path = *j;
    o.allow_block_device = allow_block_device(args);
    auto r = ext4::commit_staged_relocation(d.value(), plan.value(), o, &pg);
    pg.stop(static_cast<bool>(r) && r.value().complete);
    if (!r) return fail(r.error());
    std::cout << "commit:            "
        << (r.value().complete ? "complete" : (r.value().paused ? "paused" : "incomplete")) << "\n"
        << "moves:             " << r.value().moves_committed << " / " << r.value().moves_total << "\n"
        << "reserved:          " << r.value().destinations_reserved << "\n"
        << "owners switched:   " << r.value().owners_switched << "\n"
        << "sources released:  " << r.value().sources_released << "\n"
        << "relocated:         " << human_bytes(r.value().bytes_relocated) << "\n"
        << "filesystem clean:  " << (r.value().filesystem_clean ? "yes" : "no") << "\n"
        << "filesystem size:   unchanged\n";
    return r.value().paused ? 5 : 0;
}

int cmd_compact_resize(const std::string& path,
                       const std::string& size_text,
                       ProgressMode mode,
                       const std::vector<std::string>& args) {
    auto wr = require_write_flag(args);
    if (!wr) return fail(wr.error());
    const int stage = cmd_stage_resize(path, size_text, mode, args, false);
    if (stage != 0) return stage;
    return cmd_commit_resize(path, size_text, mode, args);
}

int cmd_finalize_shrink(const std::string& path,
                        const std::string& size_text,
                        ProgressMode mode,
                        const std::vector<std::string>& args) {
    auto wr = require_write_flag(args);
    if (!wr) return fail(wr.error());
    auto j = option_value(args, "--journal=");
    if (!j) return fail(Error{Errc::usage, 0, "finalize shrink requires --journal=FILE"});
    auto z = parse_size(size_text);
    if (!z) return fail(z.error());
    auto d = BlockDevice::open_write(path);
    if (!d) return fail(d.error());
    ext4::FinalizeOptions o;
    o.journal_path = *j;
    o.allow_block_device = allow_block_device(args);
    Progress pg(mode);
    pg.start();
    auto r = ext4::finalize_metadata_only_shrink(d.value(), z.value(), o, &pg);
    pg.stop(static_cast<bool>(r));
    if (!r) return fail(r.error());
    auto sb = ext4::read_superblock(d.value());
    if (!sb) return fail(sb.error());
    std::cout << "shrink:       complete\n"
        << "filesystem:   " << human_bytes(sb.value().size_bytes()) << "\n"
        << "note:         outer partition has not been resized\n";
    return 0;
}


int cmd_grow(const std::string& path,
             const std::string& size_text,
             ProgressMode mode,
             const std::vector<std::string>& args) {
    auto wr = require_write_flag(args);
    if (!wr) return fail(wr.error());
    auto z = parse_size(size_text);
    if (!z) return fail(z.error());
    auto d = BlockDevice::open_write(path);
    if (!d) return fail(d.error());
    ext4::GrowOptions o;
    o.allow_block_device = allow_block_device(args);
    Progress pg(mode);
    pg.start();
    auto r = ext4::grow_within_last_group(d.value(), z.value(), o, &pg);
    pg.stop(static_cast<bool>(r));
    if (!r) return fail(r.error());
    std::cout << "grow:         complete\n"
        << "old blocks:   " << r.value().old_blocks << "\n"
        << "new blocks:   " << r.value().new_blocks << "\n"
        << "added:        " << r.value().blocks_added << " blocks\n"
        << "free blocks:  " << r.value().free_blocks_after << "\n"
        << "verified:     " << (r.value().verified ? "yes" : "no") << "\n";
    return 0;
}

int cmd_resize_end_to_end(const std::string& path,
                          const std::string& size_text,
                          ProgressMode mode,
                          const std::vector<std::string>& args) {
    auto wr = require_write_flag(args);
    if (!wr) return fail(wr.error());
    auto z = parse_size(size_text);
    if (!z) return fail(z.error());
    {
        auto d = BlockDevice::open_read(path);
        if (!d) return fail(d.error());
        auto sb = ext4::read_superblock(d.value());
        if (!sb) return fail(sb.error());
        if (z.value() == sb.value().size_bytes()) {
            std::cout << "resize:       no change\n"
                << "filesystem:   " << human_bytes(sb.value().size_bytes()) << "\n";
            return 0;
        }
        if (z.value() > sb.value().size_bytes())
            return cmd_grow(path, size_text, mode, args);
    }
    auto j = option_value(args, "--journal=");
    if (!j) return fail(Error{Errc::usage, 0, "shrink resize requires --journal=FILE"});

    // Fail before copying anything when the final size transition would need
    // inode/group removal that this release intentionally does not implement.
    {
        auto d = BlockDevice::open_read(path);
        if (!d) return fail(d.error());
        auto r = ext4::analyze_shrink_readiness(d.value(), z.value(), nullptr);
        if (!r) return fail(r.error());
        if (r.value().target_groups < r.value().current_groups)
            return fail(Error{Errc::unsupported, 0,
                        "this target removes ext4 block groups; FSX 1.0 refuses the operation before "
                        "relocation because inode-number relocation is not implemented safely"
                        }
                       );
    }

    const int compact = cmd_compact_resize(path, size_text, mode, args);
    if (compact != 0) return compact;
    const int final = cmd_finalize_shrink(path, size_text, mode, args);
    if (final != 0) return final;

    const auto pdisk = option_value(args, "--partition-disk=");
    const auto pindex = option_value(args, "--partition-index=");
    if (pdisk.has_value() != pindex.has_value())
        return fail(Error{
                    Errc::usage, 0, "--partition-disk and --partition-index must be supplied together"
                    }
                   );
    if (pdisk) {
        auto idx = parse_u32(*pindex, "partition index");
        if (!idx) return fail(idx.error());
        return cmd_part_sync(*pdisk, idx.value(), path, mode, args);
    }
    return 0;
}

int cmd_part_list(const std::string& path) {
    auto d = BlockDevice::open_read(path);
    if (!d) return fail(d.error());
    auto r = gpt::read_redundant_table(d.value());
    if (!r) return fail(r.error());
    const auto& t = r.value();
    std::cout << "partition table: GPT\n"
        << "primary header:  " << (t.primary_header_crc_ok ? "ok" : "BAD") << "\n"
        << "primary entries: " << (t.entries_crc_ok ? "ok" : "BAD") << "\n"
        << "backup header:   " << (t.backup_header_crc_ok ? "ok" : "BAD") << "\n"
        << "backup entries:  " << (t.backup_entries_crc_ok ? "ok" : "BAD") << "\n"
        << "protective MBR:  " << (t.protective_mbr_ok ? "ok" : "BAD") << "\n"
        << "usable LBA:      " << t.first_usable_lba << " - " << t.last_usable_lba << "\n\n"
        << "idx  first_lba        last_lba         size             type                                  name\n";
    for (const auto& x : t.partitions) {
        const auto sectors = x.last_lba >= x.first_lba ? x.last_lba - x.first_lba + 1 : 0;
        const auto bytes = sectors * d.value().geometry().logical_sector;
        std::cout << std::setw(3) << x.index << "  "
            << std::setw(15) << x.first_lba << "  "
            << std::setw(15) << x.last_lba << "  "
            << std::setw(15) << human_bytes(bytes) << "  "
            << gpt::guid_to_string(x.type_guid) << "  " << x.name << "\n";
    }
    return (t.primary_header_crc_ok && t.entries_crc_ok &&
            t.backup_header_crc_ok && t.backup_entries_crc_ok) ? 0 : 3;
}

int cmd_part_resize(const std::vector<std::string>& a, ProgressMode mode) {
    auto wr = require_write_flag(a);
    if (!wr) return fail(wr.error());
    if (a.size() < 6) return fail(Error{
                                  Errc::usage, 0, "partition resize requires DISK INDEX FIRST_LBA LAST_LBA"
                                  }
                                 );
    auto idx = parse_u32(a[3], "partition index");
    auto first = parse_u64(a[4], "first LBA");
    auto last = parse_u64(a[5], "last LBA");
    if (!idx) return fail(idx.error());
    if (!first) return fail(first.error());
    if (!last) return fail(last.error());
    auto d = BlockDevice::open_write(a[2]);
    if (!d) return fail(d.error());
    gpt::MutationOptions o;
    o.allow_block_device = allow_block_device(a);
    Progress pg(mode);
    pg.start();
    auto r = gpt::resize_partition(d.value(), idx.value(), first.value(), last.value(), o, &pg);
    pg.stop(static_cast<bool>(r));
    if (!r) return fail(r.error());
    std::cout << "backup written:  " << (r.value().backup_written ? "yes" : "no") << "\n"
        << "primary written: " << (r.value().primary_written ? "yes" : "no") << "\n"
        << "verified:        " << (r.value().verified ? "yes" : "no") << "\n"
        << "kernel reread:   "
        << (r.value().kernel_reread_requested ? "requested" : "not requested") << "\n";
    return 0;
}


int cmd_part_sync(const std::string& disk_path,
                  std::uint32_t index,
                  const std::string& fs_path,
                  ProgressMode mode,
                  const std::vector<std::string>& args) {
    auto wr = require_write_flag(args);
    if (!wr) return fail(wr.error());
    auto fsdev = BlockDevice::open_read(fs_path);
    if (!fsdev) return fail(fsdev.error());
    auto disk = BlockDevice::open_write(disk_path);
    if (!disk) return fail(disk.error());
    PartitionSyncOptions o;
    o.allow_block_device = allow_block_device(args);
    if (auto j = option_value(args, "--journal=")) o.journal_path = *j;
    Progress pg(mode);
    pg.start();
    auto r = sync_gpt_partition_to_ext4(fsdev.value(), disk.value(), index, o, &pg);
    pg.stop(static_cast<bool>(r));
    if (!r) return fail(r.error());
    std::cout << "partition sync:  complete\n"
        << "index:           " << r.value().partition_index << "\n"
        << "first LBA:       " << r.value().first_lba << "\n"
        << "old last LBA:    " << r.value().old_last_lba << "\n"
        << "new last LBA:    " << r.value().new_last_lba << "\n"
        << "filesystem:      " << human_bytes(r.value().filesystem_bytes) << "\n"
        << "changed:         " << (r.value().changed ? "yes" : "no") << "\n"
        << "verified:        " << (r.value().verified ? "yes" : "no") << "\n";
    return 0;
}


int cmd_part_backup(const std::string& disk_path, const std::string& backup_path) {
    auto d = BlockDevice::open_read(disk_path);
    if (!d) return fail(d.error());
    auto r = gpt::backup_metadata(d.value(), backup_path);
    if (!r) return fail(r.error());
    std::cout << "GPT backup:      complete\n"
        << "disk:            " << disk_path << "\n"
        << "file:            " << backup_path << "\n";
    return 0;
}

int cmd_part_restore(const std::string& disk_path,
                     const std::string& backup_path,
                     const std::vector<std::string>& args) {
    auto wr = require_write_flag(args);
    if (!wr) return fail(wr.error());
    auto d = BlockDevice::open_write(disk_path);
    if (!d) return fail(d.error());
    gpt::MutationOptions o;
    o.allow_block_device = allow_block_device(args);
    auto r = gpt::restore_metadata(d.value(), backup_path, o);
    if (!r) return fail(r.error());
    std::cout << "GPT restore:     complete\n"
        << "backup written:  " << (r.value().backup_written ? "yes" : "no") << "\n"
        << "primary written: " << (r.value().primary_written ? "yes" : "no") << "\n"
        << "verified:        " << (r.value().verified ? "yes" : "no") << "\n";
    return 0;
}

int cmd_part_repair(const std::vector<std::string>& a) {
    auto wr = require_write_flag(a);
    if (!wr) return fail(wr.error());
    auto d = BlockDevice::open_write(a[2]);
    if (!d) return fail(d.error());
    gpt::MutationOptions o;
    o.allow_block_device = allow_block_device(a);
    auto r = gpt::repair_redundancy(d.value(), o);
    if (!r) return fail(r.error());
    std::cout << "backup written:  " << (r.value().backup_written ? "yes" : "no") << "\n"
        << "primary written: " << (r.value().primary_written ? "yes" : "no") << "\n"
        << "verified:        " << (r.value().verified ? "yes" : "no") << "\n";
    return 0;
}

int cmd_mbr_list(const std::string& path) {
    auto d = BlockDevice::open_read(path);
    if (!d) return fail(d.error());
    auto r = mbr::read_table(d.value());
    if (!r) return fail(r.error());
    const auto& t = r.value();
    std::cout << "partition table: MBR\n"
        << "signature:       " << (t.signature_ok ? "ok" : "BAD") << "\n"
        << "protective:      " << (t.protective ? "yes" : "no") << "\n"
        << "hybrid:          " << (t.hybrid ? "yes" : "no") << "\n"
        << "disk signature:  0x" << std::hex << std::setw(8)
        << std::setfill('0') << t.disk_signature << std::dec << std::setfill(' ') << "\n\n"
        << "idx  boot  type  first_lba    sectors      last_lba\n";
    for (const auto& p : t.partitions)
        std::cout << std::setw(3) << p.index << "  " << (p.bootable ? "yes " : "no  ")
            << " 0x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(p.type)
            << std::dec << std::setfill(' ') << "  " << std::setw(11) << p.first_lba
            << "  " << std::setw(11) << p.sectors << "  " << p.last_lba() << "\n";
    return 0;
}

int cmd_mbr_resize(const std::vector<std::string>& a) {
    auto wr = require_write_flag(a);
    if (!wr) return fail(wr.error());
    if (a.size() < 6) return fail(Error{Errc::usage, 0, "mbr resize requires DISK INDEX FIRST_LBA SECTORS"});
    auto backup = option_value(a, "--backup-sector=");
    if (!backup) return fail(Error{Errc::usage, 0, "mbr resize requires --backup-sector=FILE"});
    auto idx = parse_u32(a[3], "partition index");
    auto first = parse_u32(a[4], "first LBA");
    auto sectors = parse_u32(a[5], "sector count");
    if (!idx) return fail(idx.error());
    if (!first) return fail(first.error());
    if (!sectors) return fail(sectors.error());
    auto d = BlockDevice::open_write(a[2]);
    if (!d) return fail(d.error());
    mbr::MutationOptions o;
    o.allow_block_device = allow_block_device(a);
    o.backup_sector_path = *backup;
    auto r = mbr::resize_partition(d.value(), idx.value(), first.value(), sectors.value(), o);
    if (!r) return fail(r.error());
    std::cout << "mbr resize:    complete\nbackup sector: " << *backup << "\n";
    return 0;
}

int cmd_mbr_restore(const std::vector<std::string>& a) {
    auto wr = require_write_flag(a);
    if (!wr) return fail(wr.error());
    auto backup = option_value(a, "--backup-sector=");
    if (!backup) return fail(Error{Errc::usage, 0, "mbr restore requires --backup-sector=FILE"});
    auto d = BlockDevice::open_write(a[2]);
    if (!d) return fail(d.error());
    auto r = mbr::restore_sector(d.value(), *backup, allow_block_device(a));
    if (!r) return fail(r.error());
    std::cout << "mbr restore:   complete\n";
    return 0;
}

int cmd_io_profile(const std::string& path) {
    auto d = BlockDevice::open_read(path);
    if (!d) return fail(d.error());
    auto p = profile_device(d.value());
    if (!p) return fail(p.error());
    std::cout << "device class:  " << device_class_name(p.value().device_class) << "\n"
        << "workers:       " << p.value().tuning.workers << "\n"
        << "queue depth:   " << p.value().tuning.queue_depth << "\n"
        << "large io:      " << human_bytes(p.value().tuning.large_io_bytes) << "\n"
        << "selection:     " << p.value().rationale << "\n";
    return 0;
}

int cmd_bench(const std::string& path, std::uint64_t bytes, ProgressMode mode) {
    auto d = BlockDevice::open_read(path);
    if (!d) return fail(d.error());
    Progress pg(mode);
    pg.start();
    auto r = benchmark_read(d.value(), bytes, &pg);
    pg.stop(static_cast<bool>(r));
    if (!r) return fail(r.error());
    std::cout << "read:         " << human_bytes(r.value().bytes_read) << "\n"
        << "elapsed:      " << format_duration(r.value().seconds) << "\n"
        << "rate:         " << human_rate(r.value().bytes_per_second) << "\n";
    return 0;
}


int cmd_scrub(const std::string& path, std::optional<std::string> size_text,
              ProgressMode mode, const std::vector<std::string>& args) {
    auto d = BlockDevice::open_read(path);
    if (!d) return fail(d.error());
    ScrubOptions o;
    if (size_text) {
        auto z = parse_size(*size_text);
        if (!z)return fail(z.error());
        o.bytes = z.value();
    }
    if (auto p = option_value(args,"--passes=")) {
        auto n = parse_u32(*p,"passes");
        if (!n)return fail(n.error());
        o.passes = n.value();
    }
    if (auto c = option_value(args,"--chunk=")) {
        auto z = parse_size(*c);
        if (!z)return fail(z.error());
        if (z.value()>std::numeric_limits<std::size_t>::max())return fail(Error{
                                                                          Errc::invalid_argument, 0,
                                                                          "chunk size exceeds address space"
                                                                          }
                                                                         );
        o.chunk_bytes = static_cast<std::size_t>(z.value());
    }
    Progress pg(mode);
    pg.start();
    auto r = scrub_read(d.value(), o, &pg);
    pg.stop(static_cast<bool>(r));
    if (!r)return fail(r.error());
    std::cout << "scrub:        clean\n"
        << "passes:       " << r.value().passes_completed << "\n"
        << "read:         " << human_bytes(r.value().total_bytes_read) << "\n"
        << "crc32c:       0x" << std::hex << std::setw(8)
        << std::setfill('0') << r.value().crc32c << std::dec << std::setfill(' ') << "\n"
        << "elapsed:      " << format_duration(r.value().seconds) << "\n"
        << "rate:         " << human_rate(r.value().bytes_per_second) << "\n";
    return 0;
}

int cmd_clone(const std::string& source_path, const std::string& target_path,
              ProgressMode mode, const std::vector<std::string>& args) {
    auto wr = require_write_flag(args);
    if (!wr)return fail(wr.error());
    auto source = BlockDevice::open_read(source_path);
    if (!source)return fail(source.error());
    Result<BlockDevice> target = Error{Errc::open_failed, 0,"uninitialized clone target"};
    if (has_flag(args,"--create-file")) {
        struct stat st{};
        if (::lstat(target_path.c_str(), &st) == 0)return fail(Error{
                                                               Errc::unsafe, 0,
                                                               "--create-file refuses to overwrite an existing path"
                                                               }
                                                              );
        if (errno != ENOENT)return fail(from_errno(Errc::open_failed,"cannot inspect clone target", errno));
        target = BlockDevice::create_file(target_path, source.value().geometry().size_bytes);
    } else target = BlockDevice::open_write(target_path);
    if (!target)return fail(target.error());
    CloneOptions o;
    o.allow_block_device = allow_block_device(args);
    o.allow_live_source = has_flag(args,"--allow-live-source");
    if (auto c = option_value(args,"--chunk=")) {
        auto z = parse_size(*c);
        if (!z)return fail(z.error());
        if (z.value()>std::numeric_limits<std::size_t>::max())return fail(Error{
                                                                          Errc::invalid_argument, 0,
                                                                          "chunk size exceeds address space"
                                                                          }
                                                                         );
        o.chunk_bytes = static_cast<std::size_t>(z.value());
    }
    Progress pg(mode);
    pg.start();
    auto r = clone_device(source.value(), target.value(), o, &pg);
    pg.stop(static_cast<bool>(r));
    if (!r)return fail(r.error());
    std::cout << "clone:        complete\n"
        << "copied:       " << human_bytes(r.value().bytes_copied) << "\n"
        << "verified:     " << (r.value().verified?"yes":"no") << "\n"
        << "verify bytes: " << human_bytes(r.value().bytes_verified) << "\n"
        << "copy rate:    " << human_rate(r.value().copy_bytes_per_second) << "\n";
    return 0;
}

int cmd_journal_verify(const std::string& file) {
    auto j = TransactionJournal::open(file, false);
    if (!j)return fail(j.error());
    auto r = validate_journal_semantics(j.value());
    if (!r)return fail(r.error());
    std::cout << "journal:      " << file << "\n"
        << "result:       " << (r.value().clean?"clean":"errors found") << "\n"
        << "transactions: " << r.value().transactions << "\n"
        << "intents:      " << r.value().intents << "\n"
        << "verified:     " << r.value().copies_verified << "\n"
        << "reserved:     " << r.value().destinations_reserved << "\n"
        << "switched:     " << r.value().owners_switched << "\n"
        << "released:     " << r.value().sources_released << "\n"
        << "committed:    " << r.value().metadata_committed << "\n";
    for (const auto&w:r.value().warnings)std::cout<<"warning:      "<<w<<"\n";
    for (const auto&e:r.value().errors)std::cout<<"error:        "<<e<<"\n";
    return r.value().clean?0:3;
}

int cmd_journal_inspect(const std::string& file, bool dump, std::size_t limit) {
    auto j = TransactionJournal::open(file, false);
    if (!j) return fail(j.error());
    const auto& s = j.value().status();
    const auto a = assess_recovery(s);
    std::cout << "journal:      " << file << "\n"
        << "operation:    " << format_uuid(s.identity.operation_uuid) << "\n"
        << "filesystem:   " << format_uuid(s.identity.filesystem_uuid) << "\n"
        << "phase:        " << operation_phase_name(s.phase) << "\n"
        << "sequence:     " << s.header_sequence << "\n"
        << "records:      " << s.record_count << "\n"
        << "committed:    " << s.committed_transactions << "\n"
        << "header A:     " << (s.header_a_valid ? "valid" : "invalid") << "\n"
        << "header B:     " << (s.header_b_valid ? "valid" : "invalid") << "\n"
        << "records CRC:  " << (s.records_valid ? "valid" : "invalid") << "\n"
        << "recovery:     " << recovery_action_name(a.action) << "\n"
        << "reason:       " << a.reason << "\n";
    if (dump) {
        auto rr = j.value().records();
        if (!rr) return fail(rr.error());
        const auto n = std::min(limit, rr.value().size());
        for (std::size_t i = 0; i < n; ++i) {
            const auto& r = rr.value()[i];
            std::cout << "record " << i + 1
                << ": type=" << journal_record_type_name(r.type)
                << " txn=" << r.transaction_id
                << " src=" << r.source_block
                << " dst=" << r.destination_block
                << " blocks=" << r.block_count
                << " inode=" << r.inode
                << " aux=" << r.aux << "\n";
        }
    }
    return 0;
}

int cmd_image_create(const std::string& source_path, const std::string& image_path,
                     ProgressMode mode, const std::vector<std::string>& args) {
    auto d = BlockDevice::open_read(source_path);
    if (!d)return fail(d.error());
    if (d.value().geometry().is_block_device && !has_flag(args,"--allow-live-source")) {
        auto safety = inspect_device_safety(d.value());
        if (!safety)return fail(safety.error());
        if (safety.value().mounted || safety.value().active_swap
            || safety.value().descendant_mounted || safety.value().descendant_swap || safety.value().has_holders)
            return fail(Error{
                        Errc::unsafe, 0,
                        "source block device is active; use --allow-live-source only if a crash-consistent "
                        "live image is acceptable"
                        }
                       );
    }
    image::CreateOptions o;
    if (auto c = option_value(args,"--chunk=")) {
        auto z = parse_size(*c);
        if (!z)return fail(z.error());
        if (z.value()>std::numeric_limits<std::uint32_t>::max())return fail(Error{
                                                                            Errc::invalid_argument, 0,
                                                                            "image chunk size is too large"
                                                                            }
                                                                           );
        o.chunk_bytes = static_cast<std::uint32_t>(z.value());
    }
    Progress pg(mode);
    pg.start();
    auto r = image::create(d.value(), image_path, o, &pg);
    pg.stop(static_cast<bool>(r));
    if (!r)return fail(r.error());
    std::cout<<"image:        "<<image_path<<"\nsource bytes: "<<r.value().source_bytes<<"\nchunks:       "
        <<r.value().chunks<<"\nzero chunks:  "<<r.value().zero_chunks
        <<"\nstored data:  "<<human_bytes(r.value().stored_payload_bytes)<<"\n";
    return 0;
}
int cmd_image_info(const std::string& path) {
    auto r = image::inspect(path);
    if (!r)return fail(r.error());
    std::cout<<"source size:  "<<human_bytes(r.value().source_bytes)<<"\nsector:       "<<r.value().logical_sector
        <<"\nchunk:        "<<human_bytes(r.value().chunk_bytes)<<"\nchunks:       "
        <<r.value().chunks<<"\ncomplete:     "<<(r.value().complete?"yes":"no")<<"\n";
    return 0;
}
int cmd_image_verify(const std::string& path, ProgressMode mode) {
    Progress pg(mode);
    pg.start();
    auto r = image::verify(path, &pg);
    pg.stop(static_cast<bool>(r));
    if (!r)return fail(r.error());
    std::cout<<"image:        valid\nsource size:  "<<human_bytes(r.value().source_bytes)<<"\nchunks:       "
        <<r.value().chunks<<"\nzero chunks:  "<<r.value().zero_chunks
        <<"\nstored data:  "<<human_bytes(r.value().stored_payload_bytes)<<"\n";
    return 0;
}
int cmd_image_restore(const std::string& image_path, const std::string& target_path,
                      ProgressMode mode, const std::vector<std::string>& args) {
    auto wr = require_write_flag(args);
    if (!wr)return fail(wr.error());
    Result<BlockDevice> d = Error{Errc::open_failed, 0,"uninitialized restore target"};
    if (has_flag(args,"--create-file")) {
        struct stat st{};
        if (::lstat(target_path.c_str(), &st) == 0)return fail(Error{
                                                               Errc::unsafe, 0,
                                                               "--create-file refuses to overwrite an existing path"
                                                               }
                                                              );
        if (errno != ENOENT)return fail(from_errno(Errc::open_failed,"cannot inspect restore target", errno));
        auto i = image::inspect(image_path);
        if (!i)return fail(i.error());
        d = BlockDevice::create_file(target_path, i.value().source_bytes);
    }
    else d = BlockDevice::open_write(target_path);
    if (!d) return fail(d.error());
    Progress pg(mode);
    pg.start();
    auto r = image::restore(image_path, d.value(), allow_block_device(args), &pg);
    pg.stop(static_cast<bool>(r));
    if (!r) return fail(r.error());
    std::cout << "restore:      complete\nverified:     yes\nbytes:        "
        << human_bytes(r.value().source_bytes) << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Cancellation::install_signal_handlers();
    Cancellation::reset();
    std::vector<std::string> a;
    for (int i = 1; i < argc; ++i) a.emplace_back(argv[i]);
    if (a.empty() || a[0] == "--help" || a[0] == "-h") {
        usage();
        return 0;
    }
    if (a[0] == "--version") {
        std::cout << "fsx 1.0.0\n";
        return 0;
    }
    const auto pm = get_progress(a);

    if (a[0] == "info" && a.size() >= 2) return cmd_info(a[1]);
    if (a[0] == "probe" && a.size() >= 2) return cmd_probe(a[1]);
    if (a[0] == "capabilities" && a.size() >= 2) return cmd_capabilities(a[1]);
    if (a.size() >= 3 && a[0] == "check" && a[1] == "deep")
        return cmd_check_deep(a[2], pm);
    if (a.size() >= 3 && a[0] == "check" && a[1] == "metadata")
        return cmd_check_metadata(a[2], pm, has_flag(a, "--quick"));
    if (a[0] == "check" && a.size() >= 2) return cmd_check(a[1], pm);
    if (a.size() >= 3 && a[0] == "repair" && a[1] == "counters")
        return cmd_repair_counters(a[2], pm, a);
    if (a.size() >= 4 && a[0] == "tune" && a[1] == "label")
        return cmd_tune_label(a[2], a[3], a);
    if (a[0] == "allocation" && a.size() >= 2) return cmd_allocation(a[1], pm);
    if (a[0] == "owner-map" && a.size() >= 2) {
        std::optional<std::string> from;
        if (a.size() >= 3 && a[2].rfind("--", 0) != 0) from = a[2];
        return cmd_owner_map(a[1], from, pm);
    }
    if (a.size() >= 4 && a[0] == "plan" && a[1] == "resize")
        return cmd_plan_resize(a[2], a[3], pm, option_value(a, "--journal="));
    if (a.size() >= 4 && a[0] == "plan" && a[1] == "relocate")
        return cmd_plan_relocate(a[2], a[3], pm);
    if (a.size() >= 4 && a[0] == "analyze" && a[1] == "shrink")
        return cmd_analyze_shrink(a[2], a[3], pm);
    if (a.size() >= 4 && a[0] == "stage" && a[1] == "resize")
        return cmd_stage_resize(a[2], a[3], pm, a, true);
    if (a.size() >= 4 && a[0] == "commit" && a[1] == "resize")
        return cmd_commit_resize(a[2], a[3], pm, a);
    if (a.size() >= 4 && a[0] == "compact" && a[1] == "resize")
        return cmd_compact_resize(a[2], a[3], pm, a);
    if (a.size() >= 4 && a[0] == "finalize" && a[1] == "shrink")
        return cmd_finalize_shrink(a[2], a[3], pm, a);
    if (a.size() >= 3 && a[0] == "resize")
        return cmd_resize_end_to_end(a[1], a[2], pm, a);
    if (a.size() >= 3 && a[0] == "grow")
        return cmd_grow(a[1], a[2], pm, a);

    if (a.size() >= 3 && a[0] == "partition" && a[1] == "list") return cmd_part_list(a[2]);
    if (a.size() >= 6 && a[0] == "partition" && a[1] == "resize") return cmd_part_resize(a, pm);
    if (a.size() >= 5 && a[0] == "partition" && a[1] == "sync") {
        auto idx = parse_u32(a[3], "partition index");
        if (!idx) return fail(idx.error());
        return cmd_part_sync(a[2], idx.value(), a[4], pm, a);
    }
    if (a.size() >= 4 && a[0] == "partition" && a[1] == "backup") return cmd_part_backup(a[2], a[3]);
    if (a.size() >= 4 && a[0] == "partition" && a[1] == "restore") return cmd_part_restore(a[2], a[3], a);
    if (a.size() >= 3 && a[0] == "partition" && a[1] == "repair") return cmd_part_repair(a);
    if (a.size() >= 3 && a[0] == "mbr" && a[1] == "list") return cmd_mbr_list(a[2]);
    if (a.size() >= 6 && a[0] == "mbr" && a[1] == "resize") return cmd_mbr_resize(a);
    if (a.size() >= 3 && a[0] == "mbr" && a[1] == "restore") return cmd_mbr_restore(a);

    if (a[0] == "io-profile" && a.size() >= 2) return cmd_io_profile(a[1]);
    if (a[0] == "benchmark" && a.size() >= 2) {
        std::uint64_t n = 1ULL << 30U;
        if (a.size() >= 3 && a[2].rfind("--", 0) != 0) {
            auto z = parse_size(a[2]);
            if (!z) return fail(z.error());
            n = z.value();
        }
        return cmd_bench(a[1], n, pm);
    }
    if (a[0] == "scrub" && a.size() >= 2) {
        std::optional<std::string> z;
        if (a.size() >= 3 && a[2].rfind("--", 0) != 0) z = a[2];
        return cmd_scrub(a[1], z, pm, a);
    }
    if (a[0] == "clone" && a.size() >= 3) return cmd_clone(a[1], a[2], pm, a);
    if (a.size() >= 4 && a[0] == "image" && a[1] == "create") return cmd_image_create(a[2], a[3], pm, a);
    if (a.size() >= 3 && a[0] == "image" && a[1] == "info") return cmd_image_info(a[2]);
    if (a.size() >= 3 && a[0] == "image" && a[1] == "verify") return cmd_image_verify(a[2], pm);
    if (a.size() >= 4 && a[0] == "image" && a[1] == "restore") return cmd_image_restore(a[2], a[3], pm, a);
    if (a.size() >= 3 && a[0] == "journal" && a[1] == "inspect")
        return cmd_journal_inspect(a[2], false, 0);
    if (a.size() >= 3 && a[0] == "journal" && a[1] == "verify")
        return cmd_journal_verify(a[2]);
    if (a.size() >= 3 && a[0] == "journal" && a[1] == "dump") {
        std::size_t limit = 64;
        if (a.size() >= 4) {
            auto n = parse_u64(a[3], "journal record limit");
            if (!n) return fail(n.error());
            limit = static_cast<std::size_t>(std::min<std::uint64_t>(n.value(),
                                                                     std::numeric_limits<std::size_t>::max()));
        }
        return cmd_journal_inspect(a[2], true, limit);
    }

    usage();
    return 1;
}
