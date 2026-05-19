#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace core {

// Minimal fixed-size worker pool. Submit `void()` jobs; they run on a
// background thread. The pool joins all workers on destruction; pending
// jobs are dropped.
class ThreadPool {
public:
    explicit ThreadPool(std::size_t worker_count);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void submit(std::function<void()> job);
    std::size_t worker_count() const { return workers_.size(); }

private:
    void worker_loop();

    std::vector<std::thread>         workers_;
    std::queue<std::function<void()>> jobs_;
    std::mutex                       mutex_;
    std::condition_variable          cv_;
    std::atomic<bool>                stop_{false};
};

}  // namespace core
