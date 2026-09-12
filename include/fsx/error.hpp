#pragma once
#include <string>
#include <string_view>

namespace fsx {

enum class Errc {
    ok = 0,
    usage,
    invalid_argument,
    open_failed,
    read_failed,
    write_failed,
    flush_failed,
    short_io,
    unsupported,
    corrupt,
    unsafe,
    not_found,
    permission,
    io,
    internal
};

struct Error {
    Errc code{Errc::ok};
    int sys_errno{0};
    std::string message;

    explicit operator bool() const noexcept { return code != Errc::ok;
    }
};

std::string_view errc_name(Errc code) noexcept;
Error from_errno(Errc code, std::string message, int e);

} // namespace fsx
