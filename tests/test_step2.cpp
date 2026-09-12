#include "fsx/device.hpp"
#include "fsx/io_scheduler.hpp"
#include "fsx/journal.hpp"
#include "fsx/recovery.hpp"
#include "fsx/uuid.hpp"
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>

int main() {
    const std::string path = "/tmp/fsx-step2-test-"+std::to_string(static_cast<long long>(::getpid()))+".bin";
    {
        auto d = fsx::BlockDevice::create_file(path, 1024*1024);
        assert(d);
        std::vector<std::byte> pattern(4096);
        for (std::size_t i = 0; i<pattern.size(); ++i)pattern[i] = std::byte(static_cast<unsigned char>(i&0xffU));
        assert(d.value().write_exact(65536, pattern));
        assert(d.value().flush());
        fsx::IoTuning tune;
        tune.workers = 4;
        tune.queue_depth = 8;
        fsx::AsyncIoScheduler io(tune);
        auto a = io.submit_read(d.value(), 65536, 4096);
        auto b = io.submit_read(d.value(), 65536, 4096);
        auto ar = a.get();
        auto br = b.get();
        assert(ar && br);
        assert(ar.value() == pattern && br.value() == pattern);
        io.drain();
        auto st = io.stats();
        assert(st.read_ops == 2 && st.bytes_read == 8192);
    }
    std::filesystem::remove(path);

    const std::string jpath = "/tmp/fsx-step2-journal-"+std::to_string(static_cast<long long>(::getpid()))+".bin";
    fsx::JournalIdentity id;
    id.operation_uuid = fsx::TransactionJournal::random_uuid();
    auto u = fsx::parse_uuid("00112233-4455-6677-8899-aabbccddeeff");
    assert(u);
    id.filesystem_uuid = u.value();
    id.device_size = 1ULL<<30;
    id.target_size = 900ULL<<20;
    {
        auto j = fsx::TransactionJournal::create(jpath, id, 1024*1024);
        assert(j);
        fsx::JournalRecord r;
        r.type = fsx::JournalRecordType::relocation_intent;
        r.transaction_id = 1;
        r.source_block = 1000;
        r.destination_block = 100;
        r.block_count = 64;
        assert(j.value().append(r));
        r.type = fsx::JournalRecordType::copy_verified;
        assert(j.value().append(r));
        r.type = fsx::JournalRecordType::metadata_committed;
        assert(j.value().append(r));
        assert(j.value().checkpoint(fsx::OperationPhase::paused));
    }
    {
        auto j = fsx::TransactionJournal::open(jpath, false);
        assert(j);
        const auto&s = j.value().status();
        assert(s.records_valid);
        assert(s.record_count == 4);
        assert(s.committed_transactions == 1);
        assert(s.phase == fsx::OperationPhase::paused);
        auto a = fsx::assess_recovery(s);
        assert(a.action == fsx::RecoveryAction::resume_or_rollback);
        assert(a.safe_to_resume && a.rollback_available);
        auto rr = j.value().records();
        assert(rr && rr.value().size() == 4);
    }
    std::filesystem::remove(jpath);
    return 0;
}
