#pragma once
#include "fsx/journal.hpp"
#include "fsx/result.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace fsx {

struct JournalValidationReport {
    bool clean{true};
    std::uint64_t transactions{};
    std::uint64_t intents{};
    std::uint64_t copies_verified{};
    std::uint64_t destinations_reserved{};
    std::uint64_t owners_switched{};
    std::uint64_t sources_released{};
    std::uint64_t metadata_committed{};
    std::uint64_t checkpoints{};
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

Result<JournalValidationReport> validate_journal_semantics(const TransactionJournal& journal);

} // namespace fsx
