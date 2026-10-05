// LookaheadLimiter: see the header for the guarantee it gives.
#include "engine/dsp/LookaheadLimiter.h"

#include <algorithm>
#include <cmath>

namespace hr::dsp
{
void LookaheadLimiter::prepare (double newSampleRate, double lookaheadMs)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    lookahead = std::max (1, (int) std::lround (lookaheadMs * 0.001 * sampleRate));
    window = lookahead + 1;

    delayL.assign ((size_t) lookahead, 0.0f);
    delayR.assign ((size_t) lookahead, 0.0f);

    dequeCapacity = window + 1;
    dequeValue.assign ((size_t) dequeCapacity, 1.0f);
    dequeStamp.assign ((size_t) dequeCapacity, 0);
    averageRing.assign ((size_t) window, 1.0f);

    updateCoefficients();
    reset();
}

void LookaheadLimiter::reset() noexcept
{
    std::fill (delayL.begin(), delayL.end(), 0.0f);
    std::fill (delayR.begin(), delayR.end(), 0.0f);
    std::fill (averageRing.begin(), averageRing.end(), 1.0f);

    delayPos = 0;
    dequeHead = 0;
    dequeCount = 0;
    sampleIndex = 0;
    averagePos = 0;
    averageSum = (double) window;
    deficit = 0.0f;
    lastBlockMinGain = 1.0f;
    // activeAmount is left alone on purpose: a reset must not make the protection blink.
}

void LookaheadLimiter::setCeilingDb (float db) noexcept
{
    ceiling = std::pow (10.0f, db * 0.05f);
}

void LookaheadLimiter::setReleaseMs (float ms) noexcept
{
    releaseMs = std::max (0.5f, ms);
    updateCoefficients();
}

void LookaheadLimiter::updateCoefficients() noexcept
{
    releaseCoeff = (float) std::exp (-1.0 / (0.001 * (double) releaseMs * sampleRate));
    activeCoeff = (float) std::exp (-1.0 / (0.005 * sampleRate));       // ~5 ms to (dis)engage
}

void LookaheadLimiter::process (float* l, float* r, int numSamples, bool active) noexcept
{
    if (lookahead <= 0 || numSamples <= 0)
        return;

    const float activeTarget = active ? 1.0f : 0.0f;
    float blockMin = 1.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        const float inL = l[i];
        const float inR = r != nullptr ? r[i] : inL;
        const float peak = std::max (std::fabs (inL), std::fabs (inR));
        const float required = peak > ceiling ? ceiling / peak : 1.0f;

        // Sliding-window minimum over the last `window` required gains, this sample included.
        while (dequeCount > 0)
        {
            int back = dequeHead + dequeCount - 1;
            if (back >= dequeCapacity)
                back -= dequeCapacity;

            if (dequeValue[(size_t) back] < required)
                break;

            --dequeCount;
        }

        {
            int slot = dequeHead + dequeCount;
            if (slot >= dequeCapacity)
                slot -= dequeCapacity;

            dequeValue[(size_t) slot] = required;
            dequeStamp[(size_t) slot] = sampleIndex;
            ++dequeCount;
        }

        while (dequeStamp[(size_t) dequeHead] < sampleIndex - lookahead)
        {
            if (++dequeHead >= dequeCapacity)
                dequeHead = 0;

            --dequeCount;
        }

        const float windowMin = dequeValue[(size_t) dequeHead];
        ++sampleIndex;

        // Moving average of the window minimum. Re-summed every `window` samples so rounding cannot drift.
        averageSum += (double) windowMin - (double) averageRing[(size_t) averagePos];
        averageRing[(size_t) averagePos] = windowMin;

        if (++averagePos >= window)
        {
            averagePos = 0;
            averageSum = 0.0;

            for (float v : averageRing)
                averageSum += (double) v;
        }

        // Everything below works on the deficit d = 1 - gain: near unity a float gain cannot represent the tiny steps of
        // an exponential release (it would stall a few 1e-5 short of 1.0), while a small deficit keeps full precision.
        const float targetDeficit = std::max (0.0f, (float) (((double) window - averageSum) / (double) window));

        // Release: follow the required reduction instantly (the moving average already ramps it), recover exponentially.
        if (targetDeficit > deficit)
            deficit = targetDeficit;
        else
            deficit = targetDeficit + (deficit - targetDeficit) * releaseCoeff;

        if (targetDeficit < 1.0e-9f && deficit < 1.0e-9f)
            deficit = 0.0f;                              // exact unity when idle: bit-transparent

        activeAmount += (activeTarget - activeAmount) * (1.0f - activeCoeff);

        if (activeTarget == 1.0f && activeAmount > 0.99999f)
            activeAmount = 1.0f;
        else if (activeTarget == 0.0f && activeAmount < 0.00001f)
            activeAmount = 0.0f;

        const float gain = 1.0f - deficit * activeAmount;
        blockMin = std::min (blockMin, gain);

        // Delay line: read the oldest sample, store the newest.
        const size_t pos = (size_t) delayPos;
        float outL = delayL[pos];
        delayL[pos] = inL;

        if (r != nullptr)
        {
            float outR = delayR[pos];
            delayR[pos] = inR;

            outL *= gain;
            outR *= gain;

            if (activeAmount >= 1.0f)
            {
                outL = std::max (-ceiling, std::min (ceiling, outL));
                outR = std::max (-ceiling, std::min (ceiling, outR));
            }

            l[i] = outL;
            r[i] = outR;
        }
        else
        {
            outL *= gain;

            if (activeAmount >= 1.0f)
                outL = std::max (-ceiling, std::min (ceiling, outL));

            l[i] = outL;
        }

        if (++delayPos >= lookahead)
            delayPos = 0;
    }

    lastBlockMinGain = blockMin;
}
}
