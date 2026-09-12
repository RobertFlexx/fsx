#include "fsx/hash.hpp"
#include <array>
#include <cstring>

namespace fsx {
namespace {
constexpr std::uint64_t XXH_PRIME64_1 = 11400714785074694791ULL;
constexpr std::uint64_t XXH_PRIME64_2 = 14029467366897019727ULL;
constexpr std::uint64_t XXH_PRIME64_3 = 1609587929392839161ULL;
constexpr std::uint64_t XXH_PRIME64_4 = 9650029242287828579ULL;
constexpr std::uint64_t XXH_PRIME64_5 = 2870177450012600261ULL;

constexpr std::uint64_t rotl64(std::uint64_t x, unsigned r) noexcept {
    return (x << r) | (x >> (64U-r));
}
constexpr std::uint32_t rotr32(std::uint32_t x, unsigned r) noexcept {
    return (x >> r) | (x << (32U-r));
}
constexpr std::uint64_t rotr64(std::uint64_t x, unsigned r) noexcept {
    return (x >> r) | (x << (64U-r));
}

std::uint64_t read64le(const std::byte* p) noexcept {
    std::uint64_t v{};
    for (unsigned i = 0; i<8; ++i) v |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(p[i])) << (i*8U);
    return v;
}
std::uint32_t read32le(const std::byte* p) noexcept {
    std::uint32_t v{};
    for (unsigned i = 0; i<4; ++i) v |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[i])) << (i*8U);
    return v;
}
std::uint32_t read32be(const std::byte* p) noexcept {
    return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[0]))<<24U) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[1]))<<16U) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[2]))<<8U) |
        static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[3]));
}
void write32be(std::byte* p, std::uint32_t v) noexcept {
    p[0] = std::byte((v>>24U)&0xffU);
    p[1] = std::byte((v>>16U)&0xffU);
    p[2] = std::byte((v>>8U)&0xffU);
    p[3] = std::byte(v&0xffU);
}
void write64le(std::byte* p, std::uint64_t v) noexcept {
    for (unsigned i = 0; i<8; ++i)p[i] = std::byte((v>>(i*8U))&0xffU);
}
std::uint64_t xxh_round(std::uint64_t acc, std::uint64_t input) noexcept {
    acc += input * XXH_PRIME64_2;
    acc = rotl64(acc, 31);
    acc*=XXH_PRIME64_1;
    return acc;
}
std::uint64_t xxh_merge(std::uint64_t acc, std::uint64_t val) noexcept {
    val = xxh_round(0, val);
    acc^=val;
    return acc*XXH_PRIME64_1+XXH_PRIME64_4;
}

constexpr std::array<std::uint32_t, 64> K256{{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U}};

void sha256_compress(std::array<std::uint32_t, 8>& h, const std::byte* block) noexcept {
    std::array<std::uint32_t, 64>w{};
    for (unsigned i = 0; i<16; ++i)w[i] = read32be(block+i*4U);
    for (unsigned i = 16; i<64; ++i) {
        const auto s0 = rotr32(w[i-15], 7)^rotr32(w[i-15], 18)^(w[i-15]>>3U);
        const auto s1 = rotr32(w[i-2], 17)^rotr32(w[i-2], 19)^(w[i-2]>>10U);
        w[i] = w[i-16]+s0+w[i-7]+s1;
    }
    auto a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (unsigned i = 0; i<64; ++i) {
        const auto S1 = rotr32(e, 6)^rotr32(e, 11)^rotr32(e, 25);
        const auto ch = (e&f)^((~e)&g);
        const auto t1 = hh+S1+ch+K256[i]+w[i];
        const auto S0 = rotr32(a, 2)^rotr32(a, 13)^rotr32(a, 22);
        const auto maj = (a&b)^(a&c)^(b&c);
        const auto t2 = S0+maj;
        hh = g;
        g = f;
        f = e;
        e = d+t1;
        d = c;
        c = b;
        b = a;
        a = t1+t2;
    }
    h[0]+=a;
    h[1]+=b;
    h[2]+=c;
    h[3]+=d;
    h[4]+=e;
    h[5]+=f;
    h[6]+=g;
    h[7]+=hh;
}

constexpr std::array<std::uint64_t, 8> B2IV{{
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
        0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL}};
constexpr std::uint8_t SIGMA[12][16] = {{
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
          {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4}, {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4,
                                                                      0, 15, 8},
          {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13}, {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15,
                                                                      14, 1, 9},
          {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11}, {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8,
                                                                      6, 2, 10},
          {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5}, {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3,
                                                                      12, 13, 0},
          {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2,
                                                                      11, 7, 5, 3}};

void blake2b_compress(std::array<std::uint64_t, 8>& h, const std::byte* block,
                      std::uint64_t t0, std::uint64_t t1, bool last) noexcept {
    std::array<std::uint64_t, 16> m{}, v{};
    for (unsigned i = 0; i<16; ++i)m[i] = read64le(block+i*8U);
    for (unsigned i = 0; i<8; ++i) {
        v[i] = h[i];
        v[i+8] = B2IV[i];
    }
    v[12]^=t0;
    v[13]^=t1;
    if (last)v[14] = ~v[14];
    auto G = [&](unsigned a, unsigned b, unsigned c, unsigned d, std::uint64_t x, std::uint64_t y) {
        v[a] = v[a]+v[b]+x;
        v[d] = rotr64(v[d]^v[a], 32);
        v[c]+=v[d];
        v[b] = rotr64(v[b]^v[c], 24);
        v[a] = v[a]+v[b]+y;
        v[d] = rotr64(v[d]^v[a], 16);
        v[c]+=v[d];
        v[b] = rotr64(v[b]^v[c], 63);
    };
    for (unsigned r = 0; r<12; ++r) {
        const auto*s = SIGMA[r];
        G(0, 4, 8, 12, m[s[0]], m[s[1]]);
        G(1, 5, 9, 13, m[s[2]], m[s[3]]);
        G(2, 6, 10, 14, m[s[4]], m[s[5]]);
        G(3, 7, 11, 15, m[s[6]], m[s[7]]);
        G(0, 5, 10, 15, m[s[8]], m[s[9]]);
        G(1, 6, 11, 12, m[s[10]], m[s[11]]);
        G(2, 7, 8, 13, m[s[12]], m[s[13]]);
        G(3, 4, 9, 14, m[s[14]], m[s[15]]);
    }
    for (unsigned i = 0; i<8; ++i)h[i]^=v[i]^v[i+8];
}
}

std::uint64_t xxhash64(std::span<const std::byte> data, std::uint64_t seed) noexcept {
    // Keep the cursor length-based instead of constructing an end pointer.
    // A default-constructed/empty std::span is permitted to expose nullptr as
    // data(), and even pointer arithmetic/comparison on that null pointer is
    // undefined behaviour despite there being zero bytes to access.
    const auto* p = data.data();
    std::size_t remaining = data.size();
    std::uint64_t h{};

    if (remaining >= 32) {
        std::uint64_t v1 = seed+XXH_PRIME64_1+XXH_PRIME64_2;
        std::uint64_t v2 = seed+XXH_PRIME64_2;
        std::uint64_t v3 = seed;
        std::uint64_t v4 = seed-XXH_PRIME64_1;
        do {
            v1 = xxh_round(v1, read64le(p));
            p+=8;
            v2 = xxh_round(v2, read64le(p));
            p+=8;
            v3 = xxh_round(v3, read64le(p));
            p+=8;
            v4 = xxh_round(v4, read64le(p));
            p+=8;
            remaining-=32;
        } while (remaining >= 32);
        h = rotl64(v1, 1)+rotl64(v2, 7)+rotl64(v3, 12)+rotl64(v4, 18);
        h = xxh_merge(h, v1);
        h = xxh_merge(h, v2);
        h = xxh_merge(h, v3);
        h = xxh_merge(h, v4);
    } else {
        h = seed+XXH_PRIME64_5;
    }

    h+=data.size();
    while (remaining >= 8) {
        const auto k = xxh_round(0, read64le(p));
        h^=k;
        h = rotl64(h, 27)*XXH_PRIME64_1+XXH_PRIME64_4;
        p+=8;
        remaining-=8;
    }
    if (remaining >= 4) {
        h^=static_cast<std::uint64_t>(read32le(p))*XXH_PRIME64_1;
        h = rotl64(h, 23)*XXH_PRIME64_2+XXH_PRIME64_3;
        p+=4;
        remaining-=4;
    }
    while (remaining != 0) {
        h^=static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(*p))*XXH_PRIME64_5;
        h = rotl64(h, 11)*XXH_PRIME64_1;
        ++p;
        --remaining;
    }

    h^=h>>33;
    h*=XXH_PRIME64_2;
    h^=h>>29;
    h*=XXH_PRIME64_3;
    h^=h>>32;
    return h;
}

std::array<std::byte, 32> sha256(std::span<const std::byte> data) noexcept {
    std::array<std::uint32_t, 8> h{
        {
            0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
        }
    }
    ;
    const auto* p = data.data();
    std::size_t n = data.size();
    while (n >= 64) {
        sha256_compress(h, p);
        p+=64;
        n-=64;
    }
    std::array<std::byte, 128> tail{};
    if (n)std::memcpy(tail.data(), p, n);
    tail[n] = std::byte{
        0x80
    }
    ;
    const std::size_t used = (n<56)?64:128;
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size())*8ULL;
    for (unsigned i = 0; i<8; ++i)tail[used-1U-i] = std::byte((bits>>(i*8U))&0xffU);
    sha256_compress(h, tail.data());
    if (used == 128)sha256_compress(h, tail.data()+64);
    std::array<std::byte, 32> out{};
    for (unsigned i = 0; i<8; ++i)write32be(out.data()+i*4U, h[i]);
    return out;
}

std::array<std::byte, 32> blake2b_256(std::span<const std::byte> data) noexcept {
    auto h = B2IV;
    h[0]^=0x01010020ULL;
    std::uint64_t t0 = 0, t1 = 0;
    const auto* p = data.data();
    std::size_t n = data.size();
    while (n>128) {
        const auto old = t0;
        t0+=128;
        if (t0<old)++t1;
        blake2b_compress(h, p, t0, t1, false);
        p+=128;
        n-=128;
    }
    std::array<std::byte, 128> last{};
    if (n)std::memcpy(last.data(), p, n);
    const auto old = t0;
    t0+=n;
    if (t0<old)++t1;
    blake2b_compress(h, last.data(), t0, t1, true);
    std::array<std::byte, 32> out{};
    for (unsigned i = 0; i<4; ++i)write64le(out.data()+i*8U, h[i]);
    return out;
}

} // namespace fsx
