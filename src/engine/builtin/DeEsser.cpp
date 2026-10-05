// De-esser: parameter layout and DSP.
#include "engine/builtin/DeEsser.h"

#include <cmath>

namespace hr
{
namespace
{
    constexpr float kDetectorQ = 1.0f;
    constexpr float kBellQ = 0.9f;
    constexpr float kMaxReductionDb = 12.0f;
    constexpr float kKneeDb = 6.0f;
    constexpr int kUpdateInterval = 16;             // samples between filter updates
}

DeEsser::DeEsser()
    : BuiltinProcessor (kId, "De-esser", createLayout())
{
    pFrequency = apvts.getRawParameterValue ("frequency");
    pThreshold = apvts.getRawParameterValue ("threshold");
    pAmount = apvts.getRawParameterValue ("amount");

    prepareToPlay (48000.0, 512);
}

BuiltinProcessor::Layout DeEsser::createLayout()
{
    Layout layout;

    layout.add (
        makeFloat ("frequency", "Frequency", 3000.0f, 10000.0f, 6500.0f, "Hz"),
        makeFloat ("threshold", "Threshold", -60.0f, 0.0f, -30.0f, "dB"),
        makeFloat ("amount", "Amount", 0.0f, 100.0f, 50.0f, "%"));

    return layout;
}

void DeEsser::prepareToPlay (double newSampleRate, int)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;

    for (auto& s : detectorState)   s.reset();
    for (auto& s : bellState)       s.reset();

    envelope = 0.0f;
    reduction = 0.0f;
    builtFrequency = 0.0f;
    builtReduction = -1.0f;
    untilUpdate = 0;
    needsSnap = true;
    meter.store (0.0f, std::memory_order_relaxed);
}

void DeEsser::releaseResources()
{
}

// Rebuilds the detector band-pass when the frequency moved and the bell when the frequency or reduction moved.
void DeEsser::updateFilters (float frequency, bool snap) noexcept
{
    using Array = juce::dsp::IIR::ArrayCoefficients<float>;

    const float maxFrequency = (float) (sampleRate * 0.45);
    const float target = juce::jmin (frequency, maxFrequency);

    if (snap)
        currentFrequency = target;
    else if (currentFrequency != target)
    {
        // Glide in the log domain (~12 ms) so moving the knob does not click while the bell is biting.
        const float d = std::log (target / currentFrequency);
        const float k = (float) (1.0 - std::exp (-(double) kUpdateInterval / (sampleRate * 0.012)));
        currentFrequency = std::fabs (d) < 1.0e-4f ? target : currentFrequency * std::exp (d * k);
    }

    if (currentFrequency != builtFrequency)
        detectorCoeffs = dsp::BiquadCoeffs::fromArray (Array::makeBandPass (sampleRate, currentFrequency, kDetectorQ));

    if (currentFrequency != builtFrequency || std::fabs (reduction - builtReduction) > 0.02f)
    {
        // Below 0.02 dB the bell counts as flat: snap to an exact identity so the effect is transparent at rest.
        const float db = reduction > 0.02f ? reduction : 0.0f;
        bellCoeffs = dsp::BiquadCoeffs::fromArray (Array::makePeakFilter (sampleRate, currentFrequency, kBellQ, dsp::dbToGain (-db)));
        builtReduction = reduction;
    }

    builtFrequency = currentFrequency;
}

void DeEsser::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int n = buffer.getNumSamples();
    const int numChannels = juce::jmin (buffer.getNumChannels(), 2);

    if (numChannels < 1 || n <= 0)
        return;

    float* l = buffer.getWritePointer (0);
    float* r = numChannels > 1 ? buffer.getWritePointer (1) : nullptr;

    const float frequency = pFrequency->load();
    const float threshold = pThreshold->load();
    const float maxReduction = pAmount->load() * 0.01f * kMaxReductionDb;

    const float envelopeRelease = (float) std::exp (-1.0 / (0.001 * sampleRate));
    const float attackCoeff = (float) (1.0 - std::exp (-1.0 / (0.0005 * sampleRate)));
    const float releaseCoeff = (float) (1.0 - std::exp (-1.0 / (0.060 * sampleRate)));
    const float quietLevel = std::pow (10.0f, (threshold - 0.5f * kKneeDb) * 0.05f);

    if (needsSnap)
    {
        updateFilters (frequency, true);
        needsSnap = false;
        untilUpdate = kUpdateInterval;
    }

    float peakReduction = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        // Sidechain: band-passed, linked across channels.
        float level = std::fabs (detectorState[0].tick (detectorCoeffs, l[i]));

        if (r != nullptr)
            level = juce::jmax (level, std::fabs (detectorState[1].tick (detectorCoeffs, r[i])));

        envelope = juce::jmax (level, envelope * envelopeRelease);

        float wanted = 0.0f;

        if (envelope > quietLevel)
        {
            const float over = 20.0f * std::log10 (envelope) - threshold;

            if (2.0f * over > kKneeDb)
                wanted = over;
            else if (2.0f * over >= -kKneeDb)
                wanted = (over + 0.5f * kKneeDb) * (over + 0.5f * kKneeDb) / (2.0f * kKneeDb);

            wanted = juce::jmin (wanted, maxReduction);
        }

        reduction += (wanted - reduction) * (wanted > reduction ? attackCoeff : releaseCoeff);

        if (reduction < 1.0e-4f)
            reduction = 0.0f;

        peakReduction = juce::jmax (peakReduction, reduction);

        if (--untilUpdate <= 0)
        {
            updateFilters (frequency, false);
            untilUpdate = kUpdateInterval;
        }

        l[i] = bellState[0].tick (bellCoeffs, l[i]);

        if (r != nullptr)
            r[i] = bellState[1].tick (bellCoeffs, r[i]);
    }

    meter.store (peakReduction > 0.0f ? -peakReduction : 0.0f, std::memory_order_relaxed);
}
}
