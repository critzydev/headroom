// ChannelProcessor: RT-safe trim, FX and per-mix gain staging for one channel.
#include "engine/ChannelProcessor.h"
#include "engine/dsp/Sanitise.h"

namespace hr
{
void ChannelProcessor::prepare (double sampleRate)
{
    for (auto& g : mixGain)
        g.prepare (sampleRate);

    trim.prepare (sampleRate);
    snapPending.store (true, std::memory_order_relaxed);
}

void ChannelProcessor::setActive (bool shouldBeActive) noexcept
{
    if (shouldBeActive && ! active.load (std::memory_order_relaxed))
        snapPending.store (true, std::memory_order_relaxed);

    active.store (shouldBeActive, std::memory_order_relaxed);
}

bool ChannelProcessor::process (const float* inL, const float* inR, int n) noexcept
{
    jassert (n <= kMaxBlock);

    if (! active.load (std::memory_order_relaxed))
        return false;

    auto* fx = chain.load();
    const bool hasFx = fx != nullptr && ! fx->isEmpty();

    if (inL == nullptr && inR == nullptr && ! hasFx)
        return false;

    if (snapPending.exchange (false, std::memory_order_relaxed))
    {
        trim.snapTo (inputTrim.load (std::memory_order_relaxed));

        for (int m = 0; m < kNumMixes; ++m)
            mixGain[m].snapTo (muted[m].load (std::memory_order_relaxed) ? 0.0f : gain[m].load (std::memory_order_relaxed));
    }

    // Input trim (smoothed), copied into the work buffers. A null side is silence.
    trim.setTarget (inputTrim.load (std::memory_order_relaxed));
    const bool trimRamping = trim.fillRamp (ramp.data(), n);
    const float trimConstant = trim.getTarget();

    const float* sources[2] { inL, inR };

    for (int c = 0; c < 2; ++c)
    {
        auto* dst = work[c].data();
        const auto* src = sources[c];

        if (src == nullptr)
            juce::FloatVectorOperations::clear (dst, n);
        else if (trimRamping)
            juce::FloatVectorOperations::multiply (dst, src, ramp.data(), n);
        else if (trimConstant == 1.0f)
            juce::FloatVectorOperations::copy (dst, src, n);
        else
            juce::FloatVectorOperations::multiply (dst, src, trimConstant, n);
    }

    // A NaN/Inf from an app (or a huge value) must not reach the FX chain or the mix.
    dsp::sanitise (work[0].data(), n);
    dsp::sanitise (work[1].data(), n);

    meters.input.update (peakOf (work[0].data(), n), peakOf (work[1].data(), n));

    if (hasFx)
        fx->process (work[0].data(), work[1].data(), n);

    return true;
}

void ChannelProcessor::addToMix (Mix mix, float* dstL, float* dstR, int n) noexcept
{
    const int m = (int) mix;
    auto& g = mixGain[m];

    g.setTarget (muted[m].load (std::memory_order_relaxed) ? 0.0f : gain[m].load (std::memory_order_relaxed));

    const auto* srcL = work[0].data();
    const auto* srcR = work[1].data();
    float peakL = 0.0f, peakR = 0.0f;

    if (g.fillRamp (ramp.data(), n))
    {
        for (int i = 0; i < n; ++i)
        {
            const float l = srcL[i] * ramp[(size_t) i];
            const float r = srcR[i] * ramp[(size_t) i];

            dstL[i] += l;
            dstR[i] += r;
            peakL = juce::jmax (peakL, std::abs (l));
            peakR = juce::jmax (peakR, std::abs (r));
        }
    }
    else
    {
        const float target = g.getTarget();

        if (target == 0.0f)
            return;     // muted: contributes nothing and meters read zero

        for (int i = 0; i < n; ++i)
        {
            const float l = srcL[i] * target;
            const float r = srcR[i] * target;

            dstL[i] += l;
            dstR[i] += r;
            peakL = juce::jmax (peakL, std::abs (l));
            peakR = juce::jmax (peakR, std::abs (r));
        }
    }

    (mix == Mix::Monitor ? meters.monitor : meters.stream).update (peakL, peakR);
}
}
