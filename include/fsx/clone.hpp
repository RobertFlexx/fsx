#pragma once
#include "fsx/device.hpp"
#include "fsx/progress.hpp"
#include "fsx/result.hpp"
#include <cstddef>
#include <cstdint>

namespace fsx {

struct CloneOptions {
    std::size_t chunk_bytes{0};
    bool verify_after{true};
    bool allow_block_device{false};
    bool allow_live_source{false};
};

struct CloneReport {
    std::uint64_t bytes_copied{};
    std::uint64_t bytes_verified{};
    std::uint64_t chunks{};
    double copy_seconds{};
    double copy_bytes_per_second{};
    bool verified{false};
};

Result<CloneReport> clone_device(BlockDevice& source, BlockDevice& target,
                                 const CloneOptions& options = {},
                                 Progress* progress = nullptr);

} // namespace fsx
