#pragma once
#include "fsx/result.hpp"
#include <cstdint>
#include <string>
#include <string_view>

namespace fsx {
Result<std::uint64_t> parse_size(std::string_view text);
std::string human_bytes(std::uint64_t bytes, bool binary = true);
std::string human_rate(double bytes_per_sec);
std::string format_duration(double seconds);
}
