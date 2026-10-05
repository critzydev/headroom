// Lock-free peak meters: the RT thread stores max-since-last-read, the UI reads and resets.
#pragma once

#include <atomic>

namespace hr
{
struct PeakPair
{
    float l = 0.0f;
    float r = 0.0f;
};

// Single writer (RT) / single reader (UI). The UI applies its own decay and peak-hold.
class AtomicPeak
{
public:
    void update (float v) noexcept
    {
        auto current = value.load (std::memory_order_relaxed);

        // CAS rather than load/store so a concurrent readAndReset() can never swallow a larger value.
        while (v > current && ! value.compare_exchange_weak (current, v, std::memory_order_relaxed))
        {}
    }

    float readAndReset() noexcept { return value.exchange (0.0f, std::memory_order_relaxed); }

private:
    std::atomic<float> value { 0.0f };
    static_assert (std::atomic<float>::is_always_lock_free);
};

struct StereoPeak
{
    void update (float pl, float pr) noexcept   { l.update (pl); r.update (pr); }
    PeakPair readAndReset() noexcept            { return { l.readAndReset(), r.readAndReset() }; }

    AtomicPeak l, r;
};

// What the UI gets for one channel: pre-fader input plus post-fader level in each mix.
struct ChannelLevels
{
    PeakPair input, monitor, stream;
};

struct ChannelMeters
{
    ChannelLevels readAndReset() noexcept
    {
        return { input.readAndReset(), monitor.readAndReset(), stream.readAndReset() };
    }

    StereoPeak input, monitor, stream;
};
}
