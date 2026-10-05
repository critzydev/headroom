// DenoiseChain implementation: resampling, dry-path alignment and the wet/dry blend around a Denoiser48.
#include "engine/denoise/DenoiseChain.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace hr::denoise
{
namespace
{
    constexpr double kNativeRate = 48000.0;

    uint32_t nextPowerOfTwo (uint32_t v) noexcept
    {
        uint32_t p = 1;

        while (p < v)
            p <<= 1;

        return p;
    }
}

DenoiseChain::DenoiseChain (Kind kindToUse, std::unique_ptr<Denoiser48> engineToUse, double rate)
    : chainKind (kindToUse),
      engine (std::move (engineToUse)),
      hostRate (rate),
      resampled (std::abs (rate - kNativeRate) >= 0.5)
{
    jassert (engine != nullptr && rate >= 8000.0 && rate <= 192000.0);

    const int engineLatency = engine->latency();
    int maxNative = kMaxChunk, maxHostWet = 0;

    if (! resampled)
    {
        latencyHost = engineLatency;
    }
    else
    {
        const double ratio = rate / kNativeRate;
        toNative.prepare (rate, kNativeRate, 2, kMaxChunk);
        fromNative.prepare (kNativeRate, rate, 2, toNative.maxOutputFor (kMaxChunk));

        maxNative = toNative.maxOutputFor (kMaxChunk);
        maxHostWet = fromNative.maxOutputFor (maxNative);

        // Each converter needs its half-kernel of look-ahead before it can emit; the FIFO is pre-filled with that
        // much silence (plus a little margin) so a cycle never finds it empty. That pre-fill is delay.
        priming = (int) std::ceil ((double) toNative.getHalfWidth() + (double) fromNative.getHalfWidth() * ratio) + 2;
        latencyHost = (int) std::lround ((double) engineLatency * ratio) + priming;
    }

    dryMask = nextPowerOfTwo ((uint32_t) (latencyHost + kMaxChunk + 64)) - 1;
    wetMask = nextPowerOfTwo ((uint32_t) (priming + 2 * kMaxChunk + 64)) - 1;
    wetWrite = (uint32_t) priming;

    for (auto& lane : lanes)
    {
        lane.dry.assign (dryMask + 1, 0.0f);
        lane.wet.assign ((size_t) kMaxChunk, 0.0f);

        if (resampled)
        {
            lane.wetFifo.assign (wetMask + 1, 0.0f);
            lane.scratch.assign ((size_t) std::max (kMaxChunk, maxNative) + 8, 0.0f);
            lane.hostWet.assign ((size_t) maxHostWet + 8, 0.0f);
        }
    }
}

// The second lane is about to differ from the first: give it the first one's exact history.
void DenoiseChain::primeSecondLane() noexcept
{
    auto& a = lanes[0];
    auto& b = lanes[1];

    std::copy (a.dry.begin(), a.dry.end(), b.dry.begin());
    std::copy (a.wetFifo.begin(), a.wetFifo.end(), b.wetFifo.begin());

    if (resampled)
    {
        toNative.copyState (0, 1);
        fromNative.copyState (0, 1);
    }
}

void DenoiseChain::process (const float* const* in, float* const* out, float* const* dryOut, int numLanes, int n,
                            const float* dryMixRamp, float dryMixConstant) noexcept
{
    jassert (n > 0 && n <= kMaxChunk);
    numLanes = std::clamp (numLanes, 1, kMaxLanes);

    const bool stereo = numLanes == 2;

    if (stereo && ! wasStereo)
        primeSecondLane();

    wasStereo = stereo;

    // Dry path: remember the input, it comes back `latencyHost` samples later.
    for (int c = 0; c < numLanes; ++c)
    {
        auto* ring = lanes[(size_t) c].dry.data();

        for (int i = 0; i < n; ++i)
            ring[(dryPos + (uint32_t) i) & dryMask] = in[c][i];
    }

    // Wet path.
    float* wet[kMaxLanes] { lanes[0].wet.data(), lanes[1].wet.data() };

    if (! resampled)
    {
        engine->process (in, wet, numLanes, n);
    }
    else
    {
        float* scratch[kMaxLanes] { lanes[0].scratch.data(), lanes[1].scratch.data() };
        int converted = 0;

        for (int c = 0; c < numLanes; ++c)
            converted = toNative.process (c, in[c], n, scratch[c]);

        engine->process (scratch, scratch, numLanes, converted);

        int produced = 0;

        for (int c = 0; c < numLanes; ++c)
        {
            auto& lane = lanes[(size_t) c];
            produced = fromNative.process (c, scratch[c], converted, lane.hostWet.data());

            for (int i = 0; i < produced; ++i)
                lane.wetFifo[(wetWrite + (uint32_t) i) & wetMask] = lane.hostWet[(size_t) i];
        }

        wetWrite += (uint32_t) produced;

        const int available = (int) (wetWrite - wetRead);
        const int take = std::min (n, available);

        if (take < n)
            ++underruns;

        for (int c = 0; c < numLanes; ++c)
        {
            auto& lane = lanes[(size_t) c];

            for (int i = 0; i < take; ++i)
                lane.wet[(size_t) i] = lane.wetFifo[(wetRead + (uint32_t) i) & wetMask];

            for (int i = take; i < n; ++i)
                lane.wet[(size_t) i] = 0.0f;
        }

        wetRead += (uint32_t) take;
    }

    // Blend and the aligned dry signal.
    const bool constantMix = dryMixRamp == nullptr;

    for (int c = 0; c < numLanes; ++c)
    {
        const auto& lane = lanes[(size_t) c];
        const float* ring = lane.dry.data();
        const float* w = lane.wet.data();
        float* dst = out[c];
        float* dryDst = dryOut[c];

        for (int i = 0; i < n; ++i)
            dryDst[i] = ring[(dryPos + (uint32_t) i - (uint32_t) latencyHost) & dryMask];

        if (constantMix && dryMixConstant >= 1.0f)          // exactly the input, delayed (strength 0 %)
            std::memcpy (dst, dryDst, (size_t) n * sizeof (float));
        else if (constantMix && dryMixConstant <= 0.0f)
            std::memcpy (dst, w, (size_t) n * sizeof (float));
        else
            for (int i = 0; i < n; ++i)
                dst[i] = w[i] + (dryDst[i] - w[i]) * (constantMix ? dryMixConstant : dryMixRamp[i]);
    }

    dryPos += (uint32_t) n;
}
}
