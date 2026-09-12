#include "fsx/probe.hpp"
#include "fsx/endian.hpp"
#include "fsx/exfat.hpp"
#include "fsx/ext4.hpp"
#include "fsx/fat.hpp"
#include "fsx/xfs.hpp"
#include "fsx/ntfs.hpp"
#include "fsx/btrfs.hpp"
#include "fsx/iso9660.hpp"
#include "fsx/ufs.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

namespace fsx {
namespace {

bool bytes_equal(std::span<const std::byte> b, std::size_t off, std::string_view s) {
    if (off > b.size() || s.size() > b.size() - off) return false;
    for (std::size_t i = 0; i < s.size(); ++i)
        if (std::to_integer<unsigned char>(b[off + i]) != static_cast<unsigned char>(s[i])) return false;
    return true;
}

Result<std::vector<std::byte>> read_at_most(BlockDevice& dev, std::uint64_t off, std::size_t wanted) {
    if (off >= dev.geometry().size_bytes) return std::vector<std::byte>{};
    const auto available = dev.geometry().size_bytes - off;
    const auto n64 = std::min<std::uint64_t>(available, wanted);
    std::vector<std::byte> out(static_cast<std::size_t>(n64));
    if (!out.empty()) {
        auto r = dev.read_exact(off, out);
        if (!r) return r.error();
    }
    return out;
}


} // namespace

const char* filesystem_kind_name(FilesystemKind kind) noexcept {
    switch (kind) {
    case FilesystemKind::ext: return "ext2/3/4";
    case FilesystemKind::fat12: return "FAT12";
    case FilesystemKind::fat16: return "FAT16";
    case FilesystemKind::fat32: return "FAT32";
    case FilesystemKind::exfat: return "exFAT";
    case FilesystemKind::ufs1: return "UFS1/FFS1";
    case FilesystemKind::ufs2: return "UFS2/FFS2";
    case FilesystemKind::xfs: return "XFS";
    case FilesystemKind::btrfs: return "Btrfs";
    case FilesystemKind::ntfs: return "NTFS";
    case FilesystemKind::iso9660: return "ISO9660";
    default: return "unknown";
    }
}

Result<FilesystemProbe> probe_filesystem(BlockDevice& dev) {
    FilesystemProbe out;

    if (dev.geometry().size_bytes >= 11) {
        auto ex = exfat::probe(dev);
        if (!ex) return ex.error();
        if (ex.value()) {
            auto v = exfat::read_volume(dev);
            if (!v) return v.error();
            out.kind = FilesystemKind::exfat;
            out.name = "exFAT";
            out.label = v.value().label;
            out.size_bytes = v.value().size_bytes();
            out.block_size = v.value().bytes_per_sector * v.value().sectors_per_cluster;
            out.confident = v.value().main_boot_checksum_ok || v.value().backup_boot_checksum_ok;
            if (!v.value().main_boot_checksum_ok) out.notes.push_back("main boot checksum failed");
            if (!v.value().backup_boot_checksum_ok) out.notes.push_back("backup boot checksum failed");
            return out;
        }
    }

    if (dev.geometry().size_bytes >= 1082) {
        auto e = ext4::probe(dev);
        if (!e) return e.error();
        if (e.value()) {
            auto s = ext4::read_superblock(dev);
            if (!s) return s.error();
            out.kind = FilesystemKind::ext;
            out.name = "ext2/3/4";
            out.label = s.value().volume_name;
            out.uuid = s.value().uuid;
            out.size_bytes = s.value().size_bytes();
            out.block_size = static_cast<std::uint32_t>(s.value().block_size());
            out.confident = true;
            return out;
        }
    }

    if (dev.geometry().size_bytes >= 512) {
        auto f = fat::probe(dev);
        if (!f) return f.error();
        if (f.value()) {
            auto v = fat::read_volume(dev);
            if (!v) return v.error();
            out.kind = v.value().kind == fat::Kind::fat12 ? FilesystemKind::fat12 :
                v.value().kind == fat::Kind::fat16 ? FilesystemKind::fat16 : FilesystemKind::fat32;
            out.name = fat::kind_name(v.value().kind);
            out.label = v.value().label;
            out.size_bytes = v.value().size_bytes();
            out.block_size = v.value().cluster_bytes();
            out.confident = true;
            return out;
        }
    }

    auto first = read_at_most(dev, 0, 4096);
    if (!first) return first.error();
    const auto b = std::span<const std::byte>(first.value());
    if (bytes_equal(b, 0, "XFSB")) {
        auto xs = xfs::read_superblock(dev);
        if (!xs) return xs.error();
        const auto& x = xs.value();
        const auto pow2 = [](std::uint64_t v) {
            return v != 0 && (v&(v-1U)) == 0;
        };
        const auto xsize = x.size_bytes();
        const bool sane = pow2(x.block_size) && x.block_size >= 512 && x.block_size <= 65536 &&
            pow2(x.sector_size) && x.sector_size >= 512 && x.sector_size <= x.block_size &&
            x.data_blocks != 0
            && xsize != std::numeric_limits<std::uint64_t>::max() && xsize <= dev.geometry().size_bytes &&
            x.ag_blocks != 0 && x.ag_count != 0;
        out.kind = FilesystemKind::xfs;
        out.name = "XFS";
        out.confident = sane;
        out.label = x.label;
        out.uuid = xfs::uuid_string(x.uuid);
        out.size_bytes = xsize;
        out.block_size = x.block_size;
        if (!sane)out.notes.push_back("XFS primary superblock geometry is inconsistent");
        return out;
    }
    if (bytes_equal(b, 3, "NTFS    ")) {
        auto ns = ntfs::read_boot_sector(dev);
        if (!ns) return ns.error();
        const auto& n = ns.value();
        const auto pow2 = [](std::uint64_t v) {
            return v != 0 && (v&(v-1U)) == 0;
        };
        const auto nsize = n.size_bytes();
        const auto cluster = n.cluster_bytes();
        const bool sane = pow2(n.bytes_per_sector) && n.bytes_per_sector >= 256 && n.bytes_per_sector <= 4096 &&
            pow2(n.sectors_per_cluster) && n.sectors_per_cluster <= 128 && n.total_sectors != 0 &&
            nsize != std::numeric_limits<std::uint64_t>::max()
            && nsize <= dev.geometry().size_bytes && cluster != 0;
        out.kind = FilesystemKind::ntfs;
        out.name = "NTFS";
        out.confident = sane;
        out.size_bytes = nsize;
        out.block_size = static_cast<std::uint32_t>(cluster);
        if (!sane)out.notes.push_back("NTFS boot geometry is inconsistent");
        return out;
    }

    {
        auto bp = btrfs::probe(dev);
        if (!bp)return bp.error();
        if (bp.value()) {
            out.kind = FilesystemKind::btrfs;
            out.name = "Btrfs";
            auto bs = btrfs::read_best_superblock(dev);
            if (bs) {
                out.confident = true;
                out.label = bs.value().label;
                out.uuid = btrfs::uuid_string(bs.value().fsid);
                out.size_bytes = bs.value().total_bytes;
                out.block_size = bs.value().sectorsize;
            }
            else {
                out.confident = false;
                out.notes.push_back(bs.error().message);
            }
            return out;
        }
    }

    {
        auto ip = iso9660::probe(dev);
        if (!ip)return ip.error();
        if (ip.value()) {
            out.kind = FilesystemKind::iso9660;
            out.name = "ISO9660";
            auto iv = iso9660::read_primary(dev);
            if (iv) {
                out.confident = true;
                out.label = iv.value().volume_id;
                out.size_bytes = static_cast<std::uint64_t>(iv.value().volume_blocks)*iv.value().logical_block_size;
                out.block_size = iv.value().logical_block_size;
            }
            else {
                out.confident = false;
                out.notes.push_back(iv.error().message);
            }
            return out;
        }
    }

    {
        auto up = ufs::probe(dev);
        if (!up) return up.error();
        if (up.value()) {
            auto us = ufs::read_superblock(dev);
            if (!us) return us.error();
            out.kind = us.value().version == ufs::Version::ufs2 ? FilesystemKind::ufs2 : FilesystemKind::ufs1;
            out.name = ufs::version_name(us.value().version);
            out.confident = true;
            out.block_size = us.value().block_size;
            if (us.value().version == ufs::Version::ufs1
                && us.value().old_size_fragments && us.value().fragment_size)
                out.size_bytes = static_cast<std::uint64_t>(us.value().old_size_fragments)*us.value().fragment_size;
            return out;
        }
    }

    return out;
}

} // namespace fsx
