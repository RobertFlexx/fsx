#include "fsx/fat.hpp"
#include "fsx/cancel.hpp"
#include "fsx/endian.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <vector>

namespace fsx::fat {
namespace {

bool power2(std::uint32_t v) {
    return v != 0 && (v & (v - 1U)) == 0;
}

std::string trim_field(std::span<const std::byte> s) {
    std::string out;
    out.reserve(s.size());
    for (auto b : s) {
        const auto c = static_cast<char>(std::to_integer<unsigned char>(b));
        if (c == '\0') break;
        out.push_back(c);
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\0')) out.pop_back();
    return out;
}

Result<std::array<std::byte, 512>> boot_sector(BlockDevice& dev) {
    if (dev.geometry().size_bytes < 512)
        return Error{Errc::unsupported, 0, "device is too small for a FAT boot sector"};
    std::array<std::byte, 512> b{};
    auto r = dev.read_exact(0, b);
    if (!r) return r.error();
    return b;
}

Result<Volume> parse_volume(BlockDevice& dev, const std::array<std::byte, 512>& raw) {
    const auto b = std::span<const std::byte>(raw);
    if (std::to_integer<unsigned char>(b[510]) != 0x55U ||
        std::to_integer<unsigned char>(b[511]) != 0xaaU)
        return Error{Errc::unsupported, 0, "FAT boot signature is absent"};

    Volume v;
    v.bytes_per_sector = read_le<std::uint16_t>(b, 11);
    v.sectors_per_cluster = std::to_integer<std::uint8_t>(b[13]);
    v.reserved_sectors = read_le<std::uint16_t>(b, 14);
    v.fat_count = std::to_integer<std::uint8_t>(b[16]);
    const auto root_entries = read_le<std::uint16_t>(b, 17);
    const auto total16 = read_le<std::uint16_t>(b, 19);
    v.media = std::to_integer<std::uint8_t>(b[21]);
    const auto fat16 = read_le<std::uint16_t>(b, 22);
    const auto total32 = read_le<std::uint32_t>(b, 32);
    const auto fat32 = read_le<std::uint32_t>(b, 36);

    if (!power2(v.bytes_per_sector) || v.bytes_per_sector < 512 || v.bytes_per_sector > 4096)
        return Error{Errc::corrupt, 0, "invalid FAT bytes-per-sector"};
    if (!power2(v.sectors_per_cluster) || v.sectors_per_cluster > 128)
        return Error{Errc::corrupt, 0, "invalid FAT sectors-per-cluster"};
    if (v.reserved_sectors == 0 || v.fat_count == 0 || v.fat_count > 4)
        return Error{Errc::corrupt, 0, "invalid FAT reserved/FAT count geometry"};

    v.total_sectors = total16 != 0 ? total16 : total32;
    v.sectors_per_fat = fat16 != 0 ? fat16 : fat32;
    if (v.total_sectors == 0 || v.sectors_per_fat == 0)
        return Error{Errc::corrupt, 0, "invalid FAT size fields"};

    v.root_dir_sectors = ((static_cast<std::uint32_t>(root_entries) * 32U) +
                          (v.bytes_per_sector - 1U)) / v.bytes_per_sector;
    const std::uint64_t first_data = static_cast<std::uint64_t>(v.reserved_sectors) +
        static_cast<std::uint64_t>(v.fat_count) * v.sectors_per_fat + v.root_dir_sectors;
    if (first_data >= v.total_sectors)
        return Error{Errc::corrupt, 0, "FAT metadata consumes the whole volume"};
    v.first_data_sector = static_cast<std::uint32_t>(first_data);
    v.data_sectors = v.total_sectors - v.first_data_sector;
    v.cluster_count = v.data_sectors / v.sectors_per_cluster;

    if (v.cluster_count < 4085U) v.kind = Kind::fat12;
    else if (v.cluster_count < 65525U) v.kind = Kind::fat16;
    else v.kind = Kind::fat32;
    // FAT32 reserves 0x0ffffff6 and above for special markers. Refuse
    // impossible cluster counts up front so max-cluster arithmetic cannot
    // wrap and allocation references remain representable.
    if (v.kind == Kind::fat32 && v.cluster_count > 0x0ffffff5U)
        return Error{Errc::corrupt, 0, "FAT32 cluster count exceeds representable data clusters"};

    if (v.kind == Kind::fat32) {
        if (root_entries != 0 || fat16 != 0)
            return Error{Errc::corrupt, 0, "FAT32 has incompatible FAT12/16 geometry fields"};
        v.root_cluster = read_le<std::uint32_t>(b, 44);
        if (v.root_cluster < 2 || v.root_cluster >= v.cluster_count + 2U)
            return Error{Errc::corrupt, 0, "invalid FAT32 root cluster"};
        v.serial = read_le<std::uint32_t>(b, 67);
        v.label = trim_field(b.subspan(71, 11));
    } else {
        if (root_entries == 0)
            return Error{Errc::corrupt, 0, "FAT12/16 root directory entry count is zero"};
        v.serial = read_le<std::uint32_t>(b, 39);
        v.label = trim_field(b.subspan(43, 11));
    }

    if (v.size_bytes() > dev.geometry().size_bytes)
        return Error{Errc::corrupt, 0, "FAT volume is larger than containing device"};

    const std::uint64_t fat_entries_capacity =
        (static_cast<std::uint64_t>(v.sectors_per_fat) * v.bytes_per_sector * 8ULL) /
        (v.kind == Kind::fat12 ? 12ULL : (v.kind == Kind::fat16 ? 16ULL : 32ULL));
    if (fat_entries_capacity < static_cast<std::uint64_t>(v.cluster_count) + 2ULL)
        return Error{Errc::corrupt, 0, "FAT table is too small for cluster count"};
    return v;
}

class FatStream {
public:
    FatStream(BlockDevice& dev, std::uint64_t base, std::uint64_t bytes, Kind kind)
        : dev_(dev), base_(base), bytes_(bytes), kind_(kind), buffer_(4U*1024U*1024U + 8U) {
        }

    Result<std::uint32_t> get(std::uint32_t cluster) {
        const std::uint64_t off = kind_ == Kind::fat12 ? static_cast<std::uint64_t>(cluster) + cluster/2ULL :
            kind_ == Kind::fat16 ? static_cast<std::uint64_t>(cluster)*2ULL :
            static_cast<std::uint64_t>(cluster)*4ULL;
        const std::size_t need = kind_ == Kind::fat32 ? 4U : 2U;
        if (off > bytes_ || need > bytes_ - off)
            return Error{Errc::corrupt, 0,"FAT entry table ended before advertised cluster count"};
        if (!(off >= window_off_ && off + need <= window_off_ + window_valid_)) {
            constexpr std::uint64_t chunk = 4ULL*1024ULL*1024ULL;
            window_off_ = (off/chunk)*chunk;
            const auto n64 = std::min<std::uint64_t>(buffer_.size(), bytes_-window_off_);
            window_valid_ = static_cast<std::size_t>(n64);
            auto r = dev_.read_exact(base_+window_off_, std::span<std::byte>(buffer_).first(window_valid_));
            if (!r)return r.error();
        }
        const auto i = static_cast<std::size_t>(off-window_off_);
        if (kind_ == Kind::fat12) {
            const auto a = std::to_integer<std::uint8_t>(buffer_[i]);
            const auto b = std::to_integer<std::uint8_t>(buffer_[i+1]);
            const auto pair = static_cast<std::uint16_t>(a|(static_cast<std::uint16_t>(b)<<8U));
            return static_cast<std::uint32_t>((cluster&1U)?(pair>>4U):(pair&0x0fffU));
        }
        if (kind_ == Kind::fat16) {
            return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(buffer_[i]))
                | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(buffer_[i + 1])) << 8U);
        }
        return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(buffer_[i]))
                | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(buffer_[i + 1])) << 8U)
                | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(buffer_[i + 2])) << 16U)
                | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(buffer_[i + 3])) << 24U))
            & 0x0fffffffU;
    }
private:
    BlockDevice& dev_;
    std::uint64_t base_{};
    std::uint64_t bytes_{};
    Kind kind_{};
    std::vector<std::byte> buffer_;
    std::uint64_t window_off_{
        std::numeric_limits<std::uint64_t>::max()
    }
    ;
    std::size_t window_valid_{};
};

} // namespace

const char* kind_name(Kind k) noexcept {
    switch (k) {
    case Kind::fat12: return "FAT12";
    case Kind::fat16: return "FAT16";
    case Kind::fat32: return "FAT32";
    }
    return "FAT";
}

Result<bool> probe(BlockDevice& dev) {
    auto b = boot_sector(dev);
    if (!b) {
        if (b.error().code == Errc::unsupported) return false;
        return b.error();
    }
    auto v = parse_volume(dev, b.value());
    if (!v) {
        if (v.error().code == Errc::unsupported || v.error().code == Errc::corrupt) return false;
        return v.error();
    }
    return true;
}

Result<Volume> read_volume(BlockDevice& dev) {
    auto b = boot_sector(dev);
    if (!b) return b.error();
    return parse_volume(dev, b.value());
}

Result<CheckReport> check(BlockDevice& dev, Progress* progress) {
    auto vr = read_volume(dev);
    if (!vr) return vr.error();
    const auto v = vr.value();
    CheckReport out;
    const std::uint64_t fat_bytes = static_cast<std::uint64_t>(v.sectors_per_fat) * v.bytes_per_sector;
    const std::uint64_t fat0 = static_cast<std::uint64_t>(v.reserved_sectors) * v.bytes_per_sector;

    // Stream the allocation table through a bounded 4 MiB window.  Older
    // versions skipped reference validation for very large FATs; this path
    // validates the full advertised cluster range without scaling RAM use.
    FatStream stream(dev, fat0, fat_bytes, v.kind);
    const auto max_cluster = v.cluster_count+1U;
    for (std::uint32_t c = 2; c <= max_cluster; ++c) {
        if (Cancellation::requested())return Error{Errc::unsafe, 0,"FAT check cancelled"};
        auto er = stream.get(c);
        if (!er) {
            out.errors.push_back(er.error().message);
            break;
        }
        const auto e = er.value();
        const std::uint32_t mask = v.kind == Kind::fat12?0x0fffU:(v.kind == Kind::fat16?0xffffU:0x0fffffffU);
        const std::uint32_t bad = v.kind == Kind::fat12?0x0ff7U:(v.kind == Kind::fat16?0xfff7U:0x0ffffff7U);
        const std::uint32_t eoc = v.kind == Kind::fat12?0x0ff8U:(v.kind == Kind::fat16?0xfff8U:0x0ffffff8U);
        const auto val = e&mask;
        if (val == 0 || val == bad || val >= eoc)continue;
        if (val<2 || val>max_cluster)++out.invalid_cluster_entries;
        if (progress && (c%65536U) == 0U)progress->update({
                                                          "checking FAT","validating allocation references", c,
                                                          max_cluster, static_cast<std::uint64_t>(c)*2ULL, 0,
                                                          c, max_cluster
                                                          }
                                                         );
    }

    if (v.fat_count > 1) {
        constexpr std::size_t chunk = 4U * 1024U * 1024U;
        std::vector<std::byte> a(chunk), b(chunk);
        for (std::uint8_t copy = 1; copy < v.fat_count; ++copy) {
            std::uint64_t done = 0;
            while (done < fat_bytes) {
                if (Cancellation::requested()) return Error{Errc::unsafe, 0, "FAT check cancelled"};
                const auto n64 = std::min<std::uint64_t>(chunk, fat_bytes - done);
                const auto n = static_cast<std::size_t>(n64);
                auto ra = dev.read_exact(fat0 + done, std::span<std::byte>(a).first(n));
                if (!ra) return ra.error();
                auto rb = dev.read_exact(fat0 + static_cast<std::uint64_t>(copy) * fat_bytes + done,
                                         std::span<std::byte>(b).first(n));
                if (!rb) return rb.error();
                if (!std::equal(a.begin(), a.begin() + static_cast<std::ptrdiff_t>(n), b.begin()))
                    ++out.mirrored_fat_mismatches;
                out.fat_bytes_compared += n64;
                done += n64;
                if (progress) progress->update({
                                               "checking FAT", "comparing mirrored allocation tables", done, fat_bytes,
                                               done * 2, 0, copy, static_cast<std::uint64_t>(v.fat_count - 1U)
                                               }
                                              );
            }
        }
    }

    if (out.invalid_cluster_entries != 0)
        out.errors.push_back(
            std::to_string(out.invalid_cluster_entries)
            + " FAT entries point outside the data cluster range");
    if (out.mirrored_fat_mismatches != 0)
        out.errors.push_back(std::to_string(out.mirrored_fat_mismatches) + " FAT mirror chunks differ");
    out.clean = out.errors.empty();
    return out;
}

} // namespace fsx::fat
