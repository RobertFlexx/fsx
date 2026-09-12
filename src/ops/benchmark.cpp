#include "fsx/benchmark.hpp"
#include "fsx/cancel.hpp"
#include "fsx/io_scheduler.hpp"
#include <algorithm>
#include <chrono>
#include <deque>
#include <future>

namespace fsx {
Result<BenchmarkResult> benchmark_read(BlockDevice& dev, std::uint64_t bytes, Progress* progress) {
    if (bytes == 0 || bytes>dev.geometry().size_bytes)bytes = std::min<std::uint64_t>(dev.geometry().size_bytes,
                                                                                      1ULL<<30U);
    auto pr = profile_device(dev);
    if (!pr)return pr.error();
    const auto tuning = pr.value().tuning;
    AsyncIoScheduler io(tuning);
    const std::size_t chunk = std::clamp<std::size_t>(tuning.large_io_bytes, 1U*1024U*1024U, 16U*1024U*1024U);
    const std::size_t max_inflight = std::max<std::size_t>(2, std::min<std::size_t>({
                                                                                    8, tuning.queue_depth,
                                                                                    tuning.workers*2
                                                                                    }
                                                                                   ));
    struct Pending{
        std::uint64_t bytes{};
        std::future<Result<std::vector<std::byte>>> future;
    }
    ;
    std::deque<Pending> q;
    auto start = std::chrono::steady_clock::now();
    std::uint64_t submitted = 0, done = 0;
    auto collect_one = [&]()->Result<void>{
        auto p = std::move(q.front());
        q.pop_front();
        auto r = p.future.get();
        if (!r)return r.error();
        done+=p.bytes;
        const auto st = io.stats();
        if (progress)progress->update({
                                      "benchmark","parallel sequential read", done, bytes, st.bytes_read,
                                      st.bytes_written, 0, 0, 0, static_cast<std::uint32_t>(q.size()), 0, 0
                                      }
                                     );
        return {};
    }
    ;
    while (submitted<bytes || !q.empty()) {
        if (Cancellation::requested())return Error{Errc::unsafe, 0,"benchmark cancelled"};
        while (submitted<bytes && q.size()<max_inflight) {
            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(chunk, bytes-submitted));
            q.push_back({
                        n, io.submit_read(dev, submitted, n)
                        }
                       );
            submitted+=n;
        }
        if (!q.empty()) {
            auto r = collect_one();
            if (!r)return r.error();
        }
    }
    io.drain();
    const auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    return BenchmarkResult{
        done, secs, secs>0?static_cast<double>(done)/secs:0
    }
    ;
}
}
