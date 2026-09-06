// MPMC queue and thread pool concurrency tests. Same EXPECT harness as
// test_world.cpp. Also built under TSan / ASan+UBSan in CI.

#include "core/mpmc_queue.h"
#include "core/thread_pool.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;
int g_checks   = 0;

#define EXPECT(cond, label) do {                                            \
    ++g_checks;                                                             \
    if (!(cond)) {                                                          \
        std::printf("  FAIL [%s:%d] %s\n", __FILE__, __LINE__, label);     \
        ++g_failures;                                                       \
    }                                                                       \
} while (0)

void test_single_thread_fifo() {
    core::MpmcQueue<int> q(4);  // power-of-two capacity
    int out = -1;
    EXPECT(!q.try_pop(out), "pop on empty queue fails");

    EXPECT(q.try_push(1), "push 1");
    EXPECT(q.try_push(2), "push 2");
    EXPECT(q.try_push(3), "push 3");
    EXPECT(q.try_push(4), "push 4 fills capacity");
    EXPECT(!q.try_push(5), "push on full queue fails");

    EXPECT(q.try_pop(out) && out == 1, "pop returns 1 (FIFO)");
    EXPECT(q.try_pop(out) && out == 2, "pop returns 2 (FIFO)");
    // Ring should now accept a new push in a freed slot.
    EXPECT(q.try_push(5), "push 5 into freed slot");
    EXPECT(q.try_pop(out) && out == 3, "pop returns 3 (FIFO)");
    EXPECT(q.try_pop(out) && out == 4, "pop returns 4 (FIFO)");
    EXPECT(q.try_pop(out) && out == 5, "pop returns 5 (FIFO)");
    EXPECT(!q.try_pop(out), "queue drained back to empty");
}

void test_move_only_payload() {
    // Heavy payloads must move, not copy.
    core::MpmcQueue<std::vector<int>> q(2);
    std::vector<int> in{1, 2, 3};
    EXPECT(q.try_push(std::move(in)), "push vector");
    EXPECT(in.empty(), "source vector was moved-from");
    std::vector<int> out;
    EXPECT(q.try_pop(out) && out.size() == 3, "popped vector intact");
}

// P producers push disjoint integer ranges, C consumers drain. Every value
// must come out exactly once.
void test_mpmc_no_loss_no_dup() {
    constexpr int kProducers      = 4;
    constexpr int kConsumers      = 4;
    constexpr int kPerProducer    = 50000;
    constexpr int kTotal          = kProducers * kPerProducer;

    core::MpmcQueue<int> q(1024);
    std::vector<std::atomic<int>> seen(kTotal);
    for (auto& s : seen) s.store(0, std::memory_order_relaxed);

    std::atomic<int> consumed{0};
    std::atomic<bool> producers_done{false};

    std::vector<std::thread> threads;
    for (int p = 0; p < kProducers; ++p) {
        threads.emplace_back([&, p] {
            const int base = p * kPerProducer;
            for (int i = 0; i < kPerProducer; ++i) {
                int v = base + i;
                while (!q.try_push(std::move(v))) std::this_thread::yield();
            }
        });
    }
    for (int c = 0; c < kConsumers; ++c) {
        threads.emplace_back([&] {
            int v;
            for (;;) {
                if (q.try_pop(v)) {
                    seen[v].fetch_add(1, std::memory_order_relaxed);
                    consumed.fetch_add(1, std::memory_order_relaxed);
                } else if (producers_done.load(std::memory_order_acquire) &&
                           consumed.load(std::memory_order_acquire) >= kTotal) {
                    return;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (int p = 0; p < kProducers; ++p) threads[p].join();
    producers_done.store(true, std::memory_order_release);
    for (int c = kProducers; c < kProducers + kConsumers; ++c) threads[c].join();

    EXPECT(consumed.load() == kTotal, "consumed exactly the produced count");
    bool all_once = true;
    for (int i = 0; i < kTotal; ++i) {
        if (seen[i].load(std::memory_order_relaxed) != 1) { all_once = false; break; }
    }
    EXPECT(all_once, "every value consumed exactly once (no loss, no dup)");
}

// Concurrent submits from many threads; every job must run exactly once. Wait
// on the counter, not the destructor - dropping the backlog is the
// destructor's job, pinned by test_shutdown_drops_the_queued_backlog below.
void test_thread_pool_runs_every_job_once() {
    constexpr int kSubmitters = 6;
    constexpr int kPerSubmitter = 20000;
    constexpr int kTotal = kSubmitters * kPerSubmitter;

    core::ThreadPool pool(std::thread::hardware_concurrency());
    std::vector<std::atomic<int>> ran(kTotal);
    for (auto& r : ran) r.store(0, std::memory_order_relaxed);
    std::atomic<int> done{0};

    std::vector<std::thread> submitters;
    for (int s = 0; s < kSubmitters; ++s) {
        submitters.emplace_back([&, s] {
            const int base = s * kPerSubmitter;
            for (int i = 0; i < kPerSubmitter; ++i) {
                const int id = base + i;
                pool.submit([&, id] {
                    ran[id].fetch_add(1, std::memory_order_relaxed);
                    done.fetch_add(1, std::memory_order_release);
                });
            }
        });
    }
    for (auto& t : submitters) t.join();
    while (done.load(std::memory_order_acquire) < kTotal) std::this_thread::yield();

    bool each_once = true;
    for (int i = 0; i < kTotal; ++i) {
        if (ran[i].load(std::memory_order_relaxed) != 1) { each_once = false; break; }
    }
    EXPECT(each_once, "thread pool ran every submitted job exactly once");
}

// ----- shutdown ------------------------------------------------------------

// Destruction has to stop the pool, not wait out its backlog. The engine
// queues one job per chunk, so at a streaming radius the queue is hundreds
// deep whenever the window closes; a destructor that drains it turns quit
// into a wait for work whose results nobody will ever read.
//
// Getting the worker to look at the queue at a known moment is the whole
// trick here: job A pins the single worker until after the destructor has
// set its stop flag, so what the worker does next is the thing under test
// rather than a race against it.
void test_shutdown_drops_the_queued_backlog() {
    constexpr int kBacklog = 256;
    std::atomic<bool> occupied{false};
    std::atomic<bool> release{false};
    std::atomic<bool> finished_running_job{false};
    std::atomic<bool> closing{false};
    std::atomic<int>  backlog_ran{0};

    auto pool = std::make_unique<core::ThreadPool>(1);
    pool->submit([&] {
        occupied.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
        finished_running_job.store(true, std::memory_order_release);
    });
    while (!occupied.load(std::memory_order_acquire)) std::this_thread::yield();

    for (int i = 0; i < kBacklog; ++i) {
        pool->submit([&] { backlog_ran.fetch_add(1, std::memory_order_relaxed); });
    }

    std::thread closer([&] {
        closing.store(true, std::memory_order_release);
        pool.reset();  // sets stop_, notifies, joins
    });
    while (!closing.load(std::memory_order_acquire)) std::this_thread::yield();
    // The destructor takes the lock and sets stop_ in the next few
    // instructions, and the worker cannot reach the queue again until
    // release, so this margin only has to cover those.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    release.store(true, std::memory_order_release);
    closer.join();

    EXPECT(finished_running_job.load(std::memory_order_acquire),
           "shutdown lets the job already running finish");
    EXPECT(backlog_ran.load(std::memory_order_relaxed) == 0,
           "shutdown drops the queued backlog instead of draining it");
}

// A pool with no workers is not a special case anywhere in the engine, but
// it is the cheapest way to state the other half of the contract: nothing
// about destruction depends on the queue being empty.
void test_shutdown_of_a_pool_that_never_ran_anything_returns() {
    core::ThreadPool pool(0);
    EXPECT(pool.worker_count() == 0, "a zero-worker pool reports zero workers");
    for (int i = 0; i < 64; ++i) pool.submit([] {});
    EXPECT(true, "destroying a pool with a full queue and no workers returns");
}

void test_worker_count_is_the_count_that_was_asked_for() {
    for (std::size_t n : {1u, 3u, 9u}) {
        core::ThreadPool pool(n);
        EXPECT(pool.worker_count() == n, "worker_count() echoes the request");
    }
}

}  // namespace

int main() {
    std::printf("concurrency_tests: running...\n\n");
    test_single_thread_fifo();
    test_move_only_payload();
    test_mpmc_no_loss_no_dup();
    test_thread_pool_runs_every_job_once();
    test_worker_count_is_the_count_that_was_asked_for();
    test_shutdown_drops_the_queued_backlog();
    test_shutdown_of_a_pool_that_never_ran_anything_returns();

    std::printf("\nconcurrency_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
