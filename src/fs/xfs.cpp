#include "fsx/xfs.hpp"
#include "fsx/endian.hpp"
#include "fsx/crc.hpp"
#include <algorithm>
#include <array>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>

namespace fsx::xfs {
namespace {
bool pow2(std::uint64_t x) {
    return x && (x & (x - 1U)) == 0;
}
void err(CheckReport& r, std::string s) {
    r.clean = false;
    r.errors.push_back(std::move(s));
}

Result<std::array<std::byte, 512>> read_sb_raw(BlockDevice&dev, std::uint64_t off) {
    if (off>dev.geometry().size_bytes || 512ULL>dev.geometry().size_bytes-off)return Error{
        Errc::corrupt, 0,"XFS allocation-group superblock lies outside device"
    }
    ;
    std::array<std::byte, 512>b{};
    auto rr = dev.read_exact(off, b);
    if (!rr)return rr.error();
    return b;
}
Result<Superblock> parse_raw(std::span<const std::byte>b) {
    if (b.size()<512)return Error{Errc::corrupt, 0,"short XFS superblock"};
    if (std::to_integer<char>(b[0]) != 'X' || std::to_integer<char>(b[1]) != 'F'
        || std::to_integer<char>(b[2]) != 'S' || std::to_integer<char>(b[3]) != 'B')return Error{
        Errc::unsupported, 0,"not an XFS filesystem"
    }
    ;
    Superblock s;
    s.block_size = read_be<std::uint32_t>(b, 4);
    s.data_blocks = read_be<std::uint64_t>(b, 8);
    std::copy_n(b.begin()+32, 16, s.uuid.begin());
    s.ag_blocks = read_be<std::uint32_t>(b, 84);
    s.ag_count = read_be<std::uint32_t>(b, 88);
    s.log_blocks = read_be<std::uint32_t>(b, 96);
    s.version = read_be<std::uint16_t>(b, 100);
    s.sector_size = read_be<std::uint16_t>(b, 102);
    s.inode_size = read_be<std::uint16_t>(b, 104);
    s.inodes_per_block = read_be<std::uint16_t>(b, 106);
    for (std::size_t i = 108; i<120
         && std::to_integer<unsigned char>(b[i]) != 0;
         ++i)s.label.push_back(static_cast<char>(std::to_integer<unsigned char>(b[i])));
    return s;
}
bool same_uuid(const std::array<std::byte, 16>&a, const std::array<std::byte, 16>&b) {
    return a == b;
}
bool verify_v5_crc(BlockDevice&dev, std::uint64_t off, const Superblock&s) {
    if ((s.version&0x000fU) != 5U)return true;
    if (s.sector_size<512U || s.sector_size>32768U || off>dev.geometry().size_bytes
        || s.sector_size>dev.geometry().size_bytes-off)return false;
    std::vector<std::byte>b(s.sector_size);
    if (!dev.read_exact(off, b))return false;
    constexpr std::size_t crc_off = 224;
    if (b.size()<crc_off+4)return false;
    const auto stored = read_le<std::uint32_t>(b, crc_off);
    b[crc_off] = b[crc_off+1] = b[crc_off+2] = b[crc_off+3] = std::byte{0};
    const auto calc = ~crc32c(b);
    return stored == calc;
}
}

std::string uuid_string(const std::array<std::byte, 16>& u) {
    std::ostringstream o;
    o << std::hex << std::setfill('0');
    for (std::size_t i = 0; i<u.size(); ++i) {
        o << std::setw(2) << static_cast<unsigned>(std::to_integer<std::uint8_t>(u[i]));
        if (i == 3 || i == 5 || i == 7 || i == 9) o << '-';
    }
    return o.str();
}
Result<Superblock> read_superblock(BlockDevice& dev) {
    if (dev.geometry().size_bytes<512)return Error{
        Errc::unsupported, 0,"device is too small for XFS"
    }
    ;
    auto b = read_sb_raw(dev, 0);
    if (!b)return b.error();
    return parse_raw(b.value());
}

Result<CheckReport> check(BlockDevice& dev) {
    auto sr = read_superblock(dev);
    if (!sr)return sr.error();
    const auto&s = sr.value();
    CheckReport r;
    if (!pow2(s.block_size) || s.block_size<512 || s.block_size>65536)err(r,"invalid XFS block size");
    if (!pow2(s.sector_size) || s.sector_size<512 || s.sector_size>s.block_size)err(r,"invalid XFS sector size");
    if (s.data_blocks == 0)err(r,"XFS data block count is zero");
    else if (s.block_size && s.data_blocks>dev.geometry().size_bytes/s.block_size)err(r,
                                                                                      "XFS data geometry exceeds "
                                                                                     "containing device");
    if (s.ag_blocks == 0 || s.ag_count == 0)err(r,"XFS allocation-group geometry is zero");
    else {
        const auto full_before_last = static_cast<std::uint64_t>(s.ag_blocks)*(s.ag_count-1ULL);
        if (full_before_last >= s.data_blocks)err(r,"XFS allocation-group geometry exceeds data blocks");
        const auto last_blocks = s.data_blocks-full_before_last;
        if (last_blocks == 0 || last_blocks>s.ag_blocks)err(r,"XFS final allocation-group size is invalid");
    }
    if (!pow2(s.inode_size) || s.inode_size<256 || s.inode_size>s.block_size)err(r,"invalid XFS inode size");
    if (s.inode_size && s.block_size/s.inode_size != s.inodes_per_block)err(r,
                                                                            "XFS inodes-per-block does not match "
                                                                           "block/inode size");
    const auto v = static_cast<unsigned>(s.version&0x000fU);
    if (v<4 || v>5)r.warnings.push_back("XFS superblock version is outside the modern v4/v5 range");
    if (v == 5U && !verify_v5_crc(dev, 0, s))err(r,"XFS primary superblock CRC32C is invalid");
    if (!r.clean)return r;

    if (s.ag_count>1000000U) {
        err(r,"XFS allocation-group count is unreasonable");
        return r;
    }
    // Every allocation group begins with a redundant superblock. Check all of
    // them: this is cheap (512 bytes per AG) and catches split geometry/UUIDs.
    for (std::uint32_t ag = 1; ag<s.ag_count; ++ag) {
        const auto ag_block = static_cast<std::uint64_t>(ag)*s.ag_blocks;
        if (ag_block>std::numeric_limits<std::uint64_t>::max()/s.block_size) {
            err(r,"XFS allocation-group offset overflows");
            break;
        }
        const auto off = ag_block*s.block_size;
        auto raw = read_sb_raw(dev, off);
        if (!raw) {
            err(r,"cannot read XFS secondary superblock for AG "+std::to_string(ag));
            break;
        }
        auto other = parse_raw(raw.value());
        if (!other) {
            err(r,"invalid XFS secondary superblock for AG "+std::to_string(ag));
            continue;
        }
        const auto&o = other.value();
        if (o.block_size != s.block_size || o.data_blocks != s.data_blocks || o.ag_blocks != s.ag_blocks
            || o.ag_count != s.ag_count || o.sector_size != s.sector_size
            || o.inode_size != s.inode_size) {
            err(r, "XFS secondary superblock geometry disagrees in AG " + std::to_string(ag));
        }
        if (!same_uuid(o.uuid, s.uuid)) {
            err(r, "XFS secondary superblock UUID disagrees in AG " + std::to_string(ag));
        }
        if (v == 5U && !verify_v5_crc(dev, off, o)) {
            err(r, "XFS secondary superblock CRC32C is invalid in AG " + std::to_string(ag));
        }
    }
    r.clean = r.errors.empty();
    return r;
}
} // namespace fsx::xfs
