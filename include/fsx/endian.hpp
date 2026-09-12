#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace fsx {

template <class T>
    T read_le(std::span<const std::byte> data, std::size_t off) {
        static_assert(std::is_unsigned_v<T>);
        std::uint64_t wide = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            wide |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(data[off + i])) << (i * 8U);
        }
        return static_cast<T>(wide);
    }

template <class T>
    T read_be(std::span<const std::byte> data, std::size_t off) {
        static_assert(std::is_unsigned_v<T>);
        std::uint64_t wide = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            wide = (wide << 8U) | static_cast<std::uint64_t>(std::to_integer<unsigned char>(data[off + i]));
        }
        return static_cast<T>(wide);
    }

inline void write_le16(std::span<std::byte> data, std::size_t off, std::uint16_t v) {
    for (std::size_t i = 0; i < 2; ++i) data[off+i] = std::byte((v >> (i*8U)) & 0xffU);
}
inline void write_le32(std::span<std::byte> data, std::size_t off, std::uint32_t v) {
    for (std::size_t i = 0; i < 4; ++i) data[off+i] = std::byte((v >> (i*8U)) & 0xffU);
}
inline void write_le64(std::span<std::byte> data, std::size_t off, std::uint64_t v) {
    for (std::size_t i = 0; i < 8; ++i) data[off+i] = std::byte((v >> (i*8U)) & 0xffU);
}

} // namespace fsx
