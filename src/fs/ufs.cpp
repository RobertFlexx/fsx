#include "fsx/ufs.hpp"
#include "fsx/endian.hpp"
#include <array>
#include <limits>

namespace fsx::ufs {
namespace {
constexpr std::uint32_t UFS1_MAGIC = 0x00011954U;
constexpr std::uint32_t UFS2_MAGIC = 0x19540119U;
constexpr std::size_t SUPERBLOCK_BYTES = 8192;
constexpr std::size_t MAGIC_OFFSET = 1372;
constexpr std::array<std::uint64_t, 4> SEARCH{{65536ULL, 8192ULL, 0ULL, 262144ULL}};

bool power2(std::uint32_t x) noexcept { return x != 0 && (x & (x - 1U)) == 0;
}

std::uint32_t read32(std::span<const std::byte> b, std::size_t off, ByteOrder order) {
    return order == ByteOrder::little ? read_le<std::uint32_t>(b, off) : read_be<std::uint32_t>(b, off);
}

struct MagicResult { Version version;
    ByteOrder order;
};
std::optional<MagicResult> magic_at(std::span<const std::byte> b) {
    if (b.size() < MAGIC_OFFSET + 4) return std::nullopt;
    const auto le = read_le<std::uint32_t>(b, MAGIC_OFFSET);
    if (le == UFS1_MAGIC) return MagicResult{Version::ufs1, ByteOrder::little};
    if (le == UFS2_MAGIC) return MagicResult{Version::ufs2, ByteOrder::little};
    const auto be = read_be<std::uint32_t>(b, MAGIC_OFFSET);
    if (be == UFS1_MAGIC) return MagicResult{Version::ufs1, ByteOrder::big};
    if (be == UFS2_MAGIC) return MagicResult{Version::ufs2, ByteOrder::big};
    return std::nullopt;
}

Result<Superblock> parse_at(BlockDevice& dev, std::uint64_t offset) {
    if (offset > dev.geometry().size_bytes || SUPERBLOCK_BYTES > dev.geometry().size_bytes - offset)
        return Error{Errc::not_found, 0,"UFS superblock candidate lies outside device"};
    std::array<std::byte, SUPERBLOCK_BYTES> b{};
    auto rr = dev.read_exact(offset, b);
    if (!rr) return rr.error();
    const auto magic = magic_at(b);
    if (!magic) return Error{
        Errc::unsupported, 0,"UFS magic is absent at canonical superblock location"
    }
    ;
    Superblock s;
    s.version = magic->version;
    s.byte_order = magic->order;
    s.offset = offset;
    // These fields are in the stable, fixed-width prefix shared by BSD FFS/UFS
    // superblock ABIs. Avoid pointer-bearing/later ABI regions entirely.
    s.old_size_fragments = read32(b, 36, s.byte_order);
    s.cylinder_groups = read32(b, 44, s.byte_order);
    s.block_size = read32(b, 48, s.byte_order);
    s.fragment_size = read32(b, 52, s.byte_order);
    s.fragments_per_block = read32(b, 56, s.byte_order);
    s.superblock_size = read32(b, 104, s.byte_order);
    s.inodes_per_block = read32(b, 120, s.byte_order);
    s.inodes_per_group = read32(b, 184, s.byte_order);
    s.fragments_per_group = read32(b, 188, s.byte_order);
    s.clean_flags = std::to_integer<std::uint8_t>(b[209]);
    return s;
}

void fail(CheckReport& r, std::string m) {
    r.clean = false;
    r.errors.push_back(std::move(m));
}
}

const char* version_name(Version v) noexcept { return v == Version::ufs2 ? "UFS2/FFS2" : "UFS1/FFS1";
}

Result<bool> probe(BlockDevice& dev) {
    for (auto off:SEARCH) {
        if (off>dev.geometry().size_bytes || SUPERBLOCK_BYTES>dev.geometry().size_bytes-off) continue;
        std::array<std::byte, SUPERBLOCK_BYTES> b{};
        auto rr = dev.read_exact(off, b);
        if (!rr)return rr.error();
        if (magic_at(b)) return true;
    }
    return false;
}

Result<Superblock> read_superblock(BlockDevice& dev) {
    for (auto off:SEARCH) {
        if (off>dev.geometry().size_bytes || SUPERBLOCK_BYTES>dev.geometry().size_bytes-off) continue;
        auto s = parse_at(dev, off);
        if (s)return s.value();
        if (s.error().code != Errc::unsupported && s.error().code != Errc::not_found) return s.error();
    }
    return Error{Errc::unsupported, 0,"no BSD UFS1/UFS2 superblock found at canonical locations"};
}

Result<CheckReport> check(BlockDevice& dev) {
    auto sr = read_superblock(dev);
    if (!sr)return sr.error();
    const auto&s = sr.value();
    CheckReport r;
    if (!power2(s.block_size) || s.block_size<4096U || s.block_size>65536U) fail(r,"invalid UFS block size");
    if (!power2(s.fragment_size) || s.fragment_size<512U
        || s.fragment_size>s.block_size) fail(r,"invalid UFS fragment size");
    if (s.fragment_size != 0 && s.block_size / s.fragment_size != s.fragments_per_block) {
        fail(r, "UFS fragments-per-block disagrees with block/fragment sizes");
    }
    if (s.fragments_per_block == 0 || s.fragments_per_block>8U
        || !power2(s.fragments_per_block)) fail(r,"invalid UFS fragments-per-block");
    if (s.cylinder_groups == 0 || s.cylinder_groups>10000000U) fail(r,"invalid UFS cylinder-group count");
    if (s.superblock_size<MAGIC_OFFSET+4U
        || s.superblock_size>SUPERBLOCK_BYTES) fail(r,"invalid UFS superblock size");
    if (s.inodes_per_block == 0 || s.inodes_per_group == 0
        || s.fragments_per_group == 0) fail(r,"invalid UFS inode/cylinder-group geometry");
    if (s.inodes_per_block != 0) {
        const auto inode_bytes = s.block_size/s.inodes_per_block;
        if (inode_bytes<128U || inode_bytes>1024U || !power2(inode_bytes)) fail(r,
                                                                                "derived UFS inode size is "
                                                                               "implausible");
    }
    if (s.version == Version::ufs1 && s.old_size_fragments != 0 && s.fragment_size != 0) {
        const auto max = std::numeric_limits<std::uint64_t>::max()/s.fragment_size;
        if (s.old_size_fragments>max) fail(r,"UFS1 size arithmetic overflows");
        else {
            const auto bytes = static_cast<std::uint64_t>(s.old_size_fragments)*s.fragment_size;
            if (bytes>dev.geometry().size_bytes) fail(r,"UFS1 filesystem geometry exceeds containing device");
        }
    }
    if (s.version == Version::ufs2
        && s.offset != 65536ULL) r.warnings.push_back("UFS2 superblock found at a nonstandard fallback location");
    if (s.version == Version::ufs1 && s.offset != 8192ULL
        && s.offset != 0ULL) r.warnings.push_back("UFS1 superblock found at a fallback location");
    // FS_ISCLEAN / FS_WASCLEAN are the two canonical low clean-state bits.
    if ((s.clean_flags & 0x03U) == 0) {
        r.warnings.push_back("UFS clean-state flags do not indicate a clean previous state");
    }
    r.clean = r.errors.empty();
    return r;
}

} // namespace fsx::ufs
