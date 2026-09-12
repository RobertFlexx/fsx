#include "fsx/journal_validate.hpp"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace fsx {
namespace {
struct TxState {
    bool intent{false};
    bool verified{false};
    bool reserved{false};
    bool switched{false};
    bool released{false};
    bool committed{false};
    std::uint64_t src{};
    std::uint64_t dst{};
    std::uint64_t blocks{};
    std::uint64_t inode{};
};

void add_error(JournalValidationReport& r, std::string msg) {
    r.clean = false;
    r.errors.push_back(std::move(msg));
}

bool same_geometry(const TxState& s, const JournalRecord& r) {
    return s.src == r.source_block && s.dst == r.destination_block &&
        s.blocks == r.block_count && s.inode == r.inode;
}
}

Result<JournalValidationReport> validate_journal_semantics(const TransactionJournal& journal) {
    JournalValidationReport out;
    if (!journal.status().records_valid)
        return Error{Errc::corrupt, 0, "journal record integrity is not valid"};
    auto rr = journal.records();
    if (!rr) return rr.error();

    std::unordered_map<std::uint64_t, TxState> tx;
    bool filesystem_dirty = false;
    bool filesystem_clean_seen = false;
    std::uint64_t last_checkpoint_phase = 0;

    for (const auto& r : rr.value()) {
        if (r.type == JournalRecordType::checkpoint) {
            ++out.checkpoints;
            if (r.aux < static_cast<std::uint64_t>(OperationPhase::prepared) ||
                r.aux > static_cast<std::uint64_t>(OperationPhase::compacted)) {
                add_error(out, "checkpoint contains an invalid phase value");
            } else {
                last_checkpoint_phase = r.aux;
            }
            continue;
        }
        if (r.type == JournalRecordType::filesystem_marked_dirty) {
            filesystem_dirty = true;
            filesystem_clean_seen = false;
            continue;
        }
        if (r.type == JournalRecordType::filesystem_marked_clean) {
            if (!filesystem_dirty)
                out.warnings.push_back("filesystem-clean record appears without a preceding dirty record");
            filesystem_dirty = false;
            filesystem_clean_seen = true;
            continue;
        }
        if (r.type == JournalRecordType::partition_intent ||
            r.type == JournalRecordType::partition_committed ||
            r.type == JournalRecordType::note) continue;

        if (r.transaction_id == 0) {
            add_error(out, std::string(journal_record_type_name(r.type)) + " record has transaction id 0");
            continue;
        }
        auto& s = tx[r.transaction_id];
        if (r.type == JournalRecordType::relocation_intent) {
            ++out.intents;
            if (s.intent) {
                if (!same_geometry(s, r)) add_error(out,
                                                    "duplicate relocation intent changes transaction geometry");
                else {
                    out.warnings.push_back(
                        "duplicate relocation intent for transaction " + std::to_string(r.transaction_id));
                }
                continue;
            }
            s.intent = true;
            s.src = r.source_block;
            s.dst = r.destination_block;
            s.blocks = r.block_count;
            s.inode = r.inode;
            if (s.blocks == 0 || s.src == s.dst)
                add_error(out, "relocation intent has invalid source/destination geometry");
            continue;
        }

        if (!s.intent) {
            add_error(
                out,
                std::string(journal_record_type_name(r.type))
                    + " precedes relocation intent for transaction " + std::to_string(r.transaction_id));
            continue;
        }
        if (!same_geometry(s, r)) {
            add_error(
                out,
                std::string(journal_record_type_name(r.type))
                    + " changes relocation geometry for transaction " + std::to_string(r.transaction_id));
            continue;
        }

        switch (r.type) {
        case JournalRecordType::copy_verified:
            ++out.copies_verified;
            if (s.verified) {
                out.warnings.push_back(
                    "duplicate verified-copy record for transaction " + std::to_string(r.transaction_id));
            }
            s.verified = true;
            break;
        case JournalRecordType::destination_reserved:
            ++out.destinations_reserved;
            if (!s.verified) {
                out.warnings.push_back(
                    "destination reserved before durable copy-verification record for transaction "
                    + std::to_string(r.transaction_id));
            }
            s.reserved = true;
            break;
        case JournalRecordType::owner_switched:
            ++out.owners_switched;
            if (!s.verified) {
                add_error(out,
                          "owner switched before verified copy for transaction "
                              + std::to_string(r.transaction_id));
            }
            s.switched = true;
            break;
        case JournalRecordType::source_released:
            ++out.sources_released;
            if (!s.switched) {
                add_error(out,
                          "source released before owner switch for transaction "
                              + std::to_string(r.transaction_id));
            }
            s.released = true;
            break;
        case JournalRecordType::metadata_committed:
            ++out.metadata_committed;
            if (!s.verified) {
                add_error(out,
                          "metadata committed without verified copy for transaction "
                              + std::to_string(r.transaction_id));
            }
            // Crash recovery can observe the on-disk owner switch/release and then
            // append only metadata_committed, so absent intermediate records are
            // warnings rather than hard errors.
            if (!s.switched) {
                out.warnings.push_back(
                    "metadata commit has no owner-switched record; on-disk reconciliation is required for transaction "
                    + std::to_string(r.transaction_id));
            }
            if (!s.released) {
                out.warnings.push_back(
                    "metadata commit has no source-released record; on-disk reconciliation is required for transaction "
                    + std::to_string(r.transaction_id));
            }
            s.committed = true;
            break;
        default:
            break;
        }
    }

    out.transactions = static_cast<std::uint64_t>(tx.size());
    for (const auto& [id, s] : tx) {
        if (!s.intent) continue;
        if (s.committed && !s.verified)
            add_error(out, "committed transaction " + std::to_string(id) + " has no verified copy");
        if (s.released && !s.switched)
            add_error(out, "transaction " + std::to_string(id) + " released source without owner switch");
    }

    const auto status_phase = static_cast<std::uint64_t>(journal.status().phase);
    if (last_checkpoint_phase != 0 && last_checkpoint_phase != status_phase)
        out.warnings.push_back("latest checkpoint record phase differs from authoritative journal header phase");
    if (journal.status().phase == OperationPhase::complete && filesystem_dirty)
        add_error(out, "journal is complete but last recorded filesystem state is dirty");
    if ((journal.status().phase == OperationPhase::complete ||
         journal.status().phase == OperationPhase::filesystem_shrunk) &&
        !filesystem_clean_seen)
        out.warnings.push_back("terminal journal phase has no explicit filesystem-clean record");

    return out;
}

} // namespace fsx
