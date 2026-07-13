#pragma once
#include <atomic>
#include <cstddef>
#include <vector>

namespace lob {

// Single-producer / single-consumer lock-free ring buffer.
//
// This is the feed-handler -> book handoff: one thread parses market data and
// pushes normalized messages, one thread pops them and applies them to the book.
// No locks, no allocation after construction.
//
// Correctness rests on the memory ordering:
//   - the producer owns tail_, the consumer owns head_
//   - push writes the slot, then publishes it with tail_.store(release)
//   - pop reads tail_ with acquire (pairs with that release) so it never sees the
//     slot before the write is visible; likewise head_ release/acquire the other way
//   - head_ and tail_ sit on separate cache lines (alignas 64) to avoid false sharing
//
// Capacity must be a power of two; one slot is left empty to distinguish full/empty.
template <class T>
class SpscRing {
public:
    explicit SpscRing(std::size_t capacity_pow2)
        : mask_(capacity_pow2 - 1), buf_(capacity_pow2) {}

    // Producer side. Returns false if full (caller decides to spin/drop).
    bool push(const T& v) {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        const std::size_t next = (t + 1) & mask_;
        if (next == head_.load(std::memory_order_acquire)) return false;  // full
        buf_[t] = v;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns false if empty.
    bool pop(T& out) {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_.load(std::memory_order_acquire)) return false;     // empty
        out = buf_[h];
        head_.store((h + 1) & mask_, std::memory_order_release);
        return true;
    }

private:
    alignas(64) std::atomic<std::size_t> head_{0};   // consumer cursor
    alignas(64) std::atomic<std::size_t> tail_{0};   // producer cursor
    std::size_t mask_;
    std::vector<T> buf_;
};

} // namespace lob
