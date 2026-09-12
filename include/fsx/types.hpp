#pragma once
#include <cstdint>
#include <compare>

namespace fsx {

struct Bytes {
    std::uint64_t value{};
    auto operator <= >(const Bytes&) const = default;
};
struct BlockNo {
    std::uint64_t value{};
    auto operator <= >(const BlockNo&) const = default;
};
struct SectorNo {
    std::uint64_t value{};
    auto operator <= >(const SectorNo&) const = default;
};
struct InodeNo {
    std::uint64_t value{};
    auto operator <= >(const InodeNo&) const = default;
};

} // namespace fsx
