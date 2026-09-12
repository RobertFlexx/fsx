#include "fsx/allocation.hpp"
#include "fsx/cancel.hpp"
#include "internal.hpp"
#include <algorithm>
#include <future>
#include <deque>
#include <limits>

namespace fsx::ext4 {
namespace {
void append_run(std::vector<BlockExtent>& out, std::uint64_t start, std::uint64_t len) {
    if (len == 0) return;
    if (!out.empty() && out.back().end() == start) out.back().length += len;
    else out.push_back({start, len});
}
}

Result<AllocationGraph> build_allocation_graph(BlockDevice& dev,
                                               Progress* progress, const IoTuning* requested) {
    auto sr = read_superblock(dev);
    if (!sr) return sr.error();
    const auto sb = sr.value();
    if ((sb.feature_incompat & detail::INCOMPAT_META_BG) != 0) return Error{
        Errc::unsupported, 0,"allocation graph does not yet support ext4 meta_bg descriptor placement"
    }
    ;
    if ((sb.feature_ro_compat & detail::RO_COMPAT_BIGALLOC) != 0) return Error{
        Errc::unsupported, 0,"bigalloc uses cluster allocation; block-granularity relocation is refused"
    }
    ;
    auto pr = profile_device(dev);
    if (!pr) return pr.error();
    IoTuning tuning = requested?*requested:pr.value().tuning;
    tuning.workers = std::max<std::size_t>(1, tuning.workers);
    AsyncIoScheduler io(tuning);
    const auto groups = sb.groups_count();
    std::vector<detail::GroupDesc> descs;
    descs.reserve(static_cast<std::size_t>(groups));
    for (std::uint64_t g = 0; g<groups; ++g) {
        auto gd = detail::read_group_desc(dev, sb, g);
        if (!gd)return gd.error();
        if ((gd.value().flags&detail::BG_BLOCK_UNINIT) != 0)return Error{
            Errc::unsupported,
            0,
            "block-uninitialized ext4 groups require reconstructed bitmap support before destructive planning"
        }
        ;
        descs.push_back(gd.value());
    }
    if (progress) progress->update({
                                   "scanning","reading ext allocation bitmaps", 0, groups, 0, 0, 0, groups, 0, 0, 0, 0
                                   }
                                  );

    // Keep only a bounded pipeline of bitmap reads in flight.  A filesystem
    // can have hundreds of thousands of groups; one future per group wastes
    // memory without adding I/O parallelism once the scheduler queue is full.
    using BitmapFuture = std::future<Result<std::vector<std::byte>>>;
    std::deque<std::pair<std::uint64_t, BitmapFuture>> pending;
    const auto window = std::max<std::size_t>(1, tuning.queue_depth+tuning.workers);
    std::uint64_t next_group = 0;
    auto submit_one = [&](std::uint64_t g)->Result<void>{
        const auto block = descs[static_cast<std::size_t>(g)].block_bitmap;
        if (block >= sb.blocks_count)return Error{Errc::corrupt, 0,"block bitmap points beyond filesystem"};
        pending.emplace_back(g, io.submit_read(dev,
                                               block*sb.block_size(), static_cast<std::size_t>(sb.block_size())));
        return {};
    };
    while (next_group<groups && pending.size()<window) {
        auto q = submit_one(next_group++);
        if (!q)return q.error();
    }

    AllocationGraph graph;
    graph.block_size = sb.block_size();
    graph.total_blocks = sb.blocks_count;
    bool have_state = false, current_used = false;
    std::uint64_t run_start = 0, run_len = 0;
    auto flush_run = [&]{
        if (!have_state || run_len == 0)return;
        if (current_used)append_run(graph.allocated, run_start, run_len);
        else append_run(graph.free, run_start, run_len);
    }
    ;
    std::uint64_t completed = 0;
    while (!pending.empty()) {
        if (Cancellation::requested()) return Error{Errc::unsafe, 0,"scan cancelled"};
        auto g = pending.front().first;
        auto rr = pending.front().second.get();
        pending.pop_front();
        if (!rr)return rr.error();
        auto& bm = rr.value();
        if (g != completed)return Error{Errc::internal, 0,"allocation bitmap pipeline lost group ordering"};
        const auto start = detail::group_start_block(sb, g), count = detail::group_block_count(sb, g);
        for (std::uint64_t i = 0; i<count; ++i) {
            const bool used = detail::bit_set(bm, i);
            const auto block = start+i;
            if (!have_state) {
                have_state = true;
                current_used = used;
                run_start = block;
                run_len = 1;
            }
            else if (used == current_used && run_start+run_len == block) {
                ++run_len;
            }
            else {
                flush_run();
                current_used = used;
                run_start = block;
                run_len = 1;
            }
            if (used)++graph.allocated_blocks;
            else ++graph.free_blocks;
        }
        ++completed;
        if (next_group<groups) {
            auto q = submit_one(next_group++);
            if (!q)return q.error();
        }
        const auto st = io.stats();
        if (progress)progress->update({
                                      "scanning","group "+std::to_string(completed)+"/"+std::to_string(groups),
                                      completed, groups, st.bytes_read, st.bytes_written, completed, groups, 0, 0, 0, 0
                                      }
                                     );
    }
    flush_run();
    io.drain();
    return graph;
}

Result<RelocationGeometry> build_relocation_geometry(const AllocationGraph& graph,
                                                     std::uint64_t target_blocks) {
    if (target_blocks == 0 || target_blocks >= graph.total_blocks)return Error{
        Errc::invalid_argument, 0,"relocation target must be inside filesystem"
    }
    ;
    std::vector<BlockExtent> sources, dests;
    for (const auto&e:graph.allocated) {
        if (e.end() <= target_blocks)continue;
        const auto s = std::max(e.start, target_blocks);
        sources.push_back({
                          s, e.end()-s
                          }
                         );
    }
    for (const auto&e:graph.free) {
        if (e.start >= target_blocks)break;
        const auto end = std::min(e.end(), target_blocks);
        if (end>e.start)dests.push_back({
                                        e.start, end-e.start
                                        }
                                       );
    }
    auto len_desc = [](const BlockExtent&a, const BlockExtent&b) {
        return a.length>b.length;
    };
    std::sort(sources.begin(), sources.end(), len_desc);
    std::sort(dests.begin(), dests.end(), len_desc);
    RelocationGeometry r;
    r.target_blocks = target_blocks;
    for (const auto&e:sources) {
        r.source_blocks+=e.length;
        r.largest_source_extent = std::max(r.largest_source_extent, e.length);
    }
    for (const auto&e:dests) {
        r.destination_blocks+=e.length;
        r.largest_destination_extent = std::max(r.largest_destination_extent, e.length);
    }
    r.capacity_ok = r.destination_blocks >= r.source_blocks;
    if (!r.capacity_ok)return r;
    std::size_t di = 0;
    std::uint64_t dused = 0;
    for (const auto&s:sources) {
        std::uint64_t soff = 0;
        while (soff<s.length) {
            while (di<dests.size() && dused == dests[di].length) {
                ++di;
                dused = 0;
            }
            if (di >= dests.size())return Error{
                Errc::internal, 0,"relocation geometry exhausted destination extents"
            }
            ;
            const auto n = std::min(s.length-soff, dests[di].length-dused);
            r.moves.push_back({
                              {
                              s.start+soff, n
                              }
                              , {
                              dests[di].start+dused, n
                              }
                              }
                             );
            soff+=n;
            dused+=n;
        }
    }
    return r;
}

} // namespace fsx::ext4
