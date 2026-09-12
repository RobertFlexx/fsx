#include "fsx/crc.hpp"
#include <array>
#include <cstring>

#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
#include <nmmintrin.h>
#define FSX_X86_HW_CRC32C 1
#endif

namespace fsx {
namespace {
template <std::uint32_t Poly>
    std::array<std::array<std::uint32_t, 256>, 8> make_tables() {
        std::array<std::array<std::uint32_t, 256>, 8> t{};
        for (std::uint32_t i = 0; i<256; ++i) {
            std::uint32_t c = i;
            for (int j = 0; j<8; ++j) c = (c & 1U) ? (Poly ^ (c >> 1U)) : (c >> 1U);
            t[0][i] = c;
        }
        for (std::size_t n = 1; n<t.size(); ++n)
            for (std::size_t i = 0; i<256; ++i) {
                const auto c = t[n-1][i];
                t[n][i] = t[0][c&0xffU]^(c>>8U);
            }
        return t;
    }
const auto ieee = make_tables<0xedb88320U>();
const auto castagnoli = make_tables<0x82f63b78U>();

std::uint64_t load_le64(const std::byte* p) noexcept {
    std::uint64_t v = 0;
    for (unsigned i = 0; i<8; ++i) v|=static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(p[i]))<<(8U*i);
    return v;
}

std::uint32_t run(std::span<const std::byte> data, std::uint32_t seed,
                  const std::array<std::array<std::uint32_t, 256>, 8>& t) {
    std::uint32_t c = seed;
    const auto* p = data.data();
    std::size_t n = data.size();
    while (n >= 8) {
        const auto q = load_le64(p)^static_cast<std::uint64_t>(c);
        c = t[7][q&0xffU]^t[6][(q>>8U)&0xffU]^t[5][(q>>16U)&0xffU]^t[4][(q>>24U)&0xffU]^
            t[3][(q>>32U)&0xffU]^t[2][(q>>40U)&0xffU]^t[1][(q>>48U)&0xffU]^t[0][(q>>56U)&0xffU];
        p+=8;
        n-=8;
    }
    while (n--) {
        c = t[0][(c^std::to_integer<std::uint8_t>(*p++))&0xffU]^(c>>8U);
    }
    return c;
}

#if defined(FSX_X86_HW_CRC32C)
__attribute__((target("sse4.2")))
std::uint32_t crc32c_sse42(std::span<const std::byte> data, std::uint32_t seed) {
    const auto* p = reinterpret_cast<const unsigned char*>(data.data());
    std::size_t n = data.size();
#if defined(__x86_64__)
    std::uint64_t c = seed;
    while (n >= 8) {
        std::uint64_t v{};
        std::memcpy(&v, p, sizeof(v));
        c = _mm_crc32_u64(c, v);
        p += 8;
        n -= 8;
    }
    std::uint32_t c32 = static_cast<std::uint32_t>(c);
#else
    std::uint32_t c32 = seed;
#endif
    while (n >= 4) {
        std::uint32_t v{};
        std::memcpy(&v, p, sizeof(v));
        c32 = _mm_crc32_u32(c32, v);
        p += 4;
        n -= 4;
    }
    if (n >= 2) {
        std::uint16_t v{};
        std::memcpy(&v, p, sizeof(v));
        c32 = _mm_crc32_u16(c32, v);
        p += 2;
        n -= 2;
    }
    if (n) c32 = _mm_crc32_u8(c32, *p);
    return c32;
}

bool have_sse42() noexcept {
    static const bool yes = [] {
#if defined(__GNUC__) || defined(__clang__)
        __builtin_cpu_init();
        return __builtin_cpu_supports("sse4.2");
#else
        return false;
#endif
    }();
    return yes;
}
#endif
}

std::uint32_t crc32_ieee(std::span<const std::byte> data, std::uint32_t seed) {
    return run(data, seed, ieee);
}

std::uint32_t crc32c(std::span<const std::byte> data, std::uint32_t seed) {
#if defined(FSX_X86_HW_CRC32C)
    if (have_sse42()) return crc32c_sse42(data, seed);
#endif
    return run(data, seed, castagnoli);
}
}
