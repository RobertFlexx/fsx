#include "internal.hpp"
#include "fsx/endian.hpp"
#include <algorithm>
#include <limits>

namespace fsx::ext4::detail {
std::string uuid_string(std::span<const std::byte> b) {
    static constexpr char h[] = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (std::size_t i = 0; i<16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10)out.push_back('-');
        auto v = std::to_integer<unsigned>(b[i]);
        out.push_back(h[v>>4]);
        out.push_back(h[v&15]);
    }
    return out;
}
std::string fixed_string(std::span<const std::byte> b) {
    std::string s;
    for (auto x:b) {
        char c = static_cast<char>(std::to_integer<unsigned char>(x));
        if (c == 0)break;
        s.push_back(c);
    }
    return s;
}

Result<GroupDesc> read_group_desc(BlockDevice& dev, const Superblock& sb, std::uint64_t group) {
    const auto bs = sb.block_size();
    if (bs == 0)return Error{Errc::corrupt, 0,"invalid ext4 block size"};
    if (group >= sb.groups_count())return Error{
        Errc::corrupt, 0,"ext4 group descriptor index is outside filesystem"
    }
    ;
    const std::uint32_t ds = sb.desc_size >= 32?sb.desc_size:32;
    if (ds>bs || ds>1024)return Error{Errc::corrupt, 0,"invalid ext4 group descriptor size"};
    if (group>std::numeric_limits<std::uint64_t>::max()/ds)
        return Error{Errc::corrupt, 0,"ext4 group descriptor offset overflows"};
    const auto desc_off = group*static_cast<std::uint64_t>(ds);
    const std::uint64_t table_block = (bs == 1024)?2:1;
    if ((sb.feature_incompat&INCOMPAT_META_BG) != 0 && desc_off >= bs)
        return Error{Errc::unsupported, 0,"ext4 meta_bg descriptor placement is not implemented yet"};
    if (table_block>std::numeric_limits<std::uint64_t>::max()/bs)
        return Error{Errc::corrupt, 0,"ext4 group descriptor table offset overflows"};
    const auto table_off = table_block*bs;
    if (desc_off > std::numeric_limits<std::uint64_t>::max() - table_off)
        return Error{Errc::corrupt, 0, "ext4 group descriptor address overflows"};
    std::vector<std::byte> buf(ds);
    auto r = dev.read_exact(table_off + desc_off, buf);
    if (!r)
        return r.error();
    GroupDesc g;
    std::uint64_t hi_bb = 0, hi_ib = 0, hi_it = 0;
    g.block_bitmap = read_le<std::uint32_t>(buf, 0);
    g.inode_bitmap = read_le<std::uint32_t>(buf, 4);
    g.inode_table = read_le<std::uint32_t>(buf, 8);
    g.free_blocks = read_le<std::uint16_t>(buf, 12);
    g.free_inodes = read_le<std::uint16_t>(buf, 14);
    g.used_dirs = read_le<std::uint16_t>(buf, 16);
    g.flags = read_le<std::uint16_t>(buf, 18);
    if (ds >= 64 && (sb.feature_incompat&INCOMPAT_64BIT)) {
        hi_bb = read_le<std::uint32_t>(buf, 32);
        hi_ib = read_le<std::uint32_t>(buf, 36);
        hi_it = read_le<std::uint32_t>(buf, 40);
        g.free_blocks|=static_cast<std::uint32_t>(read_le<std::uint16_t>(buf, 44))<<16U;
        g.free_inodes|=static_cast<std::uint32_t>(read_le<std::uint16_t>(buf, 46))<<16U;
        g.used_dirs|=static_cast<std::uint32_t>(read_le<std::uint16_t>(buf, 48))<<16U;
    }
    g.block_bitmap|=hi_bb<<32U;
    g.inode_bitmap|=hi_ib<<32U;
    g.inode_table|=hi_it<<32U;
    if (g.block_bitmap >= sb.blocks_count || g.inode_bitmap >= sb.blocks_count || g.inode_table >= sb.blocks_count)
        return Error{Errc::corrupt, 0,"ext4 group metadata points outside filesystem"};
    const auto inode_bytes = static_cast<std::uint64_t>(sb.inodes_per_group)*sb.inode_size;
    const auto inode_table_blocks = inode_bytes/bs+(inode_bytes%bs != 0?1ULL:0ULL);
    if (inode_table_blocks == 0 || inode_table_blocks>sb.blocks_count-g.inode_table)
        return Error{Errc::corrupt, 0,"ext4 inode table extends beyond filesystem"};
    return g;
}
Result<std::vector<std::byte>> read_block_bitmap(BlockDevice& dev, const Superblock& sb, const GroupDesc& gd) {
    const auto bs = sb.block_size();
    if (bs == 0)return Error{Errc::corrupt, 0,"invalid ext4 block size"};
    if (gd.block_bitmap >= sb.blocks_count)return Error{Errc::corrupt, 0,"block bitmap points beyond filesystem"};
    if (gd.block_bitmap>std::numeric_limits<std::uint64_t>::max()/bs)return Error{
        Errc::corrupt, 0,"block bitmap byte offset overflows"
    }
    ;
    std::vector<std::byte>b(static_cast<std::size_t>(bs));
    auto r = dev.read_exact(gd.block_bitmap*bs, b);
    if (!r)return r.error();
    return b;
}
Result<std::vector<std::byte>> read_inode_bitmap(BlockDevice& dev, const Superblock& sb, const GroupDesc& gd) {
    const auto bs = sb.block_size();
    if (bs == 0)return Error{Errc::corrupt, 0,"invalid ext4 block size"};
    if (gd.inode_bitmap >= sb.blocks_count)return Error{Errc::corrupt, 0,"inode bitmap points beyond filesystem"};
    if (gd.inode_bitmap>std::numeric_limits<std::uint64_t>::max()/bs)return Error{
        Errc::corrupt, 0,"inode bitmap byte offset overflows"
    }
    ;
    std::vector<std::byte>b(static_cast<std::size_t>(bs));
    auto r = dev.read_exact(gd.inode_bitmap*bs, b);
    if (!r)return r.error();
    return b;
}
bool bit_set(std::span<const std::byte>b, std::uint64_t bit)noexcept{
    const auto by = bit>>3U, sh = bit&7U;
    return by<b.size() && ((std::to_integer<unsigned>(b[by])>>sh)&1U);
}
std::uint64_t group_start_block(const Superblock& sb, std::uint64_t g)noexcept{
    if (sb.blocks_per_group == 0)return std::numeric_limits<std::uint64_t>::max();
    const auto first = static_cast<std::uint64_t>(sb.first_data_block);
    const auto bpg = static_cast<std::uint64_t>(sb.blocks_per_group);
    if (g>(std::numeric_limits<std::uint64_t>::max()-first)/bpg)return std::numeric_limits<std::uint64_t>::max();
    return first+g*bpg;
}
std::uint64_t group_block_count(const Superblock& sb, std::uint64_t g)noexcept{
    const auto start = group_start_block(sb, g);
    return start<sb.blocks_count?std::min<std::uint64_t>(sb.blocks_per_group, sb.blocks_count-start):0;
}
}
