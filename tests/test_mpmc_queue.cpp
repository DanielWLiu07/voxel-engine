// Correctness tests for the lock-free bounded MPMC queue (core/mpmc_queue.h).
// Single-threaded FIFO + full/empty semantics, then a multi-producer /
// multi-consumer stress run that asserts every produced item is consumed
// exactly once (no loss, no duplication). Run via ctest after a build.
//
// Minimal harness, same shape as tests/test_world.cpp: one EXPECT macro,
// a failure counter, a main that reports pass/fail.

#include "core/mpmc_queue.h"

#include <atomic>
#include <cstdio>
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
    // The live result queue carries move-only-ish heavy values; make sure the
    // queue moves rather than copies.
    core::MpmcQueue<std::vector<int>> q(2);
    std::vector<int> in{1, 2, 3};
    EXPECT(q.try_push(std::move(in)), "push vector");
    EXPECT(in.empty(), "source vector was moved-from");
    std::vector<int> out;
    EXPECT(q.try_pop(out) && out.size() == 3, "popped vector intact");
}

// Multi-producer / multi-consumer stress: P producers each push a disjoint
// range of integers, C consumers drain until the expected count is reached.
// Every integer must come out exactly once.
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
                while (!q.try_push(std::move(v))) {
                    std::this_thread::yield();  // queue full: spin until space
                }
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

}  // namespace

int main() {
    std::printf("mpmc_queue_tests: running...\n\n");
    test_single_thread_fifo();
    test_move_only_payload();
    test_mpmc_no_loss_no_dup();

    std::printf("\nmpmc_queue_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
