#include "fsx/io_scheduler.hpp"
#include "fsx/cancel.hpp"
#include <algorithm>
#include <cerrno>
#include <fstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/sysmacros.h>
#endif

namespace fsx {
namespace {
std::size_t sane_workers(DeviceClass c) {
    const unsigned hc = std::max(1U, std::thread::hardware_concurrency());
    switch (c) {
    case DeviceClass::hdd: return 2;
    case DeviceClass::ssd: return std::min<std::size_t>(8, hc);
    case DeviceClass::nvme: return std::min<std::size_t>(16, hc);
    case DeviceClass::memory: return std::min<std::size_t>(8, hc);
    case DeviceClass::unknown: return std::min<std::size_t>(4, hc);
    }
    return 2;
}

#if defined(__linux__)
DeviceClass linux_sysfs_class(const BlockDevice& dev) {
    struct stat st{};
    if (::fstat(dev.native_fd(), &st) != 0 || !S_ISBLK(st.st_mode)) return DeviceClass::unknown;
    const unsigned maj = major(st.st_rdev);
    const unsigned min = minor(st.st_rdev);
    std::string path = "/sys/dev/block/" + std::to_string(maj) + ":" + std::to_string(min) + "/queue/rotational";
    std::ifstream f(path);
    int rotational = -1;
    if (f >> rotational) return rotational == 1 ? DeviceClass::hdd : DeviceClass::ssd;
    return DeviceClass::unknown;
}
#endif
}

const char* device_class_name(DeviceClass c) noexcept {
    switch (c) {
    case DeviceClass::hdd: return "hdd";
    case DeviceClass::ssd: return "ssd";
    case DeviceClass::nvme: return "nvme";
    case DeviceClass::memory: return "memory";
    default: return "unknown";
    }
}

Result<DeviceProfile> profile_device(const BlockDevice& dev) {
    DeviceProfile p;
    if (!dev.geometry().is_block_device) {
        p.device_class = DeviceClass::memory;
        p.rationale = "regular file or non-block device";
    } else {
        const auto& path = dev.path();
        if (path.find("nvme") != std::string::npos
            || path.find("/nda") != std::string::npos || path.find("/nvd") != std::string::npos) {
            p.device_class = DeviceClass::nvme;
            p.rationale = "NVMe-style device path";
        }
#if defined(__linux__)
        if (p.device_class == DeviceClass::unknown) {
            p.device_class = linux_sysfs_class(dev);
            if (p.device_class != DeviceClass::unknown) p.rationale = "Linux rotational queue attribute";
        }
#endif
        if (p.device_class == DeviceClass::unknown) p.rationale = "portable conservative fallback";
    }
    p.tuning.workers = sane_workers(p.device_class);
    switch (p.device_class) {
    case DeviceClass::hdd:
        p.tuning.queue_depth = 4;
        p.tuning.large_io_bytes = 4U * 1024U * 1024U;
        break;
    case DeviceClass::nvme:
        p.tuning.queue_depth = 64;
        p.tuning.large_io_bytes = 16U * 1024U * 1024U;
        break;
    case DeviceClass::ssd:
        p.tuning.queue_depth = 32;
        p.tuning.large_io_bytes = 8U * 1024U * 1024U;
        break;
    default:
        p.tuning.queue_depth = 16;
        p.tuning.large_io_bytes = 4U * 1024U * 1024U;
        break;
    }
    return p;
}

AsyncIoScheduler::AsyncIoScheduler(IoTuning tuning) : tuning_(tuning) {
    if (tuning_.workers == 0) tuning_.workers = 1;
    if (tuning_.queue_depth < tuning_.workers) tuning_.queue_depth = tuning_.workers;
    workers_.reserve(tuning_.workers);
    for (std::size_t i = 0; i < tuning_.workers; ++i) workers_.emplace_back([this]{ worker_loop(); });
}

AsyncIoScheduler::~AsyncIoScheduler() {
    stop();
}

bool AsyncIoScheduler::enqueue(std::function<void()> fn) {
    std::unique_lock lock(mu_);
    space_cv_.wait(lock, [this]{ return stopping_ || queue_.size() < tuning_.queue_depth; });
    if (stopping_) return false;
    queue_.push_back(std::move(fn));
    cv_.notify_one();
    return true;
}

std::future<Result<std::vector<std::byte>>> AsyncIoScheduler::submit_read(
    const BlockDevice& dev,
    std::uint64_t offset,
    std::size_t length) {
    auto task = std::make_shared<std::packaged_task<Result<std::vector<std::byte>>()>>(
        [this, &dev, offset, length]() -> Result<std::vector<std::byte>> {
            if (Cancellation::requested()) {
                return Error{Errc::unsafe, 0, "operation cancelled"};
            }

            std::vector<std::byte> data(length);
            auto r = dev.read_exact(offset, data);
            if (!r) return r.error();

            read_ops_.fetch_add(1, std::memory_order_relaxed);
            bytes_read_.fetch_add(length, std::memory_order_relaxed);
            return data;
        });

    auto future = task->get_future();
    if (!enqueue([task] { (*task)(); })) {
        std::promise<Result<std::vector<std::byte>>> rejected;
        auto rejected_future = rejected.get_future();
        rejected.set_value(Error{Errc::unsafe, 0, "I/O scheduler is stopped"});
        return rejected_future;
    }
    return future;
}

std::future<Result<void>> AsyncIoScheduler::submit_write(
    BlockDevice& dev,
    std::uint64_t offset,
    std::vector<std::byte> data) {
    auto task = std::make_shared<std::packaged_task<Result<void>()>>(
        [this, &dev, offset, data = std::move(data)]() mutable -> Result<void> {
            if (Cancellation::requested()) {
                return Error{Errc::unsafe, 0, "operation cancelled"};
            }

            auto r = dev.write_exact(offset, data);
            if (!r) return r.error();

            write_ops_.fetch_add(1, std::memory_order_relaxed);
            bytes_written_.fetch_add(data.size(), std::memory_order_relaxed);
            return {};
        });

    auto future = task->get_future();
    if (!enqueue([task] { (*task)(); })) {
        std::promise<Result<void>> rejected;
        auto rejected_future = rejected.get_future();
        rejected.set_value(Error{Errc::unsafe, 0, "I/O scheduler is stopped"});
        return rejected_future;
    }
    return future;
}

void AsyncIoScheduler::worker_loop() {
    for (; ; ) {
        std::function<void()> fn;
        {
            std::unique_lock lock(mu_);
            cv_.wait(lock, [this]{ return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty()) return;
            fn = std::move(queue_.front());
            queue_.pop_front();
            ++active_;
            space_cv_.notify_one();
        }
        try {
            fn();
        } catch (...) {
            // submit_* uses packaged_task, which captures task exceptions. This
            // final guard prevents a future internal queue user from killing a
            // worker thread and silently reducing I/O capacity.
        }
        {
            std::lock_guard lock(mu_);
            --active_;
            if (queue_.empty() && active_ == 0) idle_cv_.notify_all();
        }
    }
}

void AsyncIoScheduler::drain() {
    std::unique_lock lock(mu_);
    idle_cv_.wait(lock, [this]{ return queue_.empty() && active_ == 0; });
}

void AsyncIoScheduler::stop() {
    {
        std::lock_guard lock(mu_);
        if (stopping_) return;
        stopping_ = true;
    }
    cv_.notify_all();
    space_cv_.notify_all();
    for (auto& t : workers_) if (t.joinable()) t.join();
    workers_.clear();
}

IoStats AsyncIoScheduler::stats() const noexcept {
    return {read_ops_.load(std::memory_order_relaxed),
        write_ops_.load(std::memory_order_relaxed),
        bytes_read_.load(std::memory_order_relaxed),
        bytes_written_.load(std::memory_order_relaxed)};
}

} // namespace fsx
