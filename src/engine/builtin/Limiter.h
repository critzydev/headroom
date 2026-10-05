// Limiter built-in effect (id "limit"): lookahead brickwall (shared dsp::LookaheadLimiter), 1.5 ms of latency.
#pragma once

#include "engine/builtin/BuiltinProcessor.h"
#include "engine/dsp/LookaheadLimiter.h"

namespace hr
{
// Defaults match the first-run mic chain: ceiling -1 dBFS, release 50 ms. Nothing above the ceiling
// gets through (after the lookahead has filled); below it the audio is only delayed, bit for bit. The latency is
// reported through setLatencySamples(). Stereo-linked.
// getMeterValue() is the largest gain reduction of the last block in dB (<= 0).
class Limiter final : public BuiltinProcessor
{
public:
    static constexpr const char* kId = "limit";

    Limiter();
    ~Limiter() override = default;

    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    float getMeterValue() const noexcept override   { return meter.load (std::memory_order_relaxed); }

private:
    static Layout createLayout();

    std::atomic<float> *pCeiling, *pRelease;
    dsp::LookaheadLimiter limiter;
    float lastCeiling = 1000.0f, lastRelease = -1.0f;
    std::atomic<float> meter { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Limiter)
};
}
