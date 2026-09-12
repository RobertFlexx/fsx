#pragma once
#include "fsx/device.hpp"
#include "fsx/result.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace fsx {

enum class FilesystemKind {
    unknown,
    ext,
    fat12,
    fat16,
    fat32,
    exfat,
    ufs1,
    ufs2,
    xfs,
    btrfs,
    ntfs,
    iso9660
};

struct FilesystemProbe {
    FilesystemKind kind{FilesystemKind::unknown};
    std::string name{"unknown"};
    std::string label;
    std::string uuid;
    std::uint64_t size_bytes{};
    std::uint32_t block_size{};
    bool confident{false};
    std::vector<std::string> notes;
};

const char* filesystem_kind_name(FilesystemKind kind) noexcept;
Result<FilesystemProbe> probe_filesystem(BlockDevice& dev);

} // namespace fsx
