#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace core {

// Bounded lock-free MPMC queue (Vyukov). Each ring cell has a sequence
// counter; producers/consumers claim a slot with one CAS and publish by
// storing the next sequence. try_push/try_pop never block: a full queue fails
// the push, an empty queue fails the pop.
//
// Capacity must be a power of two. T must be default-constructible and
// move-assignable; cells store T by value, so memory is fixed at construction.
//
// The acquire load on cell.seq pairs with the release store on the other side,
// so the data write happens-before the matching read. The position CAS is
// relaxed; correctness comes from the per-cell sequence.
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
    // 128-byte cache line (Apple silicon) so the two position counters don't
    // false-share.
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
