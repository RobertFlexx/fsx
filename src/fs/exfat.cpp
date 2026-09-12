#include "fsx/exfat.hpp"
#include "fsx/endian.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

namespace fsx::exfat {
namespace {

constexpr std::array<char, 8> kName{{'E','X','F','A','T',' ',' ',' '}};

bool is_exfat_name(std::span<const std::byte> b) {
    if (b.size() < 11) return false;
    for (std::size_t i = 0; i < kName.size(); ++i)
        if (std::to_integer<unsigned char>(b[3 + i]) != static_cast<unsigned char>(kName[i])) return false;
    return true;
}

std::uint32_t boot_checksum(std::span<const std::byte> data, std::uint32_t sector_bytes) {
    std::uint32_t sum = 0;
    const std::size_t limit = static_cast<std::size_t>(sector_bytes) * 11U;
    for (std::size_t i = 0; i < limit; ++i) {
        if (i == 106U || i == 107U || i == 112U) continue;
        sum = ((sum << 31U) | (sum >> 1U)) + std::to_integer<std::uint8_t>(data[i]);
    }
    return sum;
}

bool checksum_sector_matches(std::span<const std::byte> sector, std::uint32_t expected) {
    if ((sector.size() % 4U) != 0) return false;
    for (std::size_t i = 0; i < sector.size(); i += 4U)
        if (read_le<std::uint32_t>(sector, i) != expected) return false;
    return true;
}

std::string utf16_ascii_label(std::span<const std::byte> entry) {
    if (entry.size() < 32) return {};
    const auto count = std::min<std::size_t>(15, std::to_integer<std::uint8_t>(entry[1]));
    std::string out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto ch = read_le<std::uint16_t>(entry, 2U + i * 2U);
        if (ch == 0) break;
        if (ch >= 0x20U && ch <= 0x7eU) out.push_back(static_cast<char>(ch));
        else out.push_back('?');
    }
    return out;
}

Result<std::vector<std::byte>> read_boot_region(BlockDevice& dev,
                                                std::uint32_t sector_bytes, std::uint64_t first_sector) {
    const std::uint64_t len64 = static_cast<std::uint64_t>(sector_bytes) * 12ULL;
    if (len64 > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
        return Error{Errc::unsupported, 0, "exFAT boot region is too large for this build"};
    std::vector<std::byte> data(static_cast<std::size_t>(len64));
    auto r = dev.read_exact(first_sector * sector_bytes, data);
    if (!r) return r.error();
    return data;
}


Result<std::uint32_t> read_fat_entry(BlockDevice& dev, const Volume& v, std::uint32_t cluster) {
    if (cluster >= v.cluster_count + 2U) return Error{
        Errc::corrupt, 0,"exFAT cluster index exceeds FAT range"
    }
    ;
    const std::uint64_t fat_base = static_cast<std::uint64_t>(v.fat_offset)*v.bytes_per_sector;
    const std::uint64_t off = fat_base+static_cast<std::uint64_t>(cluster)*4ULL;
    std::array<std::byte, 4> b{};
    auto r = dev.read_exact(off, b);
    if (!r)return r.error();
    return read_le<std::uint32_t>(b, 0);
}

std::uint32_t upcase_checksum(std::span<const std::byte> data) {
    std::uint32_t sum = 0;
    for (auto b:data) sum = ((sum<<31U)|(sum>>1U))+std::to_integer<std::uint8_t>(b);
    return sum;
}

Result<std::vector<std::byte>> read_chain(BlockDevice& dev, const Volume& v,
                                          std::uint32_t first, std::uint64_t length, std::uint64_t limit) {
    if (length>limit)return Error{Errc::unsupported, 0,"exFAT system stream exceeds validation safety limit"};
    const std::uint64_t cb = static_cast<std::uint64_t>(v.bytes_per_sector)*v.sectors_per_cluster;
    if (cb == 0 || length>static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))return Error{
        Errc::unsupported, 0,"exFAT stream is too large for address space"
    }
    ;
    std::vector<std::byte> out(static_cast<std::size_t>(length));
    std::uint64_t copied = 0;
    std::uint32_t c = first;
    std::vector<std::uint32_t> seen;
    while (copied<length) {
        if (c<2 || c >= v.cluster_count+2U)return Error{
            Errc::corrupt, 0,"exFAT system stream cluster lies outside heap"
        }
        ;
        if (std::find(seen.begin(), seen.end(), c) != seen.end())return Error{
            Errc::corrupt, 0,"exFAT system stream FAT chain contains a loop"
        }
        ;
        seen.push_back(c);
        const std::uint64_t sector = static_cast<std::uint64_t>(v.cluster_heap_offset)
            + static_cast<std::uint64_t>(c - 2U) * v.sectors_per_cluster;
        const auto n64 = std::min<std::uint64_t>(cb, length-copied);
        auto span = std::span<std::byte>(out).subspan(static_cast<std::size_t>(copied), static_cast<std::size_t>(n64));
        auto rr = dev.read_exact(sector*v.bytes_per_sector, span);
        if (!rr)return rr.error();
        copied+=n64;
        if (copied >= length)break;
        auto nr = read_fat_entry(dev, v, c);
        if (!nr)return nr.error();
        const auto next = nr.value();
        if (next >= 0xfffffff8U)return Error{
            Errc::corrupt, 0,"exFAT system stream FAT chain ended before advertised length"
        }
        ;
        if (next == 0xfffffff7U || next == 0)return Error{
            Errc::corrupt, 0,"exFAT system stream FAT chain contains invalid cluster"
        }
        ;
        c = next;
    }
    return out;
}

Result<Volume> parse(BlockDevice& dev) {
    if (dev.geometry().size_bytes < 512)
        return Error{Errc::unsupported, 0, "device is too small for exFAT"};
    std::array<std::byte, 512> first{};
    auto rr = dev.read_exact(0, first);
    if (!rr) return rr.error();
    const auto f = std::span<const std::byte>(first);
    if (!is_exfat_name(f)) return Error{Errc::unsupported, 0, "not an exFAT volume"};
    if (std::to_integer<std::uint8_t>(f[510]) != 0x55U || std::to_integer<std::uint8_t>(f[511]) != 0xaaU)
        return Error{Errc::corrupt, 0, "exFAT boot signature is invalid"};

    const auto bps_shift = std::to_integer<std::uint8_t>(f[108]);
    const auto spc_shift = std::to_integer<std::uint8_t>(f[109]);
    if (bps_shift < 9 || bps_shift > 12 || spc_shift > 25
        || static_cast<unsigned>(bps_shift) + spc_shift > 25U)
        return Error{Errc::corrupt, 0, "exFAT sector/cluster shift is invalid"};

    Volume v;
    v.bytes_per_sector = 1U << bps_shift;
    v.sectors_per_cluster = 1U << spc_shift;
    v.partition_offset = read_le<std::uint64_t>(f, 64);
    v.volume_length = read_le<std::uint64_t>(f, 72);
    v.fat_offset = read_le<std::uint32_t>(f, 80);
    v.fat_length = read_le<std::uint32_t>(f, 84);
    v.cluster_heap_offset = read_le<std::uint32_t>(f, 88);
    v.cluster_count = read_le<std::uint32_t>(f, 92);
    v.root_dir_cluster = read_le<std::uint32_t>(f, 96);
    v.serial = read_le<std::uint32_t>(f, 100);
    v.revision = read_le<std::uint16_t>(f, 104);
    v.flags = read_le<std::uint16_t>(f, 106);
    v.fat_count = std::to_integer<std::uint8_t>(f[110]);
    v.percent_in_use = std::to_integer<std::uint8_t>(f[112]);

    // exFAT reserves the top cluster values for bad/EOC markers; keeping the
    // advertised heap below them also makes all cluster_count+2 arithmetic
    // well-defined on hostile images.
    if (v.volume_length == 0 || v.fat_offset < 24 || v.fat_length == 0 ||
        v.cluster_heap_offset <= v.fat_offset || v.cluster_count == 0 ||
        v.cluster_count > 0xfffffff5U ||
        v.root_dir_cluster < 2 || v.root_dir_cluster >= v.cluster_count + 2U ||
        v.fat_count == 0 || v.fat_count > 2)
        return Error{Errc::corrupt, 0, "exFAT geometry is invalid"};
    if (v.percent_in_use > 100U && v.percent_in_use != 0xffU)
        return Error{Errc::corrupt, 0, "exFAT percent-in-use field is invalid"};
    if (v.size_bytes() > dev.geometry().size_bytes)
        return Error{Errc::corrupt, 0, "exFAT volume is larger than containing device"};

    const std::uint64_t fat_end = static_cast<std::uint64_t>(v.fat_offset)
        + static_cast<std::uint64_t>(v.fat_length) * v.fat_count;
    if (fat_end>v.cluster_heap_offset)return Error{Errc::corrupt, 0,"exFAT FAT region overlaps cluster heap"};
    const std::uint64_t fat_capacity = (static_cast<std::uint64_t>(v.fat_length)*v.bytes_per_sector)/4ULL;
    if (fat_capacity<static_cast<std::uint64_t>(v.cluster_count)+2ULL)return Error{
        Errc::corrupt, 0,"exFAT FAT is too small for advertised cluster count"
    }
    ;
    const std::uint64_t heap_end = static_cast<std::uint64_t>(v.cluster_heap_offset) +
        static_cast<std::uint64_t>(v.cluster_count) * v.sectors_per_cluster;
    if (heap_end > v.volume_length)
        return Error{Errc::corrupt, 0, "exFAT cluster heap exceeds volume length"};

    auto main = read_boot_region(dev, v.bytes_per_sector, 0);
    if (!main) return main.error();
    auto backup = read_boot_region(dev, v.bytes_per_sector, 12);
    if (!backup) return backup.error();
    const auto main_data = std::span<const std::byte>(main.value());
    const auto backup_data = std::span<const std::byte>(backup.value());
    const auto main_sum = boot_checksum(main_data, v.bytes_per_sector);
    const auto backup_sum = boot_checksum(backup_data, v.bytes_per_sector);
    const auto main_checksum_sector = main_data.subspan(
        static_cast<std::size_t>(v.bytes_per_sector) * 11U, v.bytes_per_sector);
    const auto backup_checksum_sector = backup_data.subspan(
        static_cast<std::size_t>(v.bytes_per_sector) * 11U, v.bytes_per_sector);
    v.main_boot_checksum_ok = checksum_sector_matches(main_checksum_sector, main_sum);
    v.backup_boot_checksum_ok = checksum_sector_matches(backup_checksum_sector, backup_sum);

    // Compare immutable bytes in sectors 0..10. VolumeFlags and PercentInUse are
    // explicitly excluded because they are mutable and excluded from checksum.
    v.boot_regions_match = true;
    const std::size_t compare_len = static_cast<std::size_t>(v.bytes_per_sector) * 11U;
    for (std::size_t i = 0; i < compare_len; ++i) {
        if (i == 106U || i == 107U || i == 112U) continue;
        if (main_data[i] != backup_data[i]) {
            v.boot_regions_match = false;
            break;
        }
    }

    const std::uint64_t cluster_bytes = static_cast<std::uint64_t>(v.bytes_per_sector) * v.sectors_per_cluster;
    if (cluster_bytes <= 16ULL * 1024ULL * 1024ULL) {
        const std::uint64_t root_sector = static_cast<std::uint64_t>(v.cluster_heap_offset) +
            static_cast<std::uint64_t>(v.root_dir_cluster - 2U) * v.sectors_per_cluster;
        std::vector<std::byte> root(static_cast<std::size_t>(cluster_bytes));
        auto r = dev.read_exact(root_sector * v.bytes_per_sector, root);
        if (r) {
            for (std::size_t off = 0; off + 32U <= root.size(); off += 32U) {
                const auto type = std::to_integer<std::uint8_t>(root[off]);
                if (type == 0x00U) break;
                if (type == 0x83U) v.label = utf16_ascii_label(std::span<const std::byte>(root).subspan(off, 32));
                else if (type == 0x81U) {
                    v.allocation_bitmap_seen = true;
                    v.allocation_bitmap_cluster = read_le<std::uint32_t>(root, off+20U);
                    v.allocation_bitmap_length = read_le<std::uint64_t>(root, off+24U);
                }
                else if (type == 0x82U) {
                    v.upcase_table_seen = true;
                    v.upcase_checksum = read_le<std::uint32_t>(root, off+4U);
                    v.upcase_cluster = read_le<std::uint32_t>(root, off+20U);
                    v.upcase_length = read_le<std::uint64_t>(root, off+24U);
                }
            }
        }
    }
    auto root_fat = read_fat_entry(dev, v, v.root_dir_cluster);
    if (root_fat)v.root_fat_entry = root_fat.value();
    return v;
}

} // namespace

Result<bool> probe(BlockDevice& dev) {
    if (dev.geometry().size_bytes < 11) return false;
    std::array<std::byte, 11> b{};
    auto r = dev.read_exact(0, b);
    if (!r) return r.error();
    return is_exfat_name(b);
}

Result<Volume> read_volume(BlockDevice& dev) {
    return parse(dev);
}

Result<CheckReport> check(BlockDevice& dev) {
    auto vr = parse(dev);
    if (!vr)return vr.error();
    const auto&v = vr.value();
    CheckReport r;
    if (!v.main_boot_checksum_ok)r.errors.push_back("main exFAT boot region checksum is invalid");
    if (!v.backup_boot_checksum_ok)r.errors.push_back("backup exFAT boot region checksum is invalid");
    if (!v.boot_regions_match)r.errors.push_back("main and backup exFAT boot regions disagree");
    if (v.revision<0x0100U)r.warnings.push_back("unusual exFAT filesystem revision");
    if (v.root_fat_entry == 0
        || v.root_fat_entry == 0xfffffff7U)r.errors.push_back("exFAT root directory has an invalid FAT entry");
    if (!v.allocation_bitmap_seen) {
        r.warnings.push_back(
            "exFAT allocation bitmap entry was not found in the first root-directory cluster");
    }
    else {
        const auto need = (static_cast<std::uint64_t>(v.cluster_count)+7ULL)/8ULL;
        if (v.allocation_bitmap_cluster < 2
            || v.allocation_bitmap_cluster >= v.cluster_count + 2U) {
            r.errors.push_back("exFAT allocation bitmap cluster lies outside heap");
        }
        if (v.allocation_bitmap_length < need) {
            r.errors.push_back("exFAT allocation bitmap is too short for cluster count");
        }
    }
    if (!v.upcase_table_seen) {
        r.warnings.push_back(
            "exFAT upcase table entry was not found in the first root-directory cluster");
    }
    else {
        if (v.upcase_cluster < 2 || v.upcase_cluster >= v.cluster_count + 2U) {
            r.errors.push_back("exFAT upcase table cluster lies outside heap");
        }
        if (v.upcase_length == 0)r.errors.push_back("exFAT upcase table length is zero");
        else if (v.upcase_length <= 16ULL*1024ULL*1024ULL) {
            auto up = read_chain(dev, v, v.upcase_cluster, v.upcase_length, 16ULL*1024ULL*1024ULL);
            if (!up)r.errors.push_back("exFAT upcase table: "+up.error().message);
            else if (upcase_checksum(up.value()) != v.upcase_checksum) {
                r.errors.push_back("exFAT upcase table checksum is invalid");
            }
        }
        else r.warnings.push_back("exFAT upcase table exceeds checksum-validation safety limit");
    }
    r.clean = r.errors.empty();
    return r;
}

} // namespace fsx::exfat
