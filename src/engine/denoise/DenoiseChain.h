// DenoiseChain: one engine plus everything around it at the host sample rate: resampling, dry alignment, blend.
#pragma once

#include "Juce.h"
#include "engine/denoise/Denoiser48.h"
#include "engine/dsp/Resampler.h"

#include <array>
#include <memory>
#include <vector>

namespace hr::denoise
{
// Signal path per lane (nothing allocates after the constructor):
//
//   host rate --[resample to 48 kHz, if needed]--> engine --[resample back]--> wet
//   host rate --> delay line (the same latency) --> dry              out = wet + (dry - wet) * dryMix
//
// The dry path is delayed by exactly the wet path's latency, so mixing the two never combs and "dryMix = 1" is the
// untouched input, delayed. Rates other than 48 kHz (8-192 kHz) go through dsp::Resampler in both directions and the
// delay is reported in host samples, priming included.
class DenoiseChain
{
public:
    enum class Kind { fast, best, light };

    static constexpr int kMaxChunk = 1024;              // host samples per process() call

    // hostRate must be within 8-192 kHz.
    DenoiseChain (Kind chainKind, std::unique_ptr<Denoiser48> engineToUse, double hostRate);

    Kind kind() const noexcept                          { return chainKind; }
    Denoiser48& getEngine() noexcept                    { return *engine; }
    const Denoiser48& getEngine() const noexcept        { return *engine; }
    bool isResampled() const noexcept                   { return resampled; }
    double getHostRate() const noexcept                 { return hostRate; }

    // Total delay in host samples: input sample -> the same sample in the output (and in the aligned dry signal).
    int getLatency() const noexcept                     { return latencyHost; }

    // Times the resampled wet path had less audio than the block needed (should stay 0).
    uint32_t getUnderruns() const noexcept              { return underruns; }

    // Audio thread. n <= kMaxChunk. in, out and dryOut must be distinct arrays (numLanes of them, 1 or 2).
    // out = the blend, dryOut = the input aligned with it. dryMixRamp (n values, or nullptr for `dryMixConstant`)
    // is the share of the aligned input in the output: 1 = input only, 0 = the engine's output only.
    void process (const float* const* in, float* const* out, float* const* dryOut, int numLanes, int n,
                  const float* dryMixRamp, float dryMixConstant) noexcept;

private:
    void primeSecondLane() noexcept;

    const Kind chainKind;
    std::unique_ptr<Denoiser48> engine;
    const double hostRate;
    const bool resampled;
    int latencyHost = 0;
    int priming = 0;                                    // resampled: pre-fill of the wet FIFO, host samples

    dsp::Resampler toNative, fromNative;                // channel index = lane
    bool wasStereo = false;

    uint32_t dryPos = 0, dryMask = 0;
    uint32_t wetRead = 0, wetWrite = 0, wetMask = 0;

    struct Lane
    {
        std::vector<float> dry;                         // delay line at the host rate
        std::vector<float> wetFifo;                     // resampled: wet samples at the host rate
        std::vector<float> wet;                         // this block's wet signal at the host rate
        std::vector<float> scratch;                     // 48 kHz work buffer (resampled)
        std::vector<float> hostWet;                     // output of the back-conversion
    };

    std::array<Lane, kMaxLanes> lanes;
    uint32_t underruns = 0;

    JUCE_DECLARE_NON_COPYABLE (DenoiseChain)
};
}
