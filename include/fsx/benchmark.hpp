#pragma once
#include "fsx/device.hpp"
#include "fsx/progress.hpp"
#include "fsx/result.hpp"
#include <cstdint>

namespace fsx {
struct BenchmarkResult {
    std::uint64_t bytes_read{};
    double seconds{};
    double bytes_per_second{};
};
Result<BenchmarkResult> benchmark_read(BlockDevice& dev, std::uint64_t bytes, Progress* progress = nullptr);
}
