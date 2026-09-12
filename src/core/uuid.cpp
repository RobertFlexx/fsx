#include "fsx/uuid.hpp"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>

namespace fsx {
namespace {
int hex(char c) {
    if (c >= '0' && c <= '9')return c-'0';
    if (c >= 'a' && c <= 'f')return c-'a'+10;
    if (c >= 'A' && c <= 'F')return c-'A'+10;
    return -1;
}
}

Result<UuidBytes> parse_uuid(std::string_view s) {
    UuidBytes out{};
    std::size_t oi = 0;
    int high = -1;
    for (char c:s) {
        if (c == '-')continue;
        int v = hex(c);
        if (v<0)return Error{
            Errc::invalid_argument, 0,"invalid UUID"
        }
        ;
        if (high<0)high = v;
        else {
            if (oi >= out.size())return Error{
                Errc::invalid_argument, 0,"UUID is too long"
            }
            ;
            out[oi++] = std::byte(static_cast<unsigned char>((high<<4)|v));
            high = -1;
        }
    }
    if (oi != 16 || high >= 0)return Error{
        Errc::invalid_argument, 0,"UUID must contain 16 bytes"
    }
    ;
    return out;
}

std::string format_uuid(const UuidBytes&u) {
    static constexpr char h[] = "0123456789abcdef";
    std::string s;
    s.reserve(36);
    for (std::size_t i = 0; i<16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10)s.push_back('-');
        auto v = std::to_integer<unsigned>(u[i]);
        s.push_back(h[v>>4]);
        s.push_back(h[v&15]);
    }
    return s;
}

UuidBytes random_uuid_v4() noexcept {
    UuidBytes out{};
    bool complete = false;
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags|=O_CLOEXEC;
#endif
    const int fd = ::open("/dev/urandom", flags);
    if (fd >= 0) {
        std::size_t done = 0;
        while (done<out.size()) {
            const auto n = ::read(fd, out.data()+done, out.size()-done);
            if (n<0) {
                if (errno == EINTR)continue;
                break;
            }
            if (n == 0)break;
            done+=static_cast<std::size_t>(n);
        }
        complete = (done == out.size());
        (void)::close(fd);
    }
    if (!complete) {
        // UUID uniqueness, not secrecy, is required by FSX transaction IDs.
        // This fallback keeps recovery tooling operational if the entropy
        // device is temporarily unavailable.
        static std::atomic<std::uint64_t> counter{0};
        const auto a = static_cast<std::uint64_t>(
                                                  std::chrono::steady_clock::now().time_since_epoch().count());
        const auto b = static_cast<std::uint64_t>(
                                                  std::chrono::system_clock::now().time_since_epoch().count());
        const auto c = static_cast<std::uint64_t>(static_cast<unsigned long>(::getpid()));
        const auto d = counter.fetch_add(1, std::memory_order_relaxed)+1U;
        const auto x = a^(c<<17U)^(d*0x9e3779b97f4a7c15ULL);
        const auto y = b^(c<<31U)^(d*0xbf58476d1ce4e5b9ULL);
        for (unsigned i = 0; i<8; ++i)out[i] = std::byte((x>>(i*8U))&0xffU);
        for (unsigned i = 0; i<8; ++i)out[8U+i] = std::byte((y>>(i*8U))&0xffU);
    }
    out[6] = std::byte((std::to_integer<unsigned char>(out[6])&0x0fU)|0x40U);
    out[8] = std::byte((std::to_integer<unsigned char>(out[8])&0x3fU)|0x80U);
    return out;
}

} // namespace fsx
