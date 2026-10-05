// De-esser built-in effect (id "deess"): band-pass sidechain drives a dynamic bell that turns the sibilance band down.
#pragma once

#include "engine/builtin/BuiltinProcessor.h"
#include "engine/dsp/Biquad.h"

namespace hr
{
// Detection: the input (both channels, linked) goes through a band-pass centred on Frequency (Q 1), a peak
// follower (1 ms release) and a soft-knee (6 dB) threshold. The excess over Threshold, limited to Amount
// (0..100 % = 0..12 dB maximum reduction), is smoothed (0.5 ms attack, 60 ms release) and drives the gain of a
// peaking filter at Frequency (Q 0.9, gain = -reduction). Only that band moves, and only while sibilance is
// present: with no reduction the filter is an exact identity, so the effect is transparent at rest.
// getMeterValue() is the largest reduction of the last block in dB (<= 0).
class DeEsser final : public BuiltinProcessor
{
public:
    static constexpr const char* kId = "deess";

    DeEsser();
    ~DeEsser() override = default;

    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    float getMeterValue() const noexcept override   { return meter.load (std::memory_order_relaxed); }

private:
    static Layout createLayout();
    void updateFilters (float frequency, bool snap) noexcept;

    std::atomic<float> *pFrequency, *pThreshold, *pAmount;

    double sampleRate = 48000.0;
    dsp::BiquadCoeffs detectorCoeffs, bellCoeffs;
    dsp::BiquadState detectorState[2], bellState[2];
    float currentFrequency = 6500.0f;       // glides towards the parameter
    float builtFrequency = 0.0f;
    float builtReduction = -1.0f;
    float envelope = 0.0f;
    float reduction = 0.0f;                 // smoothed reduction, dB >= 0
    int untilUpdate = 0;
    bool needsSnap = true;
    std::atomic<float> meter { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeEsser)
};
}
