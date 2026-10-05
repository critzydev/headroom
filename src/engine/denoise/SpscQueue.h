// SpscQueue: wait-free single-producer / single-consumer queue of fixed-size items, filled and read in place.
#pragma once

#include <atomic>
#include <cstdint>

#ifdef _MSC_VER
 #pragma warning (push)
 #pragma warning (disable: 4324)        // padded because of alignas: that is the point
#endif

namespace hr::denoise
{
// The producer claims a slot, writes into it and publishes it; the consumer looks at the oldest published slot and
// pops it. Nothing is copied, nothing blocks, nothing allocates. N must be a power of two.
template <typename T, uint32_t N>
class SpscQueue
{
    static_assert ((N & (N - 1)) == 0, "N must be a power of two");
    static_assert (std::atomic<uint32_t>::is_always_lock_free);

public:
    // Producer: the slot to write next, or nullptr if the queue is full.
    T* claim() noexcept
    {
        const auto h = head.load (std::memory_order_relaxed);
        return h - tail.load (std::memory_order_acquire) >= N ? nullptr : &slots[h & (N - 1)];
    }

    // Producer: makes the claimed slot visible.
    void publish() noexcept                     { head.store (head.load (std::memory_order_relaxed) + 1, std::memory_order_release); }

    // Consumer: the oldest published slot, or nullptr if there is none.
    T* peek() noexcept
    {
        const auto t = tail.load (std::memory_order_relaxed);
        return head.load (std::memory_order_acquire) == t ? nullptr : &slots[t & (N - 1)];
    }

    // Consumer: releases the slot peek() returned.
    void pop() noexcept                         { tail.store (tail.load (std::memory_order_relaxed) + 1, std::memory_order_release); }

    uint32_t size() const noexcept              { return head.load (std::memory_order_acquire) - tail.load (std::memory_order_acquire); }

private:
    T slots[N] {};
    alignas (64) std::atomic<uint32_t> head { 0 };
    alignas (64) std::atomic<uint32_t> tail { 0 };
};
}

#ifdef _MSC_VER
 #pragma warning (pop)
#endif
