#pragma once
#include "fsx/device.hpp"
#include "fsx/progress.hpp"
#include "fsx/result.hpp"
#include <cstdint>
#include <string>

namespace fsx::image {

struct CreateOptions {
    std::uint32_t chunk_bytes{4U * 1024U * 1024U};
    bool sparse_zero_chunks{true};
};

struct ImageInfo {
    std::uint64_t source_bytes{};
    std::uint32_t logical_sector{};
    std::uint32_t chunk_bytes{};
    std::uint64_t chunks{};
    std::uint64_t data_chunks{};
    std::uint64_t zero_chunks{};
    std::uint64_t stored_payload_bytes{};
    bool complete{false};
};

Result<ImageInfo> create(BlockDevice& source,
                         const std::string& image_path,
                         const CreateOptions& options = {},
                         Progress* progress = nullptr);
Result<ImageInfo> verify(const std::string& image_path, Progress* progress = nullptr);
Result<ImageInfo> inspect(const std::string& image_path);
Result<ImageInfo> restore(const std::string& image_path,
                          BlockDevice& target,
                          bool allow_block_device,
                          Progress* progress = nullptr);

} // namespace fsx::image
