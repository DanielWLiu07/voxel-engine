// Throughput/latency of the lock-free MPMC queue vs a mutex+std::queue pool,
// swept over thread counts and per-item work. Queue cost only matters relative
// to work per item, so the busy-work knob runs from 0 up toward the engine's
// ~1 ms chunk job, where the two converge.

#include "core/mpmc_queue.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// Baseline: mutex + std::queue with non-blocking try_pop, matching the
// lock-free harness (spin on empty rather than condvar sleep).
template <typename T>
class MutexQueue {
public:
    explicit MutexQueue(std::size_t /*capacity hint, unused*/) {}
    bool try_push(T&& v) {
        std::lock_guard<std::mutex> lock(m_);
        q_.push(std::move(v));
        return true;
    }
    bool try_pop(T& out) {
        std::lock_guard<std::mutex> lock(m_);
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop();
        return true;
    }
private:
    std::mutex            m_;
    std::queue<T>         q_;
};

// ~a few ns per unit; just a monotonic knob, not wall-clock accurate.
inline void busy_work(int units, volatile std::uint64_t& sink) {
    std::uint64_t acc = sink;
    for (int i = 0; i < units; ++i) acc = acc * 6364136223846793005ULL + 1442695040888963407ULL;
    sink = acc;
}

struct Result {
    double   items_per_sec;
    double   p50_pop_ns;
    double   p99_pop_ns;
};

template <typename Queue>
Result run(int producers, int consumers, int per_producer, int work_units) {
    const int total = producers * consumers > 0 ? producers * per_producer : 0;
    Queue q(1u << 14);  // 16384-slot ring for the lock-free variant

    std::atomic<int>  consumed{0};
    std::atomic<bool> producers_done{false};
    std::vector<std::vector<double>> pop_latencies(consumers);

    std::vector<std::thread> threads;
    threads.reserve(producers + consumers);

    const auto t0 = Clock::now();

    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&, p] {
            volatile std::uint64_t sink = 0;
            const std::uint64_t base = static_cast<std::uint64_t>(p) * per_producer;
            for (int i = 0; i < per_producer; ++i) {
                std::uint64_t v = base + i;
                while (!q.try_push(std::move(v))) std::this_thread::yield();
                if (work_units) busy_work(work_units, sink);
            }
        });
    }
    for (int c = 0; c < consumers; ++c) {
        threads.emplace_back([&, c] {
            volatile std::uint64_t sink = 0;
            auto& lat = pop_latencies[c];
            lat.reserve(static_cast<std::size_t>(total) / consumers + 64);
            std::uint64_t v;
            for (;;) {
                const auto a = Clock::now();
                if (q.try_pop(v)) {
                    const auto b = Clock::now();
                    lat.push_back(std::chrono::duration<double, std::nano>(b - a).count());
                    if (work_units) busy_work(work_units, sink);
                    consumed.fetch_add(1, std::memory_order_relaxed);
                } else if (producers_done.load(std::memory_order_acquire) &&
                           consumed.load(std::memory_order_acquire) >= total) {
                    return;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (int p = 0; p < producers; ++p) threads[p].join();
    producers_done.store(true, std::memory_order_release);
    for (int c = 0; c < consumers; ++c) threads[producers + c].join();

    const auto t1 = Clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();

    std::vector<double> all;
    for (auto& v : pop_latencies) all.insert(all.end(), v.begin(), v.end());
    std::sort(all.begin(), all.end());
    auto pct = [&](double p) {
        if (all.empty()) return 0.0;
        return all[std::min(all.size() - 1,
                            static_cast<std::size_t>(p * all.size()))];
    };
    return {total / secs, pct(0.50), pct(0.99)};
}

void sweep(int producers, int consumers) {
    constexpr int kPerProducer = 200000;
    const int work_levels[] = {0, 10, 100, 1000};  // ~0ns .. ~order of a few us

    std::printf("\n  %d producers x %d consumers, %d items each\n",
                producers, consumers, kPerProducer);
    std::printf("  %-12s | %14s | %14s | %8s\n",
                "work/item", "mutex M/s", "lockfree M/s", "speedup");
    std::printf("  -------------+----------------+----------------+---------\n");
    for (int w : work_levels) {
        Result m  = run<MutexQueue<std::uint64_t>>(producers, consumers, kPerProducer, w);
        Result lf = run<core::MpmcQueue<std::uint64_t>>(producers, consumers, kPerProducer, w);
        std::printf("  %-12d | %14.2f | %14.2f | %7.2fx\n",
                    w, m.items_per_sec / 1e6, lf.items_per_sec / 1e6,
                    lf.items_per_sec / m.items_per_sec);
    }
}

}  // namespace

int main() {
    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    std::printf("==== queue benchmark: lock-free MPMC vs mutex+std::queue ====\n");
    std::printf("hardware_concurrency=%u\n", hw);

    sweep(1, 1);                                   // SPSC
    sweep(1, static_cast<int>(hw) - 1);            // SPMC: the job-dispatch shape
    sweep(static_cast<int>(hw) - 1, 1);            // MPSC: the result-collect shape
    sweep(static_cast<int>(hw) / 2, static_cast<int>(hw) / 2);  // balanced MPMC

    std::printf(
        "\nwork/item=0 is raw queue throughput (lock-free leads under\n"
        "contention). A chunk job is ~1,000,000 ns; past that the queues match.\n");
    return 0;
}
