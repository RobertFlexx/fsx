#include "fsx/device.hpp"
#include "fsx/fault.hpp"
#include "fsx/journal.hpp"
#include "fsx/journal_validate.hpp"
#include "fsx/image.hpp"
#include <cassert>
#include <filesystem>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {
std::string tmp(const std::string& tag) {
    return "/tmp/fsx-fault-" + tag + "-" + std::to_string(static_cast<long long>(::getpid()));
}
fsx::JournalIdentity identity() {
    fsx::JournalIdentity id;
    id.operation_uuid = fsx::TransactionJournal::random_uuid();
    id.filesystem_uuid = fsx::TransactionJournal::random_uuid();
    id.device_size = 16ULL * 1024ULL * 1024ULL;
    id.target_size = 8ULL * 1024ULL * 1024ULL;
    return id;
}
void assert_openable(const std::string& path) {
    auto j = fsx::TransactionJournal::open(path, false);
    assert(j);
    auto v = fsx::validate_journal_semantics(j.value());
    assert(v && v.value().clean);
    assert(j.value().status().record_count <= 1);
}
}

int main() {
    const auto id = identity();

    // Journal publication is all-or-nothing for faults before rename.
    for (auto op : {fsx::fault::Operation::write, fsx::fault::Operation::flush}) {
        const int max_hit = op == fsx::fault::Operation::write ? 2 : 1;
        for (int hit = 1; hit <= max_hit; ++hit) {
            for (auto when : {fsx::fault::Timing::before, fsx::fault::Timing::after}) {
                const auto p = tmp(
                    "create-" + std::to_string(static_cast<int>(op)) + "-" + std::to_string(hit) + "-"
                    + std::to_string(static_cast<int>(when)));
                std::filesystem::remove(p);
                fsx::fault::install({op, when, static_cast<std::uint64_t>(hit), true});
                auto j = fsx::TransactionJournal::create(p, id, 1ULL << 20U);
                fsx::fault::reset();
                assert(!j);
                assert(!std::filesystem::exists(p));
            }
        }
    }

    // Build one pristine journal, then inject failures at every append durability
    // boundary. Regardless of which boundary reports failure, reopening must see
    // either the old state or a fully valid new state, never torn metadata.
    const auto base = tmp("base");
    std::filesystem::remove(base);
    { auto j = fsx::TransactionJournal::create(base, id, 1ULL << 20U);
        assert(j);
    }

    for (auto op : {fsx::fault::Operation::write, fsx::fault::Operation::flush}) {
        const int max_hit = 2;
        for (int hit = 1; hit <= max_hit; ++hit) {
            for (auto when : {fsx::fault::Timing::before, fsx::fault::Timing::after}) {
                const auto p = tmp(
                    "append-" + std::to_string(static_cast<int>(op)) + "-" + std::to_string(hit) + "-"
                    + std::to_string(static_cast<int>(when)));
                std::filesystem::copy_file(base, p, std::filesystem::copy_options::overwrite_existing);
                auto j = fsx::TransactionJournal::open(p, true);
                assert(j);
                fsx::JournalRecord n;
                n.type = fsx::JournalRecordType::note;
                fsx::fault::install({op, when, static_cast<std::uint64_t>(hit), true});
                (void)j.value().append(n);
                fsx::fault::reset();
                assert_openable(p);
                std::filesystem::remove(p);
            }
        }
    }

    // Process-death simulation at append boundaries. This is stronger than a
    // returned error: destructors and normal cleanup never run in the child.
    for (auto op : {fsx::fault::Operation::write, fsx::fault::Operation::flush}) {
        for (int hit = 1; hit <= 2; ++hit) {
            const auto p = tmp("crash-" + std::to_string(static_cast<int>(op)) + "-" + std::to_string(hit));
            std::filesystem::copy_file(base, p, std::filesystem::copy_options::overwrite_existing);
            const pid_t child = ::fork();
            assert(child >= 0);
            if (child == 0) {
                auto j = fsx::TransactionJournal::open(p, true);
                if (!j) ::_exit(201);
                fsx::JournalRecord n;
                n.type = fsx::JournalRecordType::note;
                fsx::fault::install({
                                    op, fsx::fault::Timing::after, static_cast<std::uint64_t>(hit), true,
                                    fsx::fault::Action::terminate_process
                                    }
                                   );
                (void)j.value().append(n);
                ::_exit(0);
            }
            int status = 0;
            assert(::waitpid(child, &status, 0) == child);
            assert(WIFEXITED(status));
            assert_openable(p);
            std::filesystem::remove(p);
        }
    }

    // Image publication obeys the same all-or-nothing rule. Failures in the
    // temporary image write stream or its durability flush must never publish
    // the requested final filename.
    const auto image_source = tmp("image-source");
    const auto image_final = tmp("image-final");
    std::filesystem::remove(image_source);
    std::filesystem::remove(image_final);
    {
        auto src = fsx::BlockDevice::create_file(image_source, 256ULL * 1024ULL);
        assert(src);
        std::array<std::byte, 4096> pattern{};
        pattern.fill(std::byte{0x5a});
        assert(src.value().write_exact(0, pattern));
        assert(src.value().flush());
    }
    for (auto when : {fsx::fault::Timing::before, fsx::fault::Timing::after}) {
        auto src = fsx::BlockDevice::open_read(image_source);
        assert(src);
        fsx::fault::install({fsx::fault::Operation::write, when, 1, true});
        auto made = fsx::image::create(src.value(), image_final);
        fsx::fault::reset();
        assert(!made);
        assert(!std::filesystem::exists(image_final));
    }
    for (auto when : {fsx::fault::Timing::before, fsx::fault::Timing::after}) {
        auto src = fsx::BlockDevice::open_read(image_source);
        assert(src);
        fsx::fault::install({fsx::fault::Operation::flush, when, 1, true});
        auto made = fsx::image::create(src.value(), image_final);
        fsx::fault::reset();
        assert(!made);
        assert(!std::filesystem::exists(image_final));
    }
    for (const auto& e : std::filesystem::directory_iterator("/tmp")) {
        const auto n = e.path().filename().string();
        const auto prefix = std::filesystem::path(image_final).filename().string()+".part.";
        if (n.rfind(prefix, 0) == 0) std::filesystem::remove(e.path());
    }
    std::filesystem::remove(image_source);
    std::filesystem::remove(base);
    return 0;
}
