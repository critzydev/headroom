// Lookahead brickwall limiter shared by the master buses and the built-in "limit" effect.
#pragma once

#include <cstdint>
#include <vector>

namespace hr::dsp
{
// Transparent by construction: when nothing exceeds the ceiling the gain is exactly 1.0 and the output is the
// input delayed by getLatencySamples(). Otherwise the gain is
//   sliding-window minimum of the per-sample required gain over the lookahead window, smoothed by a moving
//   average of the same length (so the attack is a ~lookahead-long ramp that finishes before the peak leaves
//   the delay line), followed by an exponential release.
// The moving average of the window minimum can never exceed the required gain of the sample being output, so
// the ceiling is never crossed (a final clamp only removes floating-point dust). Stereo-linked.
//
// prepare() allocates and must run off the RT thread; everything else is allocation-free.
class LookaheadLimiter
{
public:
    void prepare (double sampleRate, double lookaheadMs = 1.5);
    void reset() noexcept;

    void setCeilingDb (float db) noexcept;
    void setReleaseMs (float ms) noexcept;

    int getLatencySamples() const noexcept     { return lookahead; }

    // In place. `r` may be nullptr for a mono signal. With active == false the delay and the gain follower keep
    // running (so toggling never moves the audio in time) but the gain reduction is faded out over ~5 ms.
    void process (float* l, float* r, int numSamples, bool active = true) noexcept;

    // Smallest gain that was applied during the last process() call (1 = no reduction).
    float getLastBlockMinGain() const noexcept { return lastBlockMinGain; }

private:
    void updateCoefficients() noexcept;

    double sampleRate = 48000.0;
    float ceiling = 0.96605f;              // -0.3 dBFS
    float releaseMs = 50.0f;
    float releaseCoeff = 0.999f;
    float activeCoeff = 0.99f;

    int lookahead = 0;                     // D: samples of delay == samples of lookahead
    int window = 1;                        // W = D + 1

    std::vector<float> delayL, delayR;     // D samples each
    int delayPos = 0;

    // Monotonic deque for the sliding-window minimum (ring buffer of capacity window + 1).
    std::vector<float> dequeValue;
    std::vector<int64_t> dequeStamp;
    int dequeHead = 0, dequeCount = 0, dequeCapacity = 1;
    int64_t sampleIndex = 0;

    // Moving average of the window minimum.
    std::vector<float> averageRing;
    int averagePos = 0;
    double averageSum = 0.0;

    float deficit = 0.0f;                  // 1 - release-smoothed gain
    float activeAmount = 1.0f;             // 0..1 blend of the gain reduction
    float lastBlockMinGain = 1.0f;
};
}
