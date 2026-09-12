#include "fsx/fault.hpp"
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <unistd.h>

namespace fsx::fault {
namespace {
struct State {
    std::mutex mu;
    Rule rule{};
    std::atomic<bool> active{false};
    std::array<std::atomic<std::uint64_t>, 4> counts{};
};
State& state() {
    static State s;
    return s;
}
std::size_t idx(Operation op) noexcept { return static_cast<std::size_t>(op);
}
}

void install(Rule rule) noexcept {
    auto& s = state();
    std::lock_guard lock(s.mu);
    if (rule.hit == 0) rule.hit = 1;
    s.rule = rule;
    for (auto& c : s.counts) c.store(0, std::memory_order_relaxed);
    s.active.store(true, std::memory_order_release);
}

void reset() noexcept {
    auto& s = state();
    std::lock_guard lock(s.mu);
    s.active.store(false, std::memory_order_release);
    for (auto& c : s.counts) c.store(0, std::memory_order_relaxed);
}

bool enabled() noexcept { return state().active.load(std::memory_order_acquire);
}

std::uint64_t hits(Operation operation) noexcept {
    return state().counts[idx(operation)].load(std::memory_order_relaxed);
}

Result<void> point(Operation operation, Timing timing) {
    auto& s = state();
    if (!s.active.load(std::memory_order_acquire)) return {};

    std::lock_guard lock(s.mu);
    if (!s.active.load(std::memory_order_relaxed)) return {};
    if (s.rule.operation != operation || s.rule.timing != timing) return {};
    const auto n = s.counts[idx(operation)].fetch_add(1, std::memory_order_relaxed) + 1;
    if (n != s.rule.hit) return {};
    const auto action = s.rule.action;
    if (s.rule.one_shot) s.active.store(false, std::memory_order_release);
    if (action == Action::terminate_process) ::_exit(200);
    const char* opname = operation == Operation::read ? "read" : operation == Operation::write ? "write" : "flush";
    const char* when = timing == Timing::before ? "before" : "after";
    return Error{
        Errc::io, 0, std::string("fault injection: ") + opname + " " + when + " operation " + std::to_string(n)
    }
    ;
}

} // namespace fsx::fault
