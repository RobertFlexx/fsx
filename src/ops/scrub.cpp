#include "fsx/scrub.hpp"
#include "fsx/cancel.hpp"
#include "fsx/crc.hpp"
#include "fsx/io_scheduler.hpp"
#include <algorithm>
#include <chrono>
#include <deque>
#include <future>
#include <vector>

namespace fsx {
Result<ScrubReport> scrub_read(BlockDevice& dev, const ScrubOptions& options, Progress* progress) {
    if (dev.geometry().size_bytes == 0) return Error{Errc::invalid_argument, 0, "device size is zero"};
    if (options.passes == 0 || options.passes > 64) return Error{
        Errc::invalid_argument, 0, "scrub passes must be between 1 and 64"
    }
    ;
    auto profile = profile_device(dev);
    if (!profile) return profile.error();
    const auto bytes = options.bytes == 0 ? dev.geometry().size_bytes : options.bytes;
    if (bytes > dev.geometry().size_bytes) return Error{
        Errc::invalid_argument, 0, "scrub length exceeds device size"
    }
    ;
    std::size_t chunk = options.chunk_bytes ? options.chunk_bytes : profile.value().tuning.large_io_bytes;
    chunk = std::clamp<std::size_t>(chunk, 64U * 1024U, 64U * 1024U * 1024U);
    const auto sector = std::max<std::uint32_t>(1, dev.geometry().logical_sector);
    chunk -= chunk % sector;
    if (chunk == 0) chunk = sector;

    AsyncIoScheduler io(profile.value().tuning);
    const std::size_t max_inflight = std::max<std::size_t>(2,
                                                           std::min<std::size_t>(profile.value().tuning.queue_depth,
                                                                                 profile.value().tuning.workers * 3U));
    struct Pending {
        std::uint64_t offset{};
        std::uint64_t bytes{};
        std::future<Result<std::vector<std::byte>>> future;
    }
    ;

    ScrubReport report;
    report.bytes_per_pass = bytes;
    std::uint32_t reference_crc = 0;
    const auto start = std::chrono::steady_clock::now();

    for (std::uint32_t pass = 0; pass < options.passes; ++pass) {
        std::deque<Pending> q;
        std::uint64_t submitted = 0;
        std::uint64_t completed = 0;
        std::uint32_t crc = 0xffffffffU;
        while (submitted < bytes || !q.empty()) {
            if (Cancellation::requested()) return Error{Errc::unsafe, 0, "scrub cancelled"};
            while (submitted < bytes && q.size() < max_inflight) {
                const auto n64 = std::min<std::uint64_t>(chunk, bytes - submitted);
                const auto n = static_cast<std::size_t>(n64);
                q.push_back({submitted, n64, io.submit_read(dev, submitted, n)});
                submitted += n64;
            }
            auto p = std::move(q.front());
            q.pop_front();
            auto r = p.future.get();
            if (!r) return r.error();
            crc = crc32c(r.value(), crc);
            completed += p.bytes;
            ++report.read_operations;
            report.total_bytes_read += p.bytes;
            if (progress) progress->update({
                                           "scrubbing",
                                           "pass " + std::to_string(pass + 1) + "/" + std::to_string(options.passes),
                                           static_cast<std::uint64_t>(pass) * bytes + completed,
                                           static_cast<std::uint64_t>(options.passes) * bytes,
                                           report.total_bytes_read, 0, report.read_operations, 0,
                                           report.total_bytes_read,
                                           static_cast<std::uint32_t>(q.size()), 0, 0
                                           }
                                          );
        }
        crc ^= 0xffffffffU;
        if (pass == 0) reference_crc = crc;
        else if (crc != reference_crc) report.stable = false;
        report.crc32c = crc;
        ++report.passes_completed;
        if (!report.stable) return Error{
            Errc::corrupt, 0, "scrub checksum changed between passes; media or I/O path is unstable"
        }
        ;
    }
    io.drain();
    report.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    report.bytes_per_second = report.seconds > 0 ? static_cast<double>(report.total_bytes_read) / report.seconds : 0.0;
    return report;
}
} // namespace fsx
