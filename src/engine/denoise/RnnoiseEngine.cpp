// RnnoiseEngine implementation: frame FIFOs around rnnoise_process_frame and the voice gate.
#include "engine/denoise/RnnoiseEngine.h"

#include <rnnoise.h>

#include <algorithm>
#include <cstring>

namespace hr::denoise
{
namespace
{
    constexpr float kToRnnoise = 32768.0f;
    constexpr float kFromRnnoise = 1.0f / 32768.0f;
    constexpr float kGateOpenStep = 1.0f / (0.005f * 48000.0f);     // 5 ms
    constexpr float kGateCloseStep = 1.0f / (0.060f * 48000.0f);    // 60 ms
}

std::unique_ptr<RnnoiseEngine> RnnoiseEngine::create()
{
    std::unique_ptr<RnnoiseEngine> engine (new RnnoiseEngine());

    for (auto& lane : engine->lanes)
    {
        lane.state = rnnoise_create (nullptr);

        if (lane.state == nullptr)
            return nullptr;
    }

    return engine;
}

RnnoiseEngine::~RnnoiseEngine()
{
    for (auto& lane : lanes)
        if (lane.state != nullptr)
            rnnoise_destroy (lane.state);
}

void RnnoiseEngine::setGate (float threshold, int holdFrames) noexcept
{
    gateThreshold = threshold;
    gateHoldFrames = holdFrames;
}

// The second lane is about to differ from the first: give it the first one's exact history.
void RnnoiseEngine::primeSecondLane() noexcept
{
    auto& a = lanes[0];
    auto& b = lanes[1];

    std::memcpy ((void*) b.state, (const void*) a.state, (size_t) rnnoise_get_size());
    b.in = a.in;
    b.out = a.out;
}

void RnnoiseEngine::finishFrame (int numLanes) noexcept
{
    float voiceNow = 0.0f;

    for (int c = 0; c < numLanes; ++c)
    {
        auto& lane = lanes[(size_t) c];
        voiceNow = std::max (voiceNow, rnnoise_process_frame (lane.state, lane.out.data(), lane.in.data()));

        for (auto& v : lane.out)
            v *= kFromRnnoise;
    }

    if (voiceNow >= gateThreshold)
        holdLeft = gateHoldFrames;
    else if (holdLeft > 0)
        --holdLeft;

    gateOpen = holdLeft > 0;
    vad.store (voiceNow, std::memory_order_relaxed);
    gateLevelShared.store (gateRamp * gateRamp * (3.0f - 2.0f * gateRamp), std::memory_order_relaxed);
    fill = 0;
}

// Each sample of in[c] is replaced by the denoised, gated sample from 480 samples ago while the new one is collected
// into the next frame.
void RnnoiseEngine::process (const float* const* in, float* const* out, int numLanes, int n) noexcept
{
    numLanes = std::clamp (numLanes, 1, kMaxLanes);

    if (numLanes == 2 && ! wasStereo)
        primeSecondLane();

    wasStereo = numLanes == 2;

    int pos = 0;

    while (pos < n)
    {
        const int segment = std::min (n - pos, kFrame - fill);

        // Gate gain for this stretch of output (one open/closed decision per frame, smooth per sample).
        const float target = gateOpen ? 1.0f : 0.0f;
        const bool gated = gateRamp != 1.0f || target != 1.0f;

        if (gated)
        {
            for (int k = 0; k < segment; ++k)
            {
                if (gateRamp < target)
                    gateRamp = std::min (target, gateRamp + kGateOpenStep);
                else if (gateRamp > target)
                    gateRamp = std::max (target, gateRamp - kGateCloseStep);

                gateGain[(size_t) k] = gateRamp * gateRamp * (3.0f - 2.0f * gateRamp);      // smoothstep
            }
        }

        for (int c = 0; c < numLanes; ++c)
        {
            auto& lane = lanes[(size_t) c];
            const float* src = in[c] + pos;
            float* dst = out[c] + pos;
            const float* denoised = lane.out.data() + fill;
            float* collect = lane.in.data() + fill;

            if (gated)
            {
                for (int k = 0; k < segment; ++k)
                {
                    const float v = src[k];
                    dst[k] = denoised[k] * gateGain[(size_t) k];
                    collect[k] = v * kToRnnoise;
                }
            }
            else
            {
                for (int k = 0; k < segment; ++k)
                {
                    const float v = src[k];
                    dst[k] = denoised[k];
                    collect[k] = v * kToRnnoise;
                }
            }
        }

        fill += segment;
        pos += segment;

        if (fill >= kFrame)
            finishFrame (numLanes);
    }
}
}
