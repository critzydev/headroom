// Noise Gate built-in effect (id "gate"): peak detector with hysteresis and hold, dB-linear open/close ramps.
#pragma once

#include "engine/builtin/BuiltinProcessor.h"

namespace hr
{
// Range is how far the gate turns the signal down when closed. Attack is the time the gate needs to open fully,
// Release the time it needs to close fully; both ramps are linear in dB, so they sound like natural decays and
// never step. The detector is a stereo-linked sample-peak follower with an 8 ms release; the gate closes 4 dB
// below the level at which it opens, and stays open for Hold after the level has dropped.
// getMeterValue() is the current attenuation in dB (<= 0; 0 = open).
class Gate final : public BuiltinProcessor
{
public:
    static constexpr const char* kId = "gate";

    Gate();
    ~Gate() override = default;

    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    float getMeterValue() const noexcept override   { return meter.load (std::memory_order_relaxed); }

private:
    static Layout createLayout();

    std::atomic<float> *pThreshold, *pRange, *pAttack, *pHold, *pRelease;

    double sampleRate = 48000.0;
    float detector = 0.0f;              // peak envelope, linear
    float attenuationDb = 0.0f;         // 0 = open, range = closed
    float lastGain = 1.0f;
    float lastAttenuationDb = 0.0f;
    bool open = false;
    int holdLeft = 0;
    std::atomic<float> meter { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Gate)
};
}
