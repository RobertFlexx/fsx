#pragma once
#include "fsx/device.hpp"
#include "fsx/progress.hpp"
#include "fsx/result.hpp"
#include <cstddef>
#include <cstdint>

namespace fsx {

struct ScrubOptions {
    std::uint64_t bytes{0}; // 0 = whole device
    std::size_t chunk_bytes{0}; // 0 = device profile default
    std::uint32_t passes{1};
};

struct ScrubReport {
    std::uint64_t bytes_per_pass{};
    std::uint64_t total_bytes_read{};
    std::uint64_t read_operations{};
    std::uint32_t passes_completed{};
    std::uint32_t crc32c{};
    double seconds{};
    double bytes_per_second{};
    bool stable{true};
};

Result<ScrubReport> scrub_read(BlockDevice& dev, const ScrubOptions& options = {
                               }
                               , Progress* progress = nullptr);

} // namespace fsx
