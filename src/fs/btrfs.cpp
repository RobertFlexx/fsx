#include "fsx/btrfs.hpp"
#include "fsx/crc.hpp"
#include "fsx/endian.hpp"
#include "fsx/hash.hpp"
#include <algorithm>
#include <array>
#include <iomanip>
#include <limits>
#include <sstream>

namespace fsx::btrfs {
namespace {
constexpr std::uint64_t MAGIC = 0x4D5F53665248425FULL;
constexpr std::array<std::uint64_t, 3> MIRRORS{
    {
        64ULL*1024ULL, 64ULL*1024ULL*1024ULL, 256ULL*1024ULL*1024ULL*1024ULL
    }
}
;
constexpr std::size_t SB_SIZE = 4096;

bool power2(std::uint64_t x) noexcept { return x && ((x&(x-1U)) == 0);
}
std::string label_from(std::span<const std::byte> b) {
    std::string out;
    for (std::size_t i = 299; i<555 && i<b.size(); ++i) {
        const auto c = std::to_integer<unsigned char>(b[i]);
        if (c == 0)break;
        if (c >= 0x20U && c <= 0x7eU)out.push_back(static_cast<char>(c));
        else out.push_back('?');
    }
    return out;
}
std::size_t checksum_bytes(ChecksumType t) noexcept {
    switch (t) {
    case ChecksumType::crc32c:return 4;
    case ChecksumType::xxhash64:return 8;
    case ChecksumType::sha256:return 32;
    case ChecksumType::blake2b:return 32;
    }
    return 0;
}
bool checksum_supported(std::uint16_t raw) noexcept { return raw <= 3;
}
std::array<std::byte, 32> calculated_checksum(std::span<const std::byte> sb, ChecksumType t) {
    std::array<std::byte, 32> out{};
    const auto body = sb.subspan(32);
    if (t == ChecksumType::crc32c) {
        const auto v = crc32c(body)^0xffffffffU;
        for (unsigned i = 0; i<4; ++i)out[i] = std::byte((v>>(8U*i))&0xffU);
    }
    else if (t == ChecksumType::xxhash64) {
        const auto v = xxhash64(body);
        for (unsigned i = 0; i<8; ++i)out[i] = std::byte((v>>(8U*i))&0xffU);
    }
    else if (t == ChecksumType::sha256)out = sha256(body);
    else if (t == ChecksumType::blake2b)out = blake2b_256(body);
    return out;
}
Result<Superblock> parse(BlockDevice&dev, std::uint64_t off) {
    if (off>dev.geometry().size_bytes || SB_SIZE>dev.geometry().size_bytes-off)return Error{
        Errc::not_found, 0,"Btrfs superblock mirror lies outside device"
    }
    ;
    std::array<std::byte, SB_SIZE>b{};
    auto rr = dev.read_exact(off, b);
    if (!rr)return rr.error();
    if (read_le<std::uint64_t>(b, 64) != MAGIC)return Error{Errc::unsupported, 0,"Btrfs magic is absent"};
    Superblock s;
    s.mirror_offset = off;
    std::copy_n(b.begin()+32, 16, s.fsid.begin());
    s.bytenr = read_le<std::uint64_t>(b, 48);
    s.flags = read_le<std::uint64_t>(b, 56);
    s.generation = read_le<std::uint64_t>(b, 72);
    s.root = read_le<std::uint64_t>(b, 80);
    s.chunk_root = read_le<std::uint64_t>(b, 88);
    s.log_root = read_le<std::uint64_t>(b, 96);
    s.total_bytes = read_le<std::uint64_t>(b, 112);
    s.bytes_used = read_le<std::uint64_t>(b, 120);
    s.num_devices = read_le<std::uint64_t>(b, 136);
    s.sectorsize = read_le<std::uint32_t>(b, 144);
    s.nodesize = read_le<std::uint32_t>(b, 148);
    s.stripesize = read_le<std::uint32_t>(b, 156);
    s.sys_chunk_array_size = read_le<std::uint32_t>(b, 160);
    s.compat_flags = read_le<std::uint64_t>(b, 172);
    s.compat_ro_flags = read_le<std::uint64_t>(b, 180);
    s.incompat_flags = read_le<std::uint64_t>(b, 188);
    const auto ct = read_le<std::uint16_t>(b, 196);
    s.checksum_type = static_cast<ChecksumType>(ct);
    s.root_level = std::to_integer<std::uint8_t>(b[198]);
    s.chunk_root_level = std::to_integer<std::uint8_t>(b[199]);
    s.log_root_level = std::to_integer<std::uint8_t>(b[200]);
    // struct btrfs_dev_item is embedded immediately after the level bytes.
    s.device_id = read_le<std::uint64_t>(b, 201);
    s.device_total_bytes = read_le<std::uint64_t>(b, 209);
    s.device_bytes_used = read_le<std::uint64_t>(b, 217);
    s.label = label_from(b);
    std::copy_n(b.begin()+571, 16, s.metadata_uuid.begin());
    s.checksum_supported = checksum_supported(ct);
    if (s.checksum_supported) {
        const auto calc = calculated_checksum(b, s.checksum_type);
        const auto n = checksum_bytes(s.checksum_type);
        s.checksum_valid = std::equal(calc.begin(), calc.begin()+static_cast<std::ptrdiff_t>(n), b.begin());
    }
    return s;
}
void fail(CheckReport&r, std::string s) {
    r.clean = false;
    r.errors.push_back(std::move(s));
}
bool same_uuid(const std::array<std::byte, 16>&a, const std::array<std::byte, 16>&b) {
    return a == b;
}
bool sane_geometry(const BlockDevice& dev, const Superblock& s) noexcept {
    if (s.bytenr != s.mirror_offset || !power2(s.sectorsize)
        || s.sectorsize<4096 || s.sectorsize>65536) return false;
    if (!power2(s.nodesize) || s.nodesize<s.sectorsize || s.nodesize>65536) return false;
    if (s.stripesize == 0 || !power2(s.stripesize) || s.total_bytes == 0
        || s.bytes_used>s.total_bytes || s.num_devices == 0) return false;
    if (s.device_id == 0 || s.device_total_bytes == 0 || s.device_total_bytes>dev.geometry().size_bytes
        || s.device_bytes_used>s.device_total_bytes) return false;
    if (s.num_devices == 1 && s.total_bytes>dev.geometry().size_bytes) return false;
    if (s.root_level>8 || s.chunk_root_level>8
        || s.log_root_level>8 || s.sys_chunk_array_size>2048) return false;
    return true;
}
}

const char* checksum_name(ChecksumType t) noexcept {
    switch (t) {
    case ChecksumType::crc32c:return "crc32c";
    case ChecksumType::xxhash64:return "xxhash64";
    case ChecksumType::sha256:return "sha256";
    case ChecksumType::blake2b:return "blake2b-256";
    }
    return "unknown";
}
std::string uuid_string(const std::array<std::byte, 16>&u) {
    std::ostringstream o;
    o<<std::hex<<std::setfill('0');
    for (std::size_t i = 0; i<u.size(); ++i) {
        o<<std::setw(2)<<static_cast<unsigned>(std::to_integer<std::uint8_t>(u[i]));
        if (i == 3 || i == 5 || i == 7 || i == 9)o<<'-';
    }
    return o.str();
}

Result<bool> probe(BlockDevice&dev) {
    if (dev.geometry().size_bytes<MIRRORS[0]+SB_SIZE)return false;
    std::array<std::byte, 8>b{};
    auto r = dev.read_exact(MIRRORS[0]+64, b);
    if (!r)return r.error();
    return read_le<std::uint64_t>(b, 0) == MAGIC;
}

Result<Superblock> read_best_superblock(BlockDevice&dev) {
    std::vector<Superblock> candidates;
    for (auto off:MIRRORS) {
        if (off>dev.geometry().size_bytes || SB_SIZE>dev.geometry().size_bytes-off)continue;
        auto s = parse(dev, off);
        if (!s) {
            if (s.error().code == Errc::unsupported || s.error().code == Errc::not_found)continue;
            return s.error();
        }
        if (s.value().checksum_supported && s.value().checksum_valid
            && sane_geometry(dev, s.value()))candidates.push_back(s.value());
    }
    if (candidates.empty())
        return Error{Errc::corrupt, 0,
            "no checksum-valid, geometrically sane Btrfs superblock mirror found"};
    return *std::max_element(candidates.begin(), candidates.end(),
                             [](const Superblock& a, const Superblock& b) {
                             return a.generation < b.generation;
                             });
}

Result<CheckReport> check(BlockDevice&dev) {
    CheckReport r;
    std::vector<Superblock> all, valid;
    for (auto off:MIRRORS) {
        if (off>dev.geometry().size_bytes || SB_SIZE>dev.geometry().size_bytes-off)continue;
        auto s = parse(dev, off);
        if (!s) {
            if (s.error().code == Errc::unsupported || s.error().code == Errc::not_found)continue;
            return s.error();
        }
        ++r.mirrors_present;
        all.push_back(s.value());
        if (!s.value().checksum_supported) {
            fail(r,"Btrfs superblock uses unsupported checksum type");
            continue;
        }
        if (!s.value().checksum_valid) {
            fail(r,"Btrfs superblock checksum is invalid at mirror offset "+std::to_string(off));
            continue;
        }
        ++r.mirrors_valid;
        valid.push_back(s.value());
    }
    if (all.empty())
        return Error{Errc::unsupported, 0, "not a Btrfs filesystem"};
    if (valid.empty()) {
        fail(r, "no checksum-valid Btrfs superblock mirrors remain");
        return r;
    }
    const auto& best = *std::max_element(valid.begin(), valid.end(),
                                         [](const Superblock& a, const Superblock& b) {
                                         return a.generation < b.generation;
                                         });
    r.selected_generation = best.generation;
    if (best.bytenr != best.mirror_offset) fail(r,
                                                "selected Btrfs superblock bytenr does not match mirror location");
    if (!power2(best.sectorsize) || best.sectorsize<4096
        || best.sectorsize>65536) fail(r,"invalid Btrfs sector size");
    if (!power2(best.nodesize) || best.nodesize<best.sectorsize
        || best.nodesize>65536) fail(r,"invalid Btrfs node size");
    if (best.stripesize == 0 || !power2(best.stripesize))
        fail(r, "invalid Btrfs stripe size");
    if (best.total_bytes == 0)
        fail(r, "Btrfs total_bytes is zero");
    if (best.bytes_used > best.total_bytes)
        fail(r, "Btrfs bytes_used exceeds total_bytes");
    if (best.num_devices == 0)
        fail(r, "Btrfs num_devices is zero");
    if (best.device_id == 0)
        fail(r, "Btrfs embedded device id is zero");
    if (best.device_total_bytes == 0 || best.device_total_bytes > dev.geometry().size_bytes)
        fail(r, "Btrfs embedded device geometry exceeds containing device or is zero");
    if (best.device_bytes_used > best.device_total_bytes)
        fail(r, "Btrfs embedded device bytes_used exceeds device total_bytes");
    if (best.num_devices == 1 && best.total_bytes > dev.geometry().size_bytes)
        fail(r, "single-device Btrfs total_bytes exceeds containing device");
    if (best.root_level > 8 || best.chunk_root_level > 8 || best.log_root_level > 8)
        fail(r, "Btrfs tree level is outside supported on-disk range");
    if (best.sys_chunk_array_size > 2048)
        fail(r, "Btrfs system chunk array size is invalid");
    for (const auto& s : valid) {
        if (!same_uuid(s.fsid, best.fsid))fail(r,"checksum-valid Btrfs mirrors disagree on filesystem UUID");
        if (s.total_bytes != best.total_bytes || s.sectorsize != best.sectorsize
            || s.nodesize != best.nodesize)fail(r,"checksum-valid Btrfs mirrors disagree on filesystem geometry");
        if (s.bytenr != s.mirror_offset)fail(r,"Btrfs mirror bytenr does not match its physical offset");
        if (s.generation<best.generation
            && best.generation - s.generation > 1) {
            r.warnings.push_back(
                "a Btrfs superblock mirror is more than one generation behind the newest mirror");
        }
    }
    if (r.mirrors_valid<r.mirrors_present)
        r.warnings.push_back("one or more Btrfs superblock mirrors are unusable");
    r.clean = r.errors.empty();
    return r;
}

} // namespace fsx::btrfs
