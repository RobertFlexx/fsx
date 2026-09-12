#include "fsx/progress.hpp"
#include "fsx/size.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <sys/ioctl.h>
#include <unistd.h>

namespace fsx {
namespace {
std::size_t terminal_width() {
    struct winsize ws{};
    if (::ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 40)return ws.ws_col;
    return 100;
}
std::string clip(std::string s, std::size_t width) {
    if (s.size() <= width)return s;
    if (width<4)return s.substr(0, width);
    s.resize(width-3);
    s+="...";
    return s;
}
bool live_ok() {
    const char* term = std::getenv("TERM");
    return ::isatty(STDERR_FILENO) && (!term || std::string(term) != "dumb");
}
}
ProgressMode parse_progress_mode(const std::string&s) {
    if (s == "live")return ProgressMode::live;
    if (s == "plain")return ProgressMode::plain;
    if (s == "none")return ProgressMode::none;
    return ProgressMode::automatic;
}
Progress::Progress(ProgressMode mode):mode_(mode) {
    if (mode_ == ProgressMode::automatic)mode_ = live_ok()?ProgressMode::live:ProgressMode::plain;
}
Progress::~Progress() {
    stop(false);
}
void Progress::start() {
    if (mode_ == ProgressMode::none || running_.exchange(true))return;
    started_ = previous_time_ = std::chrono::steady_clock::now();
    thread_ = std::thread([this]{
                          render_loop();
                          }
                         );
}
void Progress::stop(bool success) {
    if (!running_.exchange(false))return;
    if (thread_.joinable())thread_.join();
    if (mode_ == ProgressMode::live && rendered_live_) {
        if (live_lines_>0)std::fprintf(stderr,"\033[%zuA", live_lines_);
        for (std::size_t i = 0; i<live_lines_; ++i)std::fprintf(stderr,"\r\033[2K\n");
        if (live_lines_>0)std::fprintf(stderr,"\033[%zuA", live_lines_);
        std::fprintf(stderr,"status: %s\n", success?"complete":"stopped");
        std::fflush(stderr);
    }
}
void Progress::update(const ProgressSnapshot&s) {
    std::lock_guard lock(mu_);
    snapshot_ = s;
}
void Progress::render_loop() {
    while (running_) {
        ProgressSnapshot s;
        {
            std::lock_guard lock(mu_);
            s = snapshot_;
        }
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now-started_).count();
        double dt = std::chrono::duration<double>(now-previous_time_).count();
        if (dt>0.05) {
            double inst = s.done >= previous_done_?static_cast<double>(s.done-previous_done_)/dt:0.0;
            smoothed_rate_ = smoothed_rate_ == 0?inst:(0.25*inst+0.75*smoothed_rate_);
            previous_done_ = s.done;
            previous_time_ = now;
        }
        if (mode_ == ProgressMode::live)render_live(s, elapsed, smoothed_rate_);
        else if (mode_ == ProgressMode::plain)render_plain(s, elapsed, smoothed_rate_);
        std::this_thread::sleep_for(std::chrono::milliseconds(125));
    }
}
void Progress::render_live(const ProgressSnapshot&s, double elapsed, double rate) {
    const double pct = s.total?100.0*static_cast<double>(s.done)/static_cast<double>(s.total):0.0;
    const double eta = (rate>1.0 && s.total>s.done)?static_cast<double>(s.total-s.done)/rate:0.0;
    const auto width = terminal_width();
    std::vector<std::string> lines;
    lines.reserve(9);
    char buf[512];
    std::snprintf(buf, sizeof(buf),"phase:        %s", s.phase.c_str());
    lines.emplace_back(buf);
    if (!s.detail.empty())lines.emplace_back("detail:       "+s.detail);
    const bool item_progress = s.items_total != 0 && s.done == s.items_done
        && s.total == s.items_total && s.bytes_read == 0 && s.bytes_written == 0;
    if (item_progress) {
        std::snprintf(buf, sizeof(buf),"progress:     %.2f%%  %llu / %llu items", pct,
                      static_cast<unsigned long long>(s.done), static_cast<unsigned long long>(s.total));
        lines.emplace_back(buf);
        std::snprintf(buf, sizeof(buf),"rate:         %.1f items/s", rate);
        lines.emplace_back(buf);
    } else {
        std::snprintf(buf, sizeof(buf),"progress:     %.2f%%  %s / %s",
                      pct, human_bytes(s.done).c_str(), human_bytes(s.total).c_str());
        lines.emplace_back(buf);
        std::snprintf(buf, sizeof(buf),"rate:         %s", human_rate(rate).c_str());
        lines.emplace_back(buf);
    }
    if (s.bytes_read || s.bytes_written) {
        std::snprintf(buf, sizeof(buf),"io:           read %s  written %s",
                      human_bytes(s.bytes_read).c_str(), human_bytes(s.bytes_written).c_str());
        lines.emplace_back(buf);
    }
    if (s.bytes_verified) {
        lines.emplace_back("verified:     "+human_bytes(s.bytes_verified));
    }
    if (s.items_total) {
        std::snprintf(buf, sizeof(buf),"items:        %llu / %llu",
                      static_cast<unsigned long long>(s.items_done), static_cast<unsigned long long>(s.items_total));
        lines.emplace_back(buf);
    }
    if (s.queue_active || s.queue_pending) {
        std::snprintf(buf, sizeof(buf),"queue:        %u active, %u pending", s.queue_active, s.queue_pending);
        lines.emplace_back(buf);
    }
    if (s.transactions) {
        std::snprintf(buf, sizeof(buf),"transactions: %llu", static_cast<unsigned long long>(s.transactions));
        lines.emplace_back(buf);
    }
    std::string timing = "elapsed:      "+format_duration(elapsed);
    if (eta>0.0)timing+="  eta "+format_duration(eta);
    lines.push_back(timing);
    if (rendered_live_ && live_lines_>0)std::fprintf(stderr,"\033[%zuA", live_lines_);
    for (std::size_t i = 0; i<lines.size(); ++i)std::fprintf(stderr,"\r\033[2K%s\n", clip(lines[i], width).c_str());
    if (rendered_live_ && live_lines_>lines.size()) {
        for (std::size_t i = lines.size(); i<live_lines_; ++i)std::fprintf(stderr,"\r\033[2K\n");
        std::fprintf(stderr,"\033[%zuA", live_lines_-lines.size());
    }
    std::fflush(stderr);
    rendered_live_ = true;
    live_lines_ = lines.size();
}
void Progress::render_plain(const ProgressSnapshot&s, double elapsed, double rate) {
    static thread_local double last = -10.0;
    if (elapsed-last<2.0 && s.done<s.total)return;
    last = elapsed;
    double pct = s.total?100.0*static_cast<double>(s.done)/static_cast<double>(s.total):0.0;
    const bool item_progress = s.items_total != 0 && s.done == s.items_done
        && s.total == s.items_total && s.bytes_read == 0 && s.bytes_written == 0;
    std::cerr<<"phase="<<s.phase<<" progress="<<pct<<"% done="<<s.done<<" total="<<s.total;
    if (item_progress)std::cerr<<" rate="<<rate<<"items/s";
    else std::cerr<<" rate="<<static_cast<std::uint64_t>(rate)<<"B/s";
    std::cerr<<" read="<<s.bytes_read<<" written="<<s.bytes_written;
    if (s.items_total)std::cerr<<" items="<<s.items_done<<"/"<<s.items_total;
    std::cerr<<"\n";
}
}
