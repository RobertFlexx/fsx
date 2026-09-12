#pragma once
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace fsx {

// Small bounded fixed-size worker pool used for CPU work and compound I/O tasks.
// The queue is deliberately bounded: storage operations must not turn a large
// extent list into unbounded heap growth or millions of outstanding requests.
class TaskPool {
public:
    explicit TaskPool(std::size_t workers, std::size_t queue_limit = 0)
        : queue_limit_(queue_limit ? queue_limit : (workers ? workers * 4U : 4U)) {
            if (workers == 0) workers = 1;
            workers_.reserve(workers);
            for (std::size_t i = 0; i < workers; ++i)
                workers_.emplace_back([this] {
                                      worker_loop(); });
        }

    TaskPool(const TaskPool&) = delete;
    TaskPool& operator = (const TaskPool&) = delete;

    ~TaskPool() {
        {
            std::lock_guard lock(mu_);
            stopping_ = true;
        }
        have_work_.notify_all();
        have_space_.notify_all();
        for (auto& t : workers_) if (t.joinable()) t.join();
    }

    template<class F>
        auto submit(F && fn) -> std::future<std::invoke_result_t<F>> {
            using R = std::invoke_result_t<F>;
            auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(fn));
            auto future = task->get_future();
            {
                std::unique_lock lock(mu_);
                have_space_.wait(lock, [this] {
                                 return stopping_ || queue_.size() < queue_limit_; });
                if (stopping_) throw std::runtime_error("task submitted to stopped TaskPool");
                queue_.emplace_back([task] {
                                    (*task)(); });
            }
            have_work_.notify_one();
            return future;
        }

    std::size_t worker_count() const noexcept { return workers_.size();
    }

private:
    void worker_loop() noexcept {
        for (; ; ) {
            std::function<void()> task;
            {
                std::unique_lock lock(mu_);
                have_work_.wait(lock, [this] {
                                return stopping_ || !queue_.empty(); });
                if (stopping_ && queue_.empty()) return;
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            have_space_.notify_one();
            try {
                task();
            } catch (...) {
                /* packaged_task stores exceptions */ }
        }
    }

    std::vector<std::thread> workers_;
    std::deque<std::function<void()>> queue_;
    std::size_t queue_limit_{};
    std::mutex mu_;
    std::condition_variable have_work_;
    std::condition_variable have_space_;
    bool stopping_{false};
};

} // namespace fsx
