#pragma once
#include "fsx/device.hpp"
#include "fsx/result.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace fsx {

enum class OperationPhase : std::uint32_t {
    prepared = 1,
    relocating = 2,
    paused = 3,
    metadata_finalize = 4,
    filesystem_shrunk = 5,
    partition_pending = 6,
    complete = 7,
    failed = 8,
    staged = 9,
    metadata_paused = 10,
    compacted = 11
};

enum class JournalRecordType : std::uint16_t {
    relocation_intent = 1,
    copy_verified = 2,
    metadata_committed = 3,
    checkpoint = 4,
    note = 5,
    destination_reserved = 6,
    owner_switched = 7,
    source_released = 8,
    filesystem_marked_dirty = 9,
    filesystem_marked_clean = 10,
    partition_intent = 11,
    partition_committed = 12
};

struct JournalIdentity {
    std::array<std::byte, 16> operation_uuid{};
    std::array<std::byte, 16> filesystem_uuid{};
    std::uint64_t device_size{};
    std::uint64_t target_size{};
};

struct JournalRecord {
    JournalRecordType type{JournalRecordType::note};
    std::uint64_t sequence{};
    std::uint64_t transaction_id{};
    std::uint64_t source_block{};
    std::uint64_t destination_block{};
    std::uint64_t block_count{};
    std::uint64_t inode{};
    std::uint64_t aux{};
    std::uint32_t data_crc32c{};
    std::uint32_t state{};
};

struct JournalStatus {
    JournalIdentity identity{};
    OperationPhase phase{OperationPhase::prepared};
    std::uint64_t header_sequence{};
    std::uint64_t record_count{};
    std::uint64_t committed_transactions{};
    std::uint64_t last_transaction_id{};
    bool header_a_valid{false};
    bool header_b_valid{false};
    bool records_valid{false};
};

class TransactionJournal {
public:
    TransactionJournal() = default;
    TransactionJournal(const TransactionJournal&) = delete;
    TransactionJournal& operator = (const TransactionJournal&) = delete;
    TransactionJournal(TransactionJournal && ) noexcept = default;
    TransactionJournal& operator = (TransactionJournal && ) noexcept = default;

    static Result<TransactionJournal> create(std::string path,
                                             const JournalIdentity& identity,
                                             std::uint64_t capacity_bytes = 16ULL * 1024ULL * 1024ULL);
    static Result<TransactionJournal> open(std::string path, bool writable = false);

    Result<void> append(JournalRecord record);
    Result<void> append_batch(const std::vector<JournalRecord>& records);
    Result<void> checkpoint(OperationPhase phase);
    Result<void> flush();
    Result<std::vector<JournalRecord>> records() const;
    const JournalStatus& status() const noexcept { return status_;
    }
    const std::string& path() const noexcept { return device_.path();
    }

    static std::array<std::byte, 16> random_uuid();

public:
    static constexpr std::uint64_t header_bytes = 4096;
    static constexpr std::uint64_t records_offset = header_bytes * 2;
    static constexpr std::uint64_t record_bytes = 128;

private:
    Result<void> write_header(std::uint64_t slot, std::uint64_t sequence,
                              OperationPhase phase, std::uint64_t record_count);
    Result<void> load();

    BlockDevice device_{};
    JournalStatus status_{};
    bool writable_{false};
};

const char* operation_phase_name(OperationPhase phase) noexcept;
const char* journal_record_type_name(JournalRecordType type) noexcept;

} // namespace fsx
