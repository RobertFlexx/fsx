#pragma once
#include "fsx/device.hpp"
#include "fsx/ext4.hpp"
#include "fsx/progress.hpp"
#include "fsx/result.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fsx::ext4 {

struct MetadataContext {
    Superblock sb{};
    std::array<std::byte, 16> uuid{};
    std::uint32_t checksum_seed{};
    bool metadata_csum{false};
    bool gdt_csum{false};
    bool csum_seed_feature{false};
};

struct MetadataIntegrityReport {
    bool clean{true};
    bool superblock_checksum_present{false};
    bool superblock_checksum_valid{true};
    std::uint64_t groups_checked{};
    std::uint64_t group_desc_checksum_failures{};
    std::uint64_t block_bitmap_checksum_failures{};
    std::uint64_t inode_bitmap_checksum_failures{};
    std::uint64_t block_count_mismatches{};
    std::uint64_t inode_count_mismatches{};
    std::uint64_t inode_checksums_checked{};
    std::uint64_t inode_checksum_failures{};
    std::uint64_t extent_blocks_checked{};
    std::uint64_t extent_block_checksum_failures{};
    std::uint64_t allocated_blocks_from_bitmaps{};
    std::uint64_t allocated_inodes_from_bitmaps{};
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

Result<MetadataContext> load_metadata_context(BlockDevice& dev,
                                              bool tolerate_bad_superblock_checksum = false);
Result<MetadataIntegrityReport> verify_metadata(BlockDevice& dev,
                                                bool scan_inodes = true,
                                                Progress* progress = nullptr);

Result<std::vector<std::byte>> read_raw_superblock(BlockDevice& dev);
Result<void> write_raw_superblock(BlockDevice& dev, std::span<const std::byte> raw);
bool verify_superblock_checksum(std::span<const std::byte> raw) noexcept;
Result<void> update_superblock_checksum(std::span<std::byte> raw);

Result<std::vector<std::byte>> read_group_descriptor_raw(BlockDevice& dev,
                                                         const MetadataContext& ctx,
                                                         std::uint64_t group);
Result<void> write_group_descriptor_raw(BlockDevice& dev,
                                        const MetadataContext& ctx,
                                        std::uint64_t group,
                                        std::span<const std::byte> raw);
Result<bool> verify_group_descriptor_checksum(const MetadataContext& ctx,
                                              std::uint64_t group,
                                              std::span<const std::byte> raw);
Result<void> update_group_descriptor_checksum(const MetadataContext& ctx,
                                              std::uint64_t group,
                                              std::span<std::byte> raw);

Result<bool> verify_block_bitmap_checksum(const MetadataContext& ctx,
                                          std::span<const std::byte> group_desc,
                                          std::span<const std::byte> bitmap);
Result<bool> verify_inode_bitmap_checksum(const MetadataContext& ctx,
                                          std::span<const std::byte> group_desc,
                                          std::span<const std::byte> bitmap);
Result<void> update_block_bitmap_checksum(const MetadataContext& ctx,
                                          std::span<std::byte> group_desc,
                                          std::span<const std::byte> bitmap);
Result<void> update_inode_bitmap_checksum(const MetadataContext& ctx,
                                          std::span<std::byte> group_desc,
                                          std::span<const std::byte> bitmap);

Result<std::vector<std::byte>> read_inode_raw(BlockDevice& dev,
                                              const MetadataContext& ctx,
                                              std::uint64_t inode);
Result<void> write_inode_raw(BlockDevice& dev,
                             const MetadataContext& ctx,
                             std::uint64_t inode,
                             std::span<const std::byte> raw);
Result<bool> verify_inode_checksum(const MetadataContext& ctx,
                                   std::uint64_t inode,
                                   std::span<const std::byte> raw);
Result<void> update_inode_checksum(const MetadataContext& ctx,
                                   std::uint64_t inode,
                                   std::span<std::byte> raw);

Result<bool> verify_extent_block_checksum(const MetadataContext& ctx,
                                          std::uint64_t inode,
                                          std::uint32_t generation,
                                          std::span<const std::byte> block);
Result<void> update_extent_block_checksum(const MetadataContext& ctx,
                                          std::uint64_t inode,
                                          std::uint32_t generation,
                                          std::span<std::byte> block);

} // namespace fsx::ext4
