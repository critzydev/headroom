// Lock-free single-producer / single-consumer FIFO of (pre, post) sample pairs: the RT thread feeds a spectrum analyzer.
#pragma once

#include "Juce.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

namespace hr::dsp
{
// The audio thread calls push(), the UI thread calls read() / clear(). push() never blocks, never allocates and
// drops whatever does not fit (the analyzer would rather miss a few milliseconds than stall the audio).
class AnalyzerFifo
{
public:
    static constexpr int kCapacity = 32768;         // frames: about 0.68 s at 48 kHz (juce::AbstractFifo keeps one of them free)
    static constexpr int kUsable = kCapacity - 1;

    AnalyzerFifo() : fifo (kCapacity), pre ((size_t) kCapacity), post ((size_t) kCapacity) {}

    // RT thread.
    void push (const float* preSamples, const float* postSamples, int numFrames) noexcept
    {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        const int wanted = numFrames;
        const int fits = juce::jmin (numFrames, fifo.getFreeSpace());

        if (fits > 0)
        {
            fifo.prepareToWrite (fits, start1, size1, start2, size2);

            std::copy (preSamples, preSamples + size1, pre.data() + start1);
            std::copy (postSamples, postSamples + size1, post.data() + start1);

            if (size2 > 0)
            {
                std::copy (preSamples + size1, preSamples + size1 + size2, pre.data() + start2);
                std::copy (postSamples + size1, postSamples + size1 + size2, post.data() + start2);
            }

            fifo.finishedWrite (size1 + size2);
        }

        written.fetch_add ((uint64_t) (size1 + size2), std::memory_order_relaxed);

        if (fits < wanted)
            dropped.fetch_add ((uint64_t) (wanted - fits), std::memory_order_relaxed);
    }

    // UI thread. Returns the number of frames copied (at most maxFrames).
    int read (float* preOut, float* postOut, int maxFrames) noexcept
    {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToRead (juce::jmin (maxFrames, fifo.getNumReady()), start1, size1, start2, size2);

        std::copy (pre.data() + start1, pre.data() + start1 + size1, preOut);
        std::copy (post.data() + start1, post.data() + start1 + size1, postOut);

        if (size2 > 0)
        {
            std::copy (pre.data() + start2, pre.data() + start2 + size2, preOut + size1);
            std::copy (post.data() + start2, post.data() + start2 + size2, postOut + size1);
        }

        fifo.finishedRead (size1 + size2);
        return size1 + size2;
    }

    // UI thread: throw away everything that is waiting (a view that was hidden starts from "now").
    void clear() noexcept
    {
        fifo.finishedRead (fifo.getNumReady());
    }

    int getNumReady() const noexcept                    { return fifo.getNumReady(); }
    uint64_t getNumWritten() const noexcept             { return written.load (std::memory_order_relaxed); }
    uint64_t getNumDropped() const noexcept             { return dropped.load (std::memory_order_relaxed); }

private:
    juce::AbstractFifo fifo;
    std::vector<float> pre, post;
    std::atomic<uint64_t> written { 0 }, dropped { 0 };
};
}
