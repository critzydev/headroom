// RnnoiseEngine: RNNoise on the audio thread with a smooth voice gate, behind the fixed-delay Denoiser48 interface.
#pragma once

#include "engine/denoise/Denoiser48.h"

#include <array>
#include <atomic>
#include <memory>

struct DenoiseState;   // from rnnoise.h, included in the .cpp

namespace hr::denoise
{
// "Light" mode. RNNoise is small enough (well under 1 % of a core) to run inside process(): every 480 samples the
// finished frame goes through it right there.
//
// Delay. RNNoise v0.2 hands back the frame from two calls ago (it looks one frame ahead) and it can only be called
// once a whole frame has arrived, so the output is delayed by exactly 3 x 480 = 1440 samples (30 ms).
//
// Voice gate. RNNoise also returns a voice probability per frame. Whenever it is at or above the threshold the gate
// opens (5 ms fade); it closes (60 ms fade) once the probability has stayed below the threshold for the hold time
// (plus the 20 ms by which the network's view runs ahead of the audio it gates). The gate acts on the cleaned signal
// and is shared by both lanes, opened by the louder voice estimate.
//
// Stereo: when the second lane starts (the channels begin to differ), it continues from a copy of the first lane's
// state, which is exactly the state it would have had (the inputs were identical up to then).
class RnnoiseEngine final : public Denoiser48
{
public:
    static constexpr int kLatency = 3 * kFrame;

    // nullptr if RNNoise cannot allocate its state.
    static std::unique_ptr<RnnoiseEngine> create();
    ~RnnoiseEngine() override;

    int latency() const noexcept override                   { return kLatency; }
    void process (const float* const* in, float* const* out, int numLanes, int n) noexcept override;
    float voice() const noexcept override                   { return vad.load (std::memory_order_relaxed); }
    void setGate (float threshold, int holdFrames) noexcept override;
    float gateLevel() const noexcept override               { return gateLevelShared.load (std::memory_order_relaxed); }

private:
    RnnoiseEngine() = default;

    struct Lane
    {
        DenoiseState* state = nullptr;
        std::array<float, kFrame> in {};                    // frame being collected, scaled to +-32768
        std::array<float, kFrame> out {};                   // last denoised frame, back at +-1
    };

    void primeSecondLane() noexcept;
    void finishFrame (int numLanes) noexcept;

    std::array<Lane, kMaxLanes> lanes;
    int fill = 0;
    bool wasStereo = false;

    float gateThreshold = 0.6f;
    int gateHoldFrames = 17;
    int holdLeft = 0;
    bool gateOpen = true;
    float gateRamp = 1.0f;                                  // 0 closed .. 1 open, linear; shaped by smoothstep
    std::array<float, kFrame> gateGain {};

    std::atomic<float> vad { 0.0f };
    std::atomic<float> gateLevelShared { 1.0f };
};
}
