#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace core {

// Bounded, lock-free multi-producer / multi-consumer queue.
//
// This is Dmitry Vyukov's bounded MPMC algorithm. Every cell in the ring
// carries a sequence counter; producers and consumers advance shared
// enqueue/dequeue positions with a single compare_exchange and publish their
// write/read by storing the next expected sequence. No mutex, no blocking:
// try_push/try_pop are wait-free for the contending CAS and never put a thread
// to sleep. A full queue fails the push; an empty queue fails the pop. That is
// the whole point for a render loop — the main thread drains what is ready and
// moves on instead of ever waiting on a worker.
//
// Capacity must be a power of two (the mask turns the modulo into an AND).
// T must be default-constructible and nothrow-move-assignable; the ring stores
// elements by value, so the queue's footprint is fixed at construction
// (capacity * sizeof(Cell)) — bounded memory, which is what we want under
// infinite chunk streaming.
//
// Memory ordering follows the canonical proof: acquire on the cell sequence
// load pairs with the release store from the other side, so the data write in
// a producer happens-before the data read in the consumer that observes the
// published sequence. The position CAS itself only needs relaxed ordering —
// correctness rides on the per-cell sequence, not the position counter.
template <typename T>
class MpmcQueue {
public:
    explicit MpmcQueue(std::size_t capacity)
        : buffer_(capacity), capacity_mask_(capacity - 1) {
        assert(capacity >= 2 && (capacity & (capacity - 1)) == 0 &&
               "MpmcQueue capacity must be a power of two");
        for (std::size_t i = 0; i < capacity; ++i) {
            buffer_[i].seq.store(i, std::memory_order_relaxed);
        }
        enqueue_pos_.store(0, std::memory_order_relaxed);
        dequeue_pos_.store(0, std::memory_order_relaxed);
    }

    MpmcQueue(const MpmcQueue&)            = delete;
    MpmcQueue& operator=(const MpmcQueue&) = delete;

    // Returns false if the queue is full. Never blocks.
    bool try_push(T&& item) {
        Cell* cell;
        std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &buffer_[pos & capacity_mask_];
            const std::size_t seq = cell->seq.load(std::memory_order_acquire);
            const std::intptr_t diff =
                static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
            if (diff == 0) {
                // Cell is free for this lap; claim the slot.
                if (enqueue_pos_.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;  // producer lapped the consumer: queue is full
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);  // retry
            }
        }
        cell->data = std::move(item);
        cell->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    // Returns false if the queue is empty. Never blocks.
    bool try_pop(T& out) {
        Cell* cell;
        std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &buffer_[pos & capacity_mask_];
            const std::size_t seq = cell->seq.load(std::memory_order_acquire);
            const std::intptr_t diff = static_cast<std::intptr_t>(seq) -
                                       static_cast<std::intptr_t>(pos + 1);
            if (diff == 0) {
                if (dequeue_pos_.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;  // consumer caught the producer: queue is empty
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);  // retry
            }
        }
        out = std::move(cell->data);
        // Free the cell for the next lap (capacity_mask_ + 1 == capacity).
        cell->seq.store(pos + capacity_mask_ + 1, std::memory_order_release);
        return true;
    }

private:
    // Apple silicon uses 128-byte cache lines; pad to that so the producer and
    // consumer position counters never share a line (false sharing would erase
    // the lock-free win under contention).
    static constexpr std::size_t kCacheLine = 128;

    struct Cell {
        std::atomic<std::size_t> seq;
        T                        data{};
    };

    std::vector<Cell> buffer_;
    const std::size_t capacity_mask_;
    alignas(kCacheLine) std::atomic<std::size_t> enqueue_pos_;
    alignas(kCacheLine) std::atomic<std::size_t> dequeue_pos_;
};

}  // namespace core
