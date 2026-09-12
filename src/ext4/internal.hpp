#pragma once
#include "fsx/ext4.hpp"
#include <cstdint>
#include <span>
#include <vector>

namespace fsx::ext4::detail {

constexpr std::uint16_t EXT4_MAGIC = 0xEF53;
constexpr std::uint32_t INCOMPAT_RECOVER = 0x04;
constexpr std::uint32_t INCOMPAT_META_BG = 0x10;
constexpr std::uint32_t INCOMPAT_64BIT = 0x80;
constexpr std::uint32_t RO_COMPAT_BIGALLOC = 0x200;
constexpr std::uint32_t RO_COMPAT_METADATA_CSUM = 0x400;

struct GroupDesc {
    std::uint64_t block_bitmap{};
    std::uint64_t inode_bitmap{};
    std::uint64_t inode_table{};
    std::uint32_t free_blocks{};
    std::uint32_t free_inodes{};
    std::uint32_t used_dirs{};
    std::uint16_t flags{};
};

constexpr std::uint16_t BG_INODE_UNINIT = 0x0001;
constexpr std::uint16_t BG_BLOCK_UNINIT = 0x0002;

Result<GroupDesc> read_group_desc(BlockDevice& dev, const Superblock& sb, std::uint64_t group);
Result<std::vector<std::byte>> read_block_bitmap(BlockDevice& dev, const Superblock& sb, const GroupDesc& gd);
Result<std::vector<std::byte>> read_inode_bitmap(BlockDevice& dev, const Superblock& sb, const GroupDesc& gd);
bool bit_set(std::span<const std::byte> bitmap, std::uint64_t bit) noexcept;
std::uint64_t group_start_block(const Superblock& sb, std::uint64_t group) noexcept;
std::uint64_t group_block_count(const Superblock& sb, std::uint64_t group) noexcept;
std::string uuid_string(std::span<const std::byte> b);
std::string fixed_string(std::span<const std::byte> b);

} // namespace fsx::ext4::detail
