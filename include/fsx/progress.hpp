#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace fsx {

enum class ProgressMode { automatic, live, plain, none };

struct ProgressSnapshot {
    std::string phase{"idle"};
    std::string detail;
    std::uint64_t done{};
    std::uint64_t total{};
    std::uint64_t bytes_read{};
    std::uint64_t bytes_written{};
    std::uint64_t items_done{};
    std::uint64_t items_total{};
    std::uint64_t bytes_verified{};
    std::uint32_t queue_active{};
    std::uint32_t queue_pending{};
    std::uint64_t transactions{};
};

class Progress {
public:
    explicit Progress(ProgressMode mode = ProgressMode::automatic);
    ~Progress();
    Progress(const Progress&) = delete;
    Progress& operator = (const Progress&) = delete;

    void start();
    void stop(bool success = true);
    void update(const ProgressSnapshot& s);
    ProgressMode mode() const noexcept { return mode_;
    }

private:
    void render_loop();
    void render_live(const ProgressSnapshot& s, double elapsed, double rate);
    void render_plain(const ProgressSnapshot& s, double elapsed, double rate);

    ProgressMode mode_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex mu_;
    ProgressSnapshot snapshot_{};
    std::chrono::steady_clock::time_point started_{};
    std::uint64_t previous_done_{};
    std::chrono::steady_clock::time_point previous_time_{};
    double smoothed_rate_{};
    bool rendered_live_{false};
    std::size_t live_lines_{0};
};

ProgressMode parse_progress_mode(const std::string& s);

} // namespace fsx
