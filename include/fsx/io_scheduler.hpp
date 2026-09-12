#pragma once
#include "fsx/device.hpp"
#include "fsx/result.hpp"
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

namespace fsx {

enum class DeviceClass { unknown, hdd, ssd, nvme, memory };

struct IoTuning {
    std::size_t workers{1};
    std::size_t queue_depth{8};
    std::size_t large_io_bytes{4U * 1024U * 1024U};
};

struct DeviceProfile {
    DeviceClass device_class{DeviceClass::unknown};
    IoTuning tuning{};
    std::string rationale;
};

struct IoStats {
    std::uint64_t read_ops{};
    std::uint64_t write_ops{};
    std::uint64_t bytes_read{};
    std::uint64_t bytes_written{};
};

Result<DeviceProfile> profile_device(const BlockDevice& dev);
const char* device_class_name(DeviceClass c) noexcept;

class AsyncIoScheduler {
public:
    explicit AsyncIoScheduler(IoTuning tuning);
    ~AsyncIoScheduler();
    AsyncIoScheduler(const AsyncIoScheduler&) = delete;
    AsyncIoScheduler& operator = (const AsyncIoScheduler&) = delete;

    std::future<Result<std::vector<std::byte>>> submit_read(const BlockDevice& dev,
                                                            std::uint64_t offset,
                                                            std::size_t length);
    std::future<Result<void>> submit_write(BlockDevice& dev,
                                           std::uint64_t offset,
                                           std::vector<std::byte> data);
    void drain();
    void stop();
    IoStats stats() const noexcept;

private:
    void worker_loop();
    bool enqueue(std::function<void()> fn);

    IoTuning tuning_{};
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::condition_variable space_cv_;
    std::condition_variable idle_cv_;
    std::deque<std::function<void()>> queue_;
    std::vector<std::thread> workers_;
    bool stopping_{false};
    std::size_t active_{0};
    std::atomic<std::uint64_t> read_ops_{0};
    std::atomic<std::uint64_t> write_ops_{0};
    std::atomic<std::uint64_t> bytes_read_{0};
    std::atomic<std::uint64_t> bytes_written_{0};
};

} // namespace fsx
