#include "fsx/ntfs.hpp"
#include "fsx/endian.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <vector>

namespace fsx::ntfs {
namespace {
bool pow2(std::uint64_t x) {
    return x && (x&(x-1U)) == 0;
}
void err(CheckReport&r, std::string s) {
    r.clean = false;
    r.errors.push_back(std::move(s));
}

Result<std::array<std::byte, 512>> read_boot(BlockDevice& dev, std::uint64_t off) {
    if (off>dev.geometry().size_bytes || 512ULL>dev.geometry().size_bytes-off)return Error{
        Errc::corrupt, 0,"NTFS boot sector lies outside device"
    }
    ;
    std::array<std::byte, 512>b{};
    auto r = dev.read_exact(off, b);
    if (!r)return r.error();
    return b;
}

Result<std::uint64_t> mft_record_bytes(const BootSector&s) {
    const auto cb = s.cluster_bytes();
    if (cb == 0)return Error{Errc::corrupt, 0,"NTFS cluster size is zero"};
    if (s.clusters_per_mft_record>0) {
        const auto n = static_cast<std::uint64_t>(s.clusters_per_mft_record);
        if (n>std::numeric_limits<std::uint64_t>::max()/cb)return Error{
            Errc::corrupt, 0,"NTFS MFT record size overflows"
        }
        ;
        return n*cb;
    }
    if (s.clusters_per_mft_record<0) {
        const auto shift = static_cast<unsigned>(-static_cast<int>(s.clusters_per_mft_record));
        if (shift >= 63)return Error{
            Errc::corrupt, 0,"NTFS MFT record size shift is invalid"
        }
        ;
        return 1ULL<<shift;
    }
    return Error{Errc::corrupt, 0,"NTFS MFT record sizing field is zero"};
}

Result<std::vector<std::byte>> read_fixed_file_record(BlockDevice&dev,
                                                      std::uint64_t off, std::uint64_t bytes, std::uint16_t sector) {
    if (bytes<sector || !pow2(bytes) || bytes>64ULL*1024ULL)return Error{
        Errc::corrupt, 0,"NTFS MFT record size is unreasonable"
    }
    ;
    if (off>dev.geometry().size_bytes || bytes>dev.geometry().size_bytes-off)return Error{
        Errc::corrupt, 0,"NTFS MFT record lies outside device"
    }
    ;
    std::vector<std::byte> r(static_cast<std::size_t>(bytes));
    auto rr = dev.read_exact(off, r);
    if (!rr)return rr.error();
    if (std::to_integer<char>(r[0]) != 'F' || std::to_integer<char>(r[1]) != 'I'
        || std::to_integer<char>(r[2]) != 'L' || std::to_integer<char>(r[3]) != 'E')return Error{
        Errc::corrupt, 0,"NTFS MFT record magic is invalid"
    }
    ;
    const auto usa_off = read_le<std::uint16_t>(r, 4), usa_count = read_le<std::uint16_t>(r, 6);
    const auto sectors = bytes/sector;
    if (usa_count != sectors+1ULL)return Error{
        Errc::corrupt, 0,"NTFS MFT update-sequence count does not match record sectors"
    }
    ;
    const auto usa_bytes = static_cast<std::uint64_t>(usa_count)*2ULL;
    if (usa_off<8 || usa_off>bytes || usa_bytes>bytes-usa_off)return Error{
        Errc::corrupt, 0,"NTFS MFT update-sequence array lies outside record"
    }
    ;
    const auto usn = read_le<std::uint16_t>(r, usa_off);
    for (std::uint64_t i = 0; i<sectors; ++i) {
        const auto tail = (i+1ULL)*sector-2ULL;
        if (read_le<std::uint16_t>(r, static_cast<std::size_t>(tail)) != usn)return Error{
            Errc::corrupt, 0,"NTFS MFT update-sequence fixup mismatch"
        }
        ;
        const auto replacement = read_le<std::uint16_t>(r, static_cast<std::size_t>(usa_off+2ULL*(i+1ULL)));
        r[static_cast<std::size_t>(tail)] = std::byte(replacement&0xffU);
        r[static_cast<std::size_t>(tail+1)] = std::byte((replacement>>8U)&0xffU);
    }
    const auto first_attr = read_le<std::uint16_t>(r, 20);
    const auto used = read_le<std::uint32_t>(r, 24);
    const auto alloc = read_le<std::uint32_t>(r, 28);
    if (first_attr<24 || first_attr >= bytes)
        return Error{Errc::corrupt, 0,"NTFS MFT first-attribute offset is invalid"};
    if (used>bytes || alloc>bytes || used>alloc)
        return Error{Errc::corrupt, 0,"NTFS MFT record size fields are invalid"};
    return r;
}
}

Result<BootSector> read_boot_sector(BlockDevice& dev) {
    if (dev.geometry().size_bytes < 512) {
        return Error{Errc::unsupported, 0, "device is too small for NTFS"};
    }

    auto br = read_boot(dev, 0);
    if (!br) return br.error();
    const auto& b = br.value();
    const char sig[] = "NTFS    ";
    for (std::size_t i = 0; i < 8; ++i) {
        if (std::to_integer<unsigned char>(b[3U + i]) != static_cast<unsigned char>(sig[i])) {
            return Error{Errc::unsupported, 0, "not an NTFS filesystem"};
        }
    }

    BootSector out;
    out.bytes_per_sector = read_le<std::uint16_t>(b, 11);
    out.sectors_per_cluster = std::to_integer<std::uint8_t>(b[13]);
    out.total_sectors = read_le<std::uint64_t>(b, 40);
    out.mft_cluster = read_le<std::uint64_t>(b, 48);
    out.mft_mirror_cluster = read_le<std::uint64_t>(b, 56);
    out.clusters_per_mft_record = static_cast<std::int8_t>(std::to_integer<std::uint8_t>(b[64]));
    out.serial = read_le<std::uint64_t>(b, 72);

    if (std::to_integer<std::uint8_t>(b[510]) != 0x55
        || std::to_integer<std::uint8_t>(b[511]) != 0xaa) {
        return Error{Errc::corrupt, 0, "NTFS boot signature is missing"};
    }
    return out;
}

Result<CheckReport> check(BlockDevice&dev) {
    auto sr = read_boot_sector(dev);
    if (!sr)return sr.error();
    const auto&s = sr.value();
    CheckReport r;
    if (!pow2(s.bytes_per_sector) || s.bytes_per_sector < 256 || s.bytes_per_sector > 4096)
        err(r, "invalid NTFS bytes-per-sector");
    if (!pow2(s.sectors_per_cluster) || s.sectors_per_cluster == 0 || s.sectors_per_cluster > 128)
        err(r, "invalid NTFS sectors-per-cluster");
    if (s.total_sectors == 0)
        err(r, "NTFS total-sector count is zero");
    else if (s.bytes_per_sector && s.total_sectors > dev.geometry().size_bytes / s.bytes_per_sector)
        err(r, "NTFS volume geometry exceeds containing device");

    const auto cb = s.cluster_bytes();
    const auto clusters = cb ? s.size_bytes() / cb : 0;
    if (clusters && s.mft_cluster >= clusters)
        err(r, "NTFS MFT cluster lies outside the volume");
    if (clusters && s.mft_mirror_cluster >= clusters)
        err(r, "NTFS MFT mirror cluster lies outside the volume");
    if (!r.clean)
        return r;
    auto mrb = mft_record_bytes(s);
    if (!mrb) {
        err(r, mrb.error().message);
        return r;
    }
    const auto record_bytes = mrb.value();
    if (record_bytes<s.bytes_per_sector || record_bytes>64ULL*1024ULL)err(r,
                                                                          "NTFS MFT record size is outside the "
                                                                          "supported sane range");
    if (s.total_sectors>0) {
        const auto backup_off = (s.total_sectors-1ULL)*s.bytes_per_sector;
        auto p = read_boot(dev, 0);
        if (!p)return p.error();
        auto b = read_boot(dev, backup_off);
        if (!b) {
            err(r,"NTFS backup boot sector cannot be read");
        }
        else {
            if (std::to_integer<std::uint8_t>(b.value()[510]) != 0x55
                || std::to_integer<std::uint8_t>(b.value()[511]) != 0xaa)err(r,"NTFS backup boot signature is invalid");
            const auto ps = std::span<const std::byte>(p.value()).subspan(3, 81);
            const auto bs = std::span<const std::byte>(b.value()).subspan(3, 81);
            if (!std::equal(ps.begin(), ps.end(), bs.begin()))err(r,"NTFS primary and backup boot geometry disagree");
        }
    }
    if (r.clean) {
        const auto mft_off = s.mft_cluster*cb;
        const auto mirr_off = s.mft_mirror_cluster*cb;
        auto mft = read_fixed_file_record(dev, mft_off, record_bytes, s.bytes_per_sector);
        if (!mft)err(r,"primary MFT record 0: "+mft.error().message);
        auto mirr = read_fixed_file_record(dev, mirr_off, record_bytes, s.bytes_per_sector);
        if (!mirr)err(r,"MFTMirr record 0: "+mirr.error().message);
        if (mft && mirr && mft.value() != mirr.value()) {
            r.warnings.push_back("NTFS MFT record 0 and MFTMirr record 0 differ after fixup");
        }
    }
    r.clean = r.errors.empty();
    return r;
}
} // namespace fsx::ntfs
