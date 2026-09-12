#pragma once
#include <atomic>

namespace fsx {

class Cancellation {
public:
    static void install_signal_handlers();
    static void request() noexcept;
    static void reset() noexcept;
    static bool requested() noexcept;

private:
    static std::atomic<bool> requested_;
};

} // namespace fsx
