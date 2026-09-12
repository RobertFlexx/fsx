#include "fsx/error.hpp"
#include <cerrno>

namespace fsx {
std::string_view errc_name(Errc code) noexcept {
    switch (code) {
    case Errc::ok: return "ok";
    case Errc::usage: return "usage";
    case Errc::invalid_argument: return "invalid_argument";
    case Errc::open_failed: return "open_failed";
    case Errc::read_failed: return "read_failed";
    case Errc::write_failed: return "write_failed";
    case Errc::flush_failed: return "flush_failed";
    case Errc::short_io: return "short_io";
    case Errc::unsupported: return "unsupported";
    case Errc::corrupt: return "corrupt";
    case Errc::unsafe: return "unsafe";
    case Errc::not_found: return "not_found";
    case Errc::permission: return "permission";
    case Errc::io: return "io";
    case Errc::internal: return "internal";
    }
    return "unknown";
}
Error from_errno(Errc code, std::string message, int e) {
    return Error{code, e, std::move(message)};
}
}
