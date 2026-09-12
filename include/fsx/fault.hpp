#pragma once
#include "fsx/result.hpp"
#include <cstdint>

namespace fsx::fault {

enum class Operation : std::uint8_t {
    read = 1,
    write = 2,
    flush = 3
};

enum class Timing : std::uint8_t {
    before = 1,
    after = 2
};

enum class Action : std::uint8_t {
    return_error = 1,
    terminate_process = 2
};

struct Rule {
    Operation operation{Operation::write};
    Timing timing{Timing::before};
    std::uint64_t hit{1};
    bool one_shot{true};
    Action action{Action::return_error};
};

// Test/fault-injection hooks. They are dormant unless a rule is explicitly
// installed by an embedding program or test. The fsx CLI never enables them.
void install(Rule rule) noexcept;
void reset() noexcept;
bool enabled() noexcept;
std::uint64_t hits(Operation operation) noexcept;
Result<void> point(Operation operation, Timing timing);

} // namespace fsx::fault
