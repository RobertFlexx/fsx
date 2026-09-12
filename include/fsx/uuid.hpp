#pragma once
#include "fsx/result.hpp"
#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace fsx {
using UuidBytes = std::array<std::byte, 16>;
Result<UuidBytes> parse_uuid(std::string_view text);
std::string format_uuid(const UuidBytes& uuid);
UuidBytes random_uuid_v4() noexcept;
} // namespace fsx
