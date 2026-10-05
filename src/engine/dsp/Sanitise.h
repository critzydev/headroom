// Cheap output guard for FX slots and channel inputs: non-finite samples become 0, everything else is hard-limited to +-4.0.
#pragma once

#include <cmath>
#include <limits>

namespace hr::dsp
{
// Last-resort ceiling (about +12 dBFS). Legitimate float audio never gets near it; a misbehaving plugin does.
constexpr float kSanitiseLimit = 4.0f;

// In place. Returns true if any sample had to be changed (so callers can count misbehaving plugins).
// The fast path (|x| <= limit) is one compare per sample; NaN fails that compare and lands in the slow path.
inline bool sanitise (float* x, int numSamples, float limit = kSanitiseLimit) noexcept
{
    bool changed = false;

    for (int i = 0; i < numSamples; ++i)
    {
        const float v = x[i];
        const float a = std::fabs (v);

        if (a <= limit)
            continue;

        // a > limit or NaN: a finite overshoot is clipped, +-Inf and NaN (not < Inf) are replaced by silence.
        x[i] = a < std::numeric_limits<float>::infinity() ? std::copysign (limit, v) : 0.0f;
        changed = true;
    }

    return changed;
}
}
