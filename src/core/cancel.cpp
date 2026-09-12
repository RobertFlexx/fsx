#include "fsx/cancel.hpp"
#include <csignal>

namespace fsx {
std::atomic<bool> Cancellation::requested_{false};

namespace {
extern "C" void fsx_signal_handler(int) {
    Cancellation::request();
}
}

void Cancellation::install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = fsx_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    (void)::sigaction(SIGINT, &sa, nullptr);
    (void)::sigaction(SIGTERM, &sa, nullptr);
#ifdef SIGHUP
    (void)::sigaction(SIGHUP, &sa, nullptr);
#endif
}
void Cancellation::request() noexcept { requested_.store(true, std::memory_order_relaxed);
}
void Cancellation::reset() noexcept { requested_.store(false, std::memory_order_relaxed);
}
bool Cancellation::requested() noexcept { return requested_.load(std::memory_order_relaxed);
}
}
