#include "fsx/size.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace fsx {
Result<std::uint64_t> parse_size(std::string_view text) {
    if (text.empty()) return Error{Errc::invalid_argument, 0, "empty size"};
    std::string s(text);
    s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) {
                           return std::isspace(c); }), s.end());
    std::size_t pos = 0;
    long double n = 0;
    try {
        n = std::stold(s, &pos);
    }
    catch (...) {
        return Error{Errc::invalid_argument, 0, "invalid size: " + s};
    }
    if (!(n >= 0.0L)) return Error{Errc::invalid_argument, 0, "negative size"};
    std::string suffix = s.substr(pos);
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) {
                   return static_cast<char>(std::toupper(c));
                   }
                  );
    long double mul = 1;
    if (suffix.empty() || suffix == "B") mul = 1;
    else if (suffix == "K" || suffix == "KB") mul = 1000.0L;
    else if (suffix == "M" || suffix == "MB") mul = 1000.0L * 1000.0L;
    else if (suffix == "G" || suffix == "GB") mul = 1000.0L * 1000.0L * 1000.0L;
    else if (suffix == "T" || suffix == "TB") mul = 1000.0L * 1000.0L * 1000.0L * 1000.0L;
    else if (suffix == "KIB") mul = 1024.0L;
    else if (suffix == "MIB") mul = 1024.0L * 1024.0L;
    else if (suffix == "GIB") mul = 1024.0L * 1024.0L * 1024.0L;
    else if (suffix == "TIB") mul = 1024.0L * 1024.0L * 1024.0L * 1024.0L;
    else return Error{Errc::invalid_argument, 0, "unknown size suffix: " + suffix};
    long double v = n * mul;
    if (v > static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))
        return Error{Errc::invalid_argument, 0, "size overflow"};
    return static_cast<std::uint64_t>(v);
}

std::string human_bytes(std::uint64_t bytes, bool binary) {
    static const char* bi[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    static const char* si[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    const double base = binary ? 1024.0 : 1000.0;
    double v = static_cast<double>(bytes);
    int unit = 0;
    while (v >= base && unit < 5) {
        v /= base;
        ++unit;
    }
    std::ostringstream o;
    o << std::fixed << std::setprecision(unit == 0 ? 0 : (v < 10 ? 2 : 1))
        << v << ' ' << (binary ? bi[unit] : si[unit]);
    return o.str();
}
std::string human_rate(double bps) {
    if (!(bps >= 0)) bps = 0;
    return human_bytes(static_cast<std::uint64_t>(bps)) + "/s";
}
std::string format_duration(double seconds) {
    if (!(seconds >= 0)) seconds = 0;
    auto s = static_cast<std::uint64_t>(seconds);
    auto h = s / 3600;
    s %= 3600;
    auto m = s / 60;
    s %= 60;
    std::ostringstream o;
    if (h) o << std::setw(2) << std::setfill('0') << h << ':';
    o << std::setw(2) << std::setfill('0') << m << ':' << std::setw(2) << s;
    return o.str();
}
}
