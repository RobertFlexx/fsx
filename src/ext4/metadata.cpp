#include "fsx/ext4_metadata.hpp"
#include "fsx/cancel.hpp"
#include "fsx/crc.hpp"
#include "fsx/endian.hpp"
#include "internal.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <set>

namespace fsx::ext4 {
namespace {
constexpr std::uint32_t RO_GDT_CSUM = 0x0010U;
constexpr std::uint32_t RO_METADATA_CSUM = 0x0400U;
constexpr std::uint32_t INCOMPAT_CSUM_SEED = 0x2000U;
constexpr std::size_t SB_CHECKSUM_OFF = 0x3fc;
constexpr std::size_t SB_CHECKSUM_SEED_OFF = 0x270;
constexpr std::size_t GD_BLOCK_CSUM_LO = 0x18;
constexpr std::size_t GD_INODE_CSUM_LO = 0x1a;
constexpr std::size_t GD_CHECKSUM = 0x1e;
constexpr std::size_t GD_BLOCK_CSUM_HI = 0x38;
constexpr std::size_t GD_INODE_CSUM_HI = 0x3a;
constexpr std::size_t INODE_CSUM_LO = 0x7c;
constexpr std::size_t INODE_EXTRA_ISIZE = 0x80;
constexpr std::size_t INODE_CSUM_HI = 0x82;

std::uint16_t crc16_ibm(std::span<const std::byte> data, std::uint16_t seed) noexcept {
    std::uint16_t crc = seed;
    for (auto b : data) {
        crc ^= static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(b));
        for (unsigned i = 0; i<8; ++i) crc = (crc & 1U) ? static_cast<std::uint16_t>((crc >> 1U) ^ 0xa001U) :
            static_cast<std::uint16_t>(crc >> 1U);
    }
    return crc;
}

std::array<std::byte, 4> le32_bytes(std::uint32_t v) {
    std::array<std::byte, 4> b{};
    write_le32(b, 0, v);
    return b;
}

std::uint32_t metadata_seed(std::span<const std::byte> raw_sb) {
    const auto incompat = read_le<std::uint32_t>(raw_sb, 0x60);
    if ((incompat & INCOMPAT_CSUM_SEED) != 0) return read_le<std::uint32_t>(raw_sb, SB_CHECKSUM_SEED_OFF);
    return crc32c(raw_sb.subspan(0x68, 16), 0xffffffffU);
}

std::uint32_t bitmap_checksum(const MetadataContext& ctx,
                              std::span<const std::byte> bitmap, std::size_t bytes) {
    return crc32c(bitmap.first(std::min(bytes, bitmap.size())), ctx.checksum_seed);
}

std::uint32_t descriptor_bitmap_checksum(std::span<const std::byte> gd, bool inode) {
    const auto lo = read_le<std::uint16_t>(gd, inode?GD_INODE_CSUM_LO:GD_BLOCK_CSUM_LO);
    std::uint32_t v = lo;
    const auto hi_off = inode?GD_INODE_CSUM_HI:GD_BLOCK_CSUM_HI;
    if (gd.size() >= hi_off+2) v|=static_cast<std::uint32_t>(read_le<std::uint16_t>(gd, hi_off))<<16U;
    return v;
}

Result<std::uint64_t> inode_offset(BlockDevice& dev, const MetadataContext& ctx, std::uint64_t inode) {
    if (inode == 0 || inode>ctx.sb.inodes_count) return Error{
        Errc::invalid_argument, 0,"inode number outside filesystem"
    }
    ;
    const auto z = inode-1;
    const auto group = z/ctx.sb.inodes_per_group;
    const auto index = z%ctx.sb.inodes_per_group;
    auto gd = detail::read_group_desc(dev, ctx.sb, group);
    if (!gd)return gd.error();
    const auto bs = ctx.sb.block_size();
    if (bs == 0 || gd.value().inode_table>std::numeric_limits<std::uint64_t>::max()/bs)
        return Error{Errc::corrupt, 0,"inode table byte offset overflows"};
    const auto table_off = gd.value().inode_table*bs;
    const auto inode_size = static_cast<std::uint64_t>(ctx.sb.inode_size);
    if (index>std::numeric_limits<std::uint64_t>::max()/inode_size)
        return Error{Errc::corrupt, 0,"inode entry offset overflows"};
    const auto rel = index*inode_size;
    if (rel>std::numeric_limits<std::uint64_t>::max()-table_off)
        return Error{Errc::corrupt, 0,"inode absolute offset overflows"};
    const auto off = table_off+rel;
    const auto fs_bytes = ctx.sb.size_bytes();
    if (off>fs_bytes || inode_size>fs_bytes-off) return Error{
        Errc::corrupt, 0,"inode table entry lies beyond filesystem"
    }
    ;
    return off;
}

bool inode_has_hi_checksum(std::span<const std::byte> inode) {
    if (inode.size() <= 128 || inode.size()<INODE_CSUM_HI+2) return false;
    if (inode.size()<INODE_EXTRA_ISIZE+2) return false;
    return read_le<std::uint16_t>(inode, INODE_EXTRA_ISIZE) >= 4;
}

std::uint32_t compute_inode_checksum(const MetadataContext& ctx,
                                     std::uint64_t inode_no, std::span<std::byte> inode) {
    const std::uint16_t old_lo = inode.size() >= INODE_CSUM_LO+2?read_le<std::uint16_t>(inode,
                                                                                        INODE_CSUM_LO):std::uint16_t{
        0
    }
    ;
    const bool has_hi = inode_has_hi_checksum(inode);
    const std::uint16_t old_hi = has_hi?read_le<std::uint16_t>(inode, INODE_CSUM_HI):std::uint16_t{0};
    if (inode.size() >= INODE_CSUM_LO+2) write_le16(inode, INODE_CSUM_LO, 0);
    if (has_hi) write_le16(inode, INODE_CSUM_HI, 0);
    const auto ino = le32_bytes(static_cast<std::uint32_t>(inode_no));
    const auto gen = le32_bytes(read_le<std::uint32_t>(inode, 0x64));
    auto crc = crc32c(ino, ctx.checksum_seed);
    crc = crc32c(gen, crc);
    crc = crc32c(inode, crc);
    if (inode.size() >= INODE_CSUM_LO+2) write_le16(inode, INODE_CSUM_LO, old_lo);
    if (has_hi) write_le16(inode, INODE_CSUM_HI, old_hi);
    return crc;
}

Result<std::uint32_t> compute_extent_checksum(const MetadataContext& ctx,
                                              std::uint64_t inode, std::uint32_t generation,
                                              std::span<const std::byte> block) {
    if (block.size()<12) return Error{Errc::corrupt, 0,"extent block too small"};
    if (read_le<std::uint16_t>(block, 0) != 0xf30aU) return Error{Errc::corrupt, 0,"invalid extent block magic"};
    const auto max = read_le<std::uint16_t>(block, 4);
    const std::uint64_t tail = 12ULL+static_cast<std::uint64_t>(max)*12ULL;
    if (tail+4>block.size()) return Error{Errc::corrupt, 0,"extent checksum tail lies outside block"};
    const auto ino = le32_bytes(static_cast<std::uint32_t>(inode));
    const auto gen = le32_bytes(generation);
    auto crc = crc32c(ino, ctx.checksum_seed);
    crc = crc32c(gen, crc);
    crc = crc32c(block.first(static_cast<std::size_t>(tail)), crc);
    return crc;
}

std::uint64_t popcount_bitmap(std::span<const std::byte> bm, std::uint64_t bits) {
    std::uint64_t n = 0;
    for (std::uint64_t i = 0; i<bits; ++i) if (detail::bit_set(bm, i))++n;
    return n;
}
}

Result<std::vector<std::byte>> read_raw_superblock(BlockDevice& dev) {
    std::vector<std::byte> b(1024);
    auto r = dev.read_exact(1024, b);
    if (!r)return r.error();
    return b;
}
Result<void> write_raw_superblock(BlockDevice& dev, std::span<const std::byte> raw) {
    if (raw.size() != 1024)return Error{
        Errc::invalid_argument, 0,"ext superblock write requires exactly 1024 bytes"
    }
    ;
    return dev.write_exact(1024, raw);
}
bool verify_superblock_checksum(std::span<const std::byte> raw) noexcept {
    if (raw.size()<1024)return false;
    const auto ro = read_le<std::uint32_t>(raw, 0x64);
    if ((ro&RO_METADATA_CSUM) == 0)return true;
    const auto stored = read_le<std::uint32_t>(raw, SB_CHECKSUM_OFF);
    const auto calc = crc32c(raw.first(SB_CHECKSUM_OFF), 0xffffffffU);
    return stored == calc;
}
Result<void> update_superblock_checksum(std::span<std::byte> raw) {
    if (raw.size()<1024)return Error{Errc::invalid_argument, 0,"superblock buffer is too small"};
    const auto ro = read_le<std::uint32_t>(raw, 0x64);
    if ((ro&RO_METADATA_CSUM) == 0)return {};
    write_le32(raw, SB_CHECKSUM_OFF,
               crc32c(std::span<const std::byte>(raw).first(SB_CHECKSUM_OFF), 0xffffffffU));
    return {};
}

Result<MetadataContext> load_metadata_context(BlockDevice& dev, bool tolerate_bad_superblock_checksum) {
    auto sb = read_superblock(dev);
    if (!sb)return sb.error();
    auto raw = read_raw_superblock(dev);
    if (!raw)return raw.error();
    MetadataContext c;
    c.sb = sb.value();
    std::copy_n(raw.value().begin()+0x68, 16, c.uuid.begin());
    c.metadata_csum = (c.sb.feature_ro_compat&RO_METADATA_CSUM) != 0;
    c.gdt_csum = (c.sb.feature_ro_compat&RO_GDT_CSUM) != 0 || c.metadata_csum;
    c.csum_seed_feature = (c.sb.feature_incompat&INCOMPAT_CSUM_SEED) != 0;
    c.checksum_seed = metadata_seed(raw.value());
    if (c.metadata_csum && !verify_superblock_checksum(raw.value())
        && !tolerate_bad_superblock_checksum) return Error{
        Errc::corrupt, 0,"ext4 superblock checksum mismatch"
    }
    ;
    return c;
}

Result<std::vector<std::byte>> read_group_descriptor_raw(BlockDevice& dev,
                                                         const MetadataContext& ctx, std::uint64_t group) {
    if (group >= ctx.sb.groups_count())return Error{Errc::invalid_argument, 0,"group number outside filesystem"};
    const auto bs = ctx.sb.block_size();
    const auto ds64 = static_cast<std::uint64_t>(ctx.sb.desc_size >= 32?ctx.sb.desc_size:32);
    if (bs == 0 || ds64 == 0 || ds64>bs)return Error{Errc::corrupt, 0,"invalid ext4 descriptor geometry"};
    if (group>std::numeric_limits<std::uint64_t>::max()/ds64)return Error{
        Errc::corrupt, 0,"group descriptor offset overflows"
    }
    ;
    const auto desc_off = group*ds64;
    if ((ctx.sb.feature_incompat&detail::INCOMPAT_META_BG) != 0 && desc_off >= bs)
        return Error{
            Errc::unsupported, 0,"meta_bg descriptor placement is not supported by the write engine yet"
        }
    ;
    const auto table_block = bs == 1024?2ULL:1ULL;
    const auto table_off = table_block*bs;
    if (desc_off>std::numeric_limits<std::uint64_t>::max()-table_off)return Error{
        Errc::corrupt, 0,"group descriptor address overflows"
    }
    ;
    const auto ds = static_cast<std::size_t>(ds64);
    std::vector<std::byte> b(ds);
    auto r = dev.read_exact(table_off+desc_off, b);
    if (!r)return r.error();
    return b;
}
Result<void> write_group_descriptor_raw(BlockDevice& dev,
                                        const MetadataContext& ctx, std::uint64_t group,
                                        std::span<const std::byte> raw) {
    const auto bs = ctx.sb.block_size();
    const auto ds64 = static_cast<std::uint64_t>(ctx.sb.desc_size >= 32?ctx.sb.desc_size:32);
    if (bs == 0 || ds64 == 0 || ds64>bs)return Error{Errc::corrupt, 0,"invalid ext4 descriptor geometry"};
    if (raw.size() != ds64)return Error{Errc::invalid_argument, 0,"group descriptor buffer has wrong size"};
    if (group >= ctx.sb.groups_count())return Error{Errc::invalid_argument, 0,"group number outside filesystem"};
    if (group>std::numeric_limits<std::uint64_t>::max()/ds64)return Error{
        Errc::corrupt, 0,"group descriptor offset overflows"
    }
    ;
    const auto desc_off = group*ds64;
    if ((ctx.sb.feature_incompat&detail::INCOMPAT_META_BG) != 0 && desc_off >= bs)return Error{
        Errc::unsupported, 0,"meta_bg descriptor writes are not supported yet"
    }
    ;
    const auto table_block = bs == 1024?2ULL:1ULL;
    const auto table_off = table_block*bs;
    if (desc_off>std::numeric_limits<std::uint64_t>::max()-table_off)return Error{
        Errc::corrupt, 0,"group descriptor address overflows"
    }
    ;
    return dev.write_exact(table_off+desc_off, raw);
}

Result<bool> verify_group_descriptor_checksum(const MetadataContext& ctx,
                                              std::uint64_t group, std::span<const std::byte> raw) {
    if (!ctx.gdt_csum)return true;
    if (raw.size()<32)return Error{Errc::corrupt, 0,"group descriptor too short"};
    const auto stored = read_le<std::uint16_t>(raw, GD_CHECKSUM);
    auto copy = std::vector<std::byte>(raw.begin(), raw.end());
    write_le16(copy, GD_CHECKSUM, 0);
    const auto gb = le32_bytes(static_cast<std::uint32_t>(group));
    std::uint16_t expected = 0;
    if (ctx.metadata_csum) {
        auto crc = crc32c(gb, ctx.checksum_seed);
        crc = crc32c(copy, crc);
        expected = static_cast<std::uint16_t>(crc&0xffffU);
    } else {
        auto crc = crc16_ibm(ctx.uuid, 0xffffU);
        crc = crc16_ibm(gb, crc);
        crc = crc16_ibm(std::span<const std::byte>(copy).first(30), crc);
        if (copy.size()>32)crc = crc16_ibm(std::span<const std::byte>(copy).subspan(32), crc);
        expected = crc;
    }
    return stored == expected;
}
Result<void> update_group_descriptor_checksum(const MetadataContext& ctx,
                                              std::uint64_t group, std::span<std::byte> raw) {
    if (!ctx.gdt_csum)return {};
    if (raw.size()<32)return Error{Errc::corrupt, 0,"group descriptor too short"};
    write_le16(raw, GD_CHECKSUM, 0);
    const auto gb = le32_bytes(static_cast<std::uint32_t>(group));
    std::uint16_t out = 0;
    if (ctx.metadata_csum) {
        auto crc = crc32c(gb, ctx.checksum_seed);
        crc = crc32c(std::span<const std::byte>(raw), crc);
        out = static_cast<std::uint16_t>(crc&0xffffU);
    }
    else {
        auto crc = crc16_ibm(ctx.uuid, 0xffffU);
        crc = crc16_ibm(gb, crc);
        crc = crc16_ibm(std::span<const std::byte>(raw).first(30), crc);
        if (raw.size()>32)crc = crc16_ibm(std::span<const std::byte>(raw).subspan(32), crc);
        out = crc;
    }
    write_le16(raw, GD_CHECKSUM, out);
    return {};
}

Result<bool> verify_block_bitmap_checksum(const MetadataContext& ctx,
                                          std::span<const std::byte> gd, std::span<const std::byte> bitmap) {
    if (!ctx.metadata_csum)return true;
    const auto bytes = static_cast<std::size_t>((static_cast<std::uint64_t>(ctx.sb.blocks_per_group)+7ULL)/8ULL);
    const auto got = bitmap_checksum(ctx, bitmap, bytes);
    const auto stored = descriptor_bitmap_checksum(gd, false);
    return gd.size() >= GD_BLOCK_CSUM_HI+2?stored == got:(stored&0xffffU) == (got&0xffffU);
}
Result<bool> verify_inode_bitmap_checksum(const MetadataContext& ctx,
                                          std::span<const std::byte> gd, std::span<const std::byte> bitmap) {
    if (!ctx.metadata_csum)return true;
    const auto bytes = static_cast<std::size_t>((static_cast<std::uint64_t>(ctx.sb.inodes_per_group)+7ULL)/8ULL);
    const auto got = bitmap_checksum(ctx, bitmap, bytes);
    const auto stored = descriptor_bitmap_checksum(gd, true);
    return gd.size() >= GD_INODE_CSUM_HI+2?stored == got:(stored&0xffffU) == (got&0xffffU);
}
Result<void> update_block_bitmap_checksum(const MetadataContext& ctx,
                                          std::span<std::byte> gd, std::span<const std::byte> bitmap) {
    if (!ctx.metadata_csum)return {};
    const auto bytes = static_cast<std::size_t>((static_cast<std::uint64_t>(ctx.sb.blocks_per_group)+7ULL)/8ULL);
    const auto c = bitmap_checksum(ctx, bitmap, bytes);
    write_le16(gd, GD_BLOCK_CSUM_LO, static_cast<std::uint16_t>(c));
    if (gd.size() >= GD_BLOCK_CSUM_HI+2)write_le16(gd, GD_BLOCK_CSUM_HI, static_cast<std::uint16_t>(c>>16U));
    return {};
}
Result<void> update_inode_bitmap_checksum(const MetadataContext& ctx,
                                          std::span<std::byte> gd, std::span<const std::byte> bitmap) {
    if (!ctx.metadata_csum)return {};
    const auto bytes = static_cast<std::size_t>((static_cast<std::uint64_t>(ctx.sb.inodes_per_group)+7ULL)/8ULL);
    const auto c = bitmap_checksum(ctx, bitmap, bytes);
    write_le16(gd, GD_INODE_CSUM_LO, static_cast<std::uint16_t>(c));
    if (gd.size() >= GD_INODE_CSUM_HI+2)write_le16(gd, GD_INODE_CSUM_HI, static_cast<std::uint16_t>(c>>16U));
    return {};
}

Result<std::vector<std::byte>> read_inode_raw(BlockDevice& dev,
                                              const MetadataContext& ctx, std::uint64_t inode) {
    auto off = inode_offset(dev, ctx, inode);
    if (!off)return off.error();
    std::vector<std::byte>b(ctx.sb.inode_size);
    auto r = dev.read_exact(off.value(), b);
    if (!r)return r.error();
    return b;
}
Result<void> write_inode_raw(BlockDevice& dev, const MetadataContext& ctx,
                             std::uint64_t inode, std::span<const std::byte> raw) {
    if (raw.size() != ctx.sb.inode_size)return Error{
        Errc::invalid_argument, 0,"inode buffer has wrong size"
    }
    ;
    auto off = inode_offset(dev, ctx, inode);
    if (!off)return off.error();
    return dev.write_exact(off.value(), raw);
}
Result<bool> verify_inode_checksum(const MetadataContext& ctx,
                                   std::uint64_t inode, std::span<const std::byte> raw) {
    if (!ctx.metadata_csum)return true;
    if (raw.size()<128)return Error{Errc::corrupt, 0,"inode buffer too short"};
    auto copy = std::vector<std::byte>(raw.begin(), raw.end());
    const auto stored_lo = read_le<std::uint16_t>(copy, INODE_CSUM_LO);
    const bool hi = inode_has_hi_checksum(copy);
    const auto stored_hi = hi?read_le<std::uint16_t>(copy, INODE_CSUM_HI):0;
    const auto expected = compute_inode_checksum(ctx, inode, copy);
    const auto stored = static_cast<std::uint32_t>(stored_lo)|(static_cast<std::uint32_t>(stored_hi)<<16U);
    return hi?stored == expected:(stored&0xffffU) == (expected&0xffffU);
}
Result<void> update_inode_checksum(const MetadataContext& ctx, std::uint64_t inode, std::span<std::byte> raw) {
    if (!ctx.metadata_csum)return {};
    if (raw.size()<128)return Error{Errc::corrupt, 0,"inode buffer too short"};
    const auto c = compute_inode_checksum(ctx, inode, raw);
    write_le16(raw, INODE_CSUM_LO, static_cast<std::uint16_t>(c));
    if (inode_has_hi_checksum(raw))write_le16(raw, INODE_CSUM_HI, static_cast<std::uint16_t>(c>>16U));
    return {};
}
Result<bool> verify_extent_block_checksum(const MetadataContext& ctx,
                                          std::uint64_t inode, std::uint32_t generation,
                                          std::span<const std::byte> block) {
    if (!ctx.metadata_csum)return true;
    auto c = compute_extent_checksum(ctx, inode, generation, block);
    if (!c)return c.error();
    const auto max = read_le<std::uint16_t>(block, 4);
    const auto off = 12ULL+static_cast<std::uint64_t>(max)*12ULL;
    return read_le<std::uint32_t>(block, static_cast<std::size_t>(off)) == c.value();
}
Result<void> update_extent_block_checksum(const MetadataContext& ctx,
                                          std::uint64_t inode, std::uint32_t generation, std::span<std::byte> block) {
    if (!ctx.metadata_csum)return {};
    auto c = compute_extent_checksum(ctx, inode, generation, block);
    if (!c)return c.error();
    const auto max = read_le<std::uint16_t>(block, 4);
    const auto off = 12ULL+static_cast<std::uint64_t>(max)*12ULL;
    write_le32(block, static_cast<std::size_t>(off), c.value());
    return {};
}

Result<MetadataIntegrityReport> verify_metadata(BlockDevice& dev, bool scan_inodes, Progress* progress) {
    MetadataIntegrityReport out;
    auto raw = read_raw_superblock(dev);
    if (!raw)return raw.error();
    auto sb = read_superblock(dev);
    if (!sb)return sb.error();
    out.superblock_checksum_present = (sb.value().feature_ro_compat&RO_METADATA_CSUM) != 0;
    out.superblock_checksum_valid = verify_superblock_checksum(raw.value());
    if (!out.superblock_checksum_valid) {
        out.clean = false;
        out.errors.push_back("superblock checksum mismatch");
    }
    auto ctxr = load_metadata_context(dev);
    if (!ctxr)return ctxr.error();
    const auto ctx = ctxr.value();
    const auto groups = ctx.sb.groups_count();
    out.groups_checked = groups;
    std::uint64_t allocated_blocks = 0, allocated_inodes = 0;
    for (std::uint64_t g = 0; g<groups; ++g) {
        if (Cancellation::requested())return Error{Errc::unsafe, 0,"metadata verification cancelled"};
        auto gd_raw = read_group_descriptor_raw(dev, ctx, g);
        if (!gd_raw)return gd_raw.error();
        auto gd_ok = verify_group_descriptor_checksum(ctx, g, gd_raw.value());
        if (!gd_ok)return gd_ok.error();
        if (!gd_ok.value()) {
            ++out.group_desc_checksum_failures;
            out.errors.push_back("group "+std::to_string(g)+" descriptor checksum mismatch");
        }
        auto gd = detail::read_group_desc(dev, ctx.sb, g);
        if (!gd)return gd.error();
        auto bb = detail::read_block_bitmap(dev, ctx.sb, gd.value());
        if (!bb)return bb.error();
        auto ib = detail::read_inode_bitmap(dev, ctx.sb, gd.value());
        if (!ib)return ib.error();
        auto bbc = verify_block_bitmap_checksum(ctx, gd_raw.value(), bb.value());
        if (!bbc)return bbc.error();
        if (!bbc.value()) {
            ++out.block_bitmap_checksum_failures;
            out.errors.push_back("group "+std::to_string(g)+" block bitmap checksum mismatch");
        }
        auto ibc = verify_inode_bitmap_checksum(ctx, gd_raw.value(), ib.value());
        if (!ibc)return ibc.error();
        if (!ibc.value()) {
            ++out.inode_bitmap_checksum_failures;
            out.errors.push_back("group "+std::to_string(g)+" inode bitmap checksum mismatch");
        }
        const auto gb = detail::group_block_count(ctx.sb, g);
        const auto ba = popcount_bitmap(bb.value(), gb);
        allocated_blocks+=ba;
        const auto free_count = gb-ba;
        if (free_count != gd.value().free_blocks) {
            ++out.block_count_mismatches;
            out.errors.push_back("group "+std::to_string(g)+" free-block count mismatch");
        }
        const auto first_inode = g*static_cast<std::uint64_t>(ctx.sb.inodes_per_group)+1;
        const auto remain = ctx.sb.inodes_count >= first_inode?ctx.sb.inodes_count-first_inode+1:0;
        const auto icount = std::min<std::uint64_t>(ctx.sb.inodes_per_group, remain);
        const auto ia = popcount_bitmap(ib.value(), icount);
        allocated_inodes+=ia;
        if (icount-ia != gd.value().free_inodes) {
            ++out.inode_count_mismatches;
            out.errors.push_back("group "+std::to_string(g)+" free-inode count mismatch");
        }
        if (scan_inodes && ctx.metadata_csum && (gd.value().flags&detail::BG_INODE_UNINIT) == 0) {
            for (std::uint64_t i = 0; i<icount; ++i) {
                if (!detail::bit_set(ib.value(), i))continue;
                const auto ino = first_inode+i;
                auto inode = read_inode_raw(dev, ctx, ino);
                if (!inode)return inode.error();
                const auto mode = read_le<std::uint16_t>(inode.value(), 0);
                if (mode == 0)continue;
                auto ok = verify_inode_checksum(ctx, ino, inode.value());
                if (!ok)return ok.error();
                ++out.inode_checksums_checked;
                if (!ok.value()) {
                    ++out.inode_checksum_failures;
                    out.errors.push_back("inode "+std::to_string(ino)+" checksum mismatch");
                }
            }
        }
        if (progress)progress->update({
                                      "metadata-check",
                                      "group "+std::to_string(g+1)+"/"+std::to_string(groups), g+1,
                                      groups, 0, 0, g+1, groups
                                      }
                                     );
    }
    out.allocated_blocks_from_bitmaps = allocated_blocks;
    out.allocated_inodes_from_bitmaps = allocated_inodes;
    const auto expected_alloc = ctx.sb.blocks_count-ctx.sb.free_blocks;
    if (allocated_blocks != expected_alloc) {
        out.errors.push_back("superblock free-block count disagrees with allocation bitmaps");
        ++out.block_count_mismatches;
    }
    if (ctx.sb.inodes_count-ctx.sb.free_inodes != allocated_inodes) {
        out.errors.push_back("superblock free-inode count disagrees with inode bitmaps");
        ++out.inode_count_mismatches;
    }
    out.clean = out.errors.empty();
    return out;
}

} // namespace fsx::ext4
