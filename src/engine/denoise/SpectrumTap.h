// SpectrumTap: the audio thread's copy of the last few thousand samples (input and output), for the panel's spectrum.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace hr::denoise
{
// The audio thread writes into two rings, but only while `active` is set: nothing at all is written (or even
// touched) for a panel that is not on screen. The UI thread reads the latest window at 30 Hz and does the FFT itself.
//
// Tear-free without a lock: the reader copies a window that ends `kSlack` samples before the write position, and the
// rings are far larger than window + slack, so the audio thread cannot overwrite what is being read.
class SpectrumTap
{
public:
    static constexpr uint32_t kSize = 16384;            // per ring, a power of two
    static constexpr uint32_t kSlack = 1024;

    std::atomic<bool> active { false };
    std::atomic<double> sampleRate { 48000.0 };

    // Audio thread. Two mono signals of n samples (the input aligned with the output, and the output).
    void push (const float* input, const float* output, int n) noexcept
    {
        if (! active.load (std::memory_order_relaxed))
            return;

        const auto start = head.load (std::memory_order_relaxed);

        for (int i = 0; i < n; ++i)
        {
            const auto at = (start + (uint32_t) i) & (kSize - 1);
            inRing[at].store (input[i], std::memory_order_relaxed);
            outRing[at].store (output[i], std::memory_order_relaxed);
        }

        head.store (start + (uint32_t) n, std::memory_order_release);
    }

    // Any thread. Total samples written so far (0 for ever if the tap has never been active).
    uint32_t written() const noexcept                   { return head.load (std::memory_order_acquire); }

    // UI thread. The most recent `count` samples (count <= 8192). False until enough have been written.
    bool read (float* input, float* output, int count) const noexcept
    {
        const auto end = head.load (std::memory_order_acquire);

        if (count <= 0 || (uint32_t) count + kSlack > end)
            return false;

        const auto first = end - kSlack - (uint32_t) count;

        for (int i = 0; i < count; ++i)
        {
            const auto at = (first + (uint32_t) i) & (kSize - 1);
            input[i] = inRing[at].load (std::memory_order_relaxed);
            output[i] = outRing[at].load (std::memory_order_relaxed);
        }

        return true;
    }

private:
    static_assert (std::atomic<float>::is_always_lock_free);

    std::array<std::atomic<float>, kSize> inRing {}, outRing {};
    std::atomic<uint32_t> head { 0 };
};
}
