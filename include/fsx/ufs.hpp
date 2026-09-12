#pragma once
#include "fsx/device.hpp"
#include "fsx/result.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace fsx::ufs {

enum class Version { ufs1, ufs2 };
enum class ByteOrder { little, big };

struct Superblock {
    Version version{Version::ufs1};
    ByteOrder byte_order{ByteOrder::little};
    std::uint64_t offset{};
    std::uint32_t cylinder_groups{};
    std::uint32_t block_size{};
    std::uint32_t fragment_size{};
    std::uint32_t fragments_per_block{};
    std::uint32_t superblock_size{};
    std::uint32_t inodes_per_block{};
    std::uint32_t inodes_per_group{};
    std::uint32_t fragments_per_group{};
    std::uint32_t old_size_fragments{};
    std::uint8_t clean_flags{};
};

struct CheckReport {
    bool clean{true};
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

const char* version_name(Version) noexcept;
Result<bool> probe(BlockDevice& dev);
Result<Superblock> read_superblock(BlockDevice& dev);
Result<CheckReport> check(BlockDevice& dev);

} // namespace fsx::ufs
