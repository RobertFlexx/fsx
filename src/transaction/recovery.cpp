#include "fsx/recovery.hpp"
namespace fsx {
const char* recovery_action_name(RecoveryAction a) noexcept {
    switch (a) {
    case RecoveryAction::none:return "none";
    case RecoveryAction::resume_or_rollback:return "resume_or_rollback";
    case RecoveryAction::resume_forward:return "resume_forward";
    case RecoveryAction::finish_forward:return "finish_forward";
    case RecoveryAction::manual_review:return "manual_review";
    }
    return "unknown";
}
RecoveryAssessment assess_recovery(const JournalStatus&s) {
    RecoveryAssessment r;
    if (!s.records_valid || (!s.header_a_valid && !s.header_b_valid)) {
        r.action = RecoveryAction::manual_review;
        r.reason = "journal integrity validation failed";
        return r;
    }
    switch (s.phase) {
    case OperationPhase::prepared:
        r.action = RecoveryAction::resume_or_rollback;
        r.safe_to_resume = true;
        r.rollback_available = true;
        r.reason = "no irreversible filesystem transition has committed";
        break;
    case OperationPhase::relocating:
    case OperationPhase::paused:
        r.action = RecoveryAction::resume_or_rollback;
        r.safe_to_resume = true;
        r.rollback_available = true;
        r.reason = "relocation is checkpointed before irreversible metadata transitions";
        break;
    case OperationPhase::staged:
        r.action = RecoveryAction::resume_forward;
        r.safe_to_resume = true;
        r.rollback_available = true;
        r.reason = "all relocation copies are verified; metadata has not been switched";
        break;
    case OperationPhase::metadata_finalize:
    case OperationPhase::metadata_paused:
        r.action = RecoveryAction::resume_forward;
        r.safe_to_resume = true;
        r.reason = "metadata finalization has begun; reconcile on-disk move state and finish forward";
        break;
    case OperationPhase::compacted:
        r.action = RecoveryAction::resume_forward;
        r.safe_to_resume = true;
        r.reason = "tail data is compacted; filesystem size has not yet changed";
        break;
    case OperationPhase::filesystem_shrunk:
    case OperationPhase::partition_pending:
        r.action = RecoveryAction::finish_forward;
        r.safe_to_resume = true;
        r.reason = "filesystem size transition committed; recovery must finish forward";
        break;
    case OperationPhase::complete:
        r.action = RecoveryAction::none;
        r.reason = "operation is complete";
        break;
    case OperationPhase::failed:
        r.action = RecoveryAction::manual_review;
        r.reason = "operation is marked failed; inspect the journal and filesystem before writing";
        break;
    }

    return r;
}
}
