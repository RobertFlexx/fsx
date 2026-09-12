#pragma once
#include "fsx/device.hpp"
#include "fsx/ext4.hpp"
#include "fsx/progress.hpp"
#include "fsx/result.hpp"
#include <cstdint>
#include <vector>

namespace fsx::ext4 {

enum class ExtentRefKind : std::uint8_t {
    inode_root = 1,
    extent_block = 2
};

struct ExtentReference {
    ExtentRefKind kind{ExtentRefKind::inode_root};
    std::uint64_t container_byte_offset{};
    std::uint64_t container_block{};
    std::uint32_t entry_offset{};
    std::uint16_t tree_depth{};
};

struct OwnedExtent {
    std::uint64_t inode{};
    std::uint64_t logical_block{};
    std::uint64_t physical_block{};
    std::uint64_t length{};
    std::uint32_t inode_generation{};
    bool unwritten{false};
    ExtentReference reference{};

    std::uint64_t end() const noexcept { return physical_block + length;
    }
};

struct ExtentTreeBlock {
    std::uint64_t inode{};
    std::uint64_t physical_block{};
    std::uint64_t parent_container_byte_offset{};
    std::uint32_t parent_entry_offset{};
    std::uint16_t depth{};
};

struct OwnerScan {
    std::uint64_t inodes_examined{};
    std::uint64_t allocated_inodes{};
    std::uint64_t extent_inodes{};
    std::uint64_t legacy_blockmap_inodes{};
    std::uint64_t corrupt_inodes{};
    std::uint64_t total_extent_records{};
    std::uint64_t retained_extent_blocks{};
    std::uint64_t retained_tree_blocks{};
    std::vector<OwnedExtent> extents;
    std::vector<ExtentTreeBlock> tree_blocks;
};

Result<OwnerScan> scan_extent_owners(BlockDevice& dev,
                                     std::uint64_t retain_from_physical_block = 0,
                                     Progress* progress = nullptr);

// Scan one inode without walking every inode bitmap. This is used by the
// transactional mutation/recovery path to reconcile journal state with the
// actual extent tree after an interrupted metadata commit.
Result<OwnerScan> scan_inode_extent_tree(BlockDevice& dev,
                                         std::uint64_t inode,
                                         std::uint64_t retain_from_physical_block = 0);

Result<OwnedExtent> find_inode_extent(BlockDevice& dev,
                                      std::uint64_t inode,
                                      std::uint64_t logical_block);

} // namespace fsx::ext4
