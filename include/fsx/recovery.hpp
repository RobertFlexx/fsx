#pragma once
#include "fsx/journal.hpp"
#include <string>

namespace fsx {

enum class RecoveryAction {
    none,
    resume_or_rollback,
    resume_forward,
    finish_forward,
    manual_review
};

struct RecoveryAssessment {
    RecoveryAction action{RecoveryAction::none};
    bool safe_to_resume{false};
    bool rollback_available{false};
    std::string reason;
};

RecoveryAssessment assess_recovery(const JournalStatus& status);
const char* recovery_action_name(RecoveryAction action) noexcept;

} // namespace fsx
