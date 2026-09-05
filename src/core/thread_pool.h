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

// Fixed-size worker pool.
//
// Destruction stops the pool: jobs already running finish, jobs still
// queued are dropped, and the destructor joins. It deliberately does not
// wait out the backlog, so shutdown costs one job rather than however many
// the caller had queued. Callers that need every job to have run wait on
// their own counter - World tracks jobs_in_flight_ for exactly this.
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

    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> jobs_;
    std::mutex                        mutex_;
    std::condition_variable           cv_;
    std::atomic<bool>                 stop_{false};
};

}  // namespace core
