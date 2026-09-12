#include "fsx/clone.hpp"
#include "fsx/cancel.hpp"
#include "fsx/io_scheduler.hpp"
#include "fsx/safety.hpp"
#include "fsx/task_pool.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <future>
#include <vector>

namespace fsx {
namespace {
Result<void> copy_chunk(BlockDevice& source, BlockDevice& target, std::uint64_t off, std::size_t len) {
    std::vector<std::byte> b(len);
    auto r = source.read_exact(off, b);
    if (!r) return r.error();
    return target.write_exact(off, b);
}
Result<void> verify_chunk(BlockDevice& source, BlockDevice& target, std::uint64_t off, std::size_t len) {
    std::vector<std::byte> a(len), b(len);
    auto r = source.read_exact(off, a);
    if (!r) return r.error();
    r = target.read_exact(off, b);
    if (!r) return r.error();
    if (a != b) return Error{Errc::corrupt, 0, "clone verification mismatch at byte " + std::to_string(off)};
    return {};
}
}

Result<CloneReport> clone_device(BlockDevice& source, BlockDevice& target,
                                 const CloneOptions& options, Progress* progress) {
    auto disjoint = require_nonoverlapping_devices(source, target);
    if (!disjoint) return disjoint.error();
    if (source.geometry().size_bytes == 0) return Error{
        Errc::invalid_argument, 0, "source device has zero size"
    }
    ;
    if (source.geometry().is_block_device && !options.allow_live_source) {
        auto ss = inspect_device_safety(source);
        if (!ss) return ss.error();
        const auto& s = ss.value();
        if (s.mounted || s.active_swap || s.descendant_mounted ||
            s.descendant_swap || s.has_holders)
            return Error{Errc::unsafe, 0,
                "clone source is active; use --allow-live-source only if a crash-consistent live clone is acceptable"
            }
        ;
    }
    if (target.geometry().size_bytes < source.geometry().size_bytes)
        return Error{Errc::unsafe, 0, "clone target is smaller than source"};
    if (target.geometry().is_block_device && !options.allow_block_device)
        return Error{Errc::unsafe, 0, "clone to block device requires --allow-block-device"};
    auto safe = require_offline_for_write(target);
    if (!safe) return safe.error();

    auto profile = profile_device(target);
    if (!profile) return profile.error();
    std::size_t chunk = options.chunk_bytes ? options.chunk_bytes : profile.value().tuning.large_io_bytes;
    chunk = std::clamp<std::size_t>(chunk, 256U * 1024U, 64U * 1024U * 1024U);
    const auto align = std::max<std::uint32_t>({
                                               1U, source.geometry().logical_sector, target.geometry().logical_sector
                                               }
                                              );
    chunk -= chunk % align;
    if (chunk == 0) chunk = align;
    const auto workers = std::max<std::size_t>(1, profile.value().tuning.workers);
    TaskPool pool(workers, std::max<std::size_t>(4, workers * 2U));

    CloneReport out;
    const auto bytes = source.geometry().size_bytes;
    const auto chunks = (bytes + chunk - 1U) / chunk;
    out.chunks = chunks;
    auto started = std::chrono::steady_clock::now();
    std::uint64_t next = 0;
    while (next < chunks) {
        if (Cancellation::requested()) return Error{
            Errc::unsafe, 0, "clone cancelled; target contains a partial clone"
        }
        ;
        const auto batch_n = std::min<std::uint64_t>(workers, chunks - next);
        std::vector<std::future<Result<void>>> jobs;
        jobs.reserve(static_cast<std::size_t>(batch_n));
        std::vector<std::uint64_t> lens;
        lens.reserve(static_cast<std::size_t>(batch_n));
        for (std::uint64_t j = 0; j < batch_n; ++j) {
            const auto idx = next + j;
            const auto off = idx * static_cast<std::uint64_t>(chunk);
            const auto len64 = std::min<std::uint64_t>(chunk, bytes - off);
            const auto len = static_cast<std::size_t>(len64);
            lens.push_back(len64);
            jobs.push_back(pool.submit([&source, &target, off, len] {
                                       return copy_chunk(source, target, off, len);
                                       }
                                      ));
        }
        for (std::size_t j = 0; j < jobs.size(); ++j) {
            auto r = jobs[j].get();
            if (!r) return r.error();
            out.bytes_copied += lens[j];
        }
        next += batch_n;
        if (progress) progress->update({"cloning", "copy", out.bytes_copied, bytes,
                                       out.bytes_copied, out.bytes_copied, next, chunks});
    }
    auto fl = target.flush();
    if (!fl) return fl.error();
    out.copy_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    out.copy_bytes_per_second = out.copy_seconds > 0 ? static_cast<double>(out.bytes_copied) / out.copy_seconds : 0.0;

    if (options.verify_after) {
        next = 0;
        while (next < chunks) {
            if (Cancellation::requested()) return Error{Errc::unsafe, 0, "clone verification cancelled"};
            const auto batch_n = std::min<std::uint64_t>(workers, chunks - next);
            std::vector<std::future<Result<void>>> jobs;
            jobs.reserve(static_cast<std::size_t>(batch_n));
            std::vector<std::uint64_t> lens;
            lens.reserve(static_cast<std::size_t>(batch_n));
            for (std::uint64_t j = 0; j < batch_n; ++j) {
                const auto idx = next + j;
                const auto off = idx * static_cast<std::uint64_t>(chunk);
                const auto len64 = std::min<std::uint64_t>(chunk, bytes - off);
                const auto len = static_cast<std::size_t>(len64);
                lens.push_back(len64);
                jobs.push_back(pool.submit([&source, &target, off, len] {
                                           return verify_chunk(source, target, off, len);
                                           }
                                          ));
            }
            for (std::size_t j = 0; j < jobs.size(); ++j) {
                auto r = jobs[j].get();
                if (!r) return r.error();
                out.bytes_verified += lens[j];
            }
            next += batch_n;
            if (progress) progress->update({"cloning", "verify", out.bytes_verified, bytes,
                                           out.bytes_verified, 0, next, chunks});
        }
        out.verified = true;
    }
    return out;
}
} // namespace fsx
