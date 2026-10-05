// Noise Gate: parameter layout and DSP.
#include "engine/builtin/Gate.h"

#include <cmath>

namespace hr
{
namespace
{
    constexpr float kHysteresisLinear = 0.63095734f;       // -4 dB
    constexpr float kDbToNeper = 0.115129255f;
}

Gate::Gate()
    : BuiltinProcessor (kId, "Noise Gate", createLayout())
{
    pThreshold = apvts.getRawParameterValue ("threshold");
    pRange = apvts.getRawParameterValue ("range");
    pAttack = apvts.getRawParameterValue ("attack");
    pHold = apvts.getRawParameterValue ("hold");
    pRelease = apvts.getRawParameterValue ("release");

    prepareToPlay (48000.0, 512);
}

BuiltinProcessor::Layout Gate::createLayout()
{
    Layout layout;

    layout.add (
        makeFloat ("threshold", "Threshold", -80.0f, 0.0f, -50.0f, "dB"),
        makeFloat ("range", "Range", 0.0f, 80.0f, 40.0f, "dB"),
        makeFloat ("attack", "Attack", 0.1f, 50.0f, 1.0f, "ms", 5.0f),
        makeFloat ("hold", "Hold", 0.0f, 500.0f, 100.0f, "ms", 100.0f),
        makeFloat ("release", "Release", 5.0f, 1000.0f, 150.0f, "ms", 150.0f));

    return layout;
}

void Gate::prepareToPlay (double newSampleRate, int)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    detector = 0.0f;
    attenuationDb = 0.0f;          // starts open: the first words after adding the gate are not swallowed
    lastGain = 1.0f;
    lastAttenuationDb = 0.0f;
    open = true;
    holdLeft = 0;
    meter.store (0.0f, std::memory_order_relaxed);
}

void Gate::releaseResources()
{
}

void Gate::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int n = buffer.getNumSamples();
    const int numChannels = juce::jmin (buffer.getNumChannels(), 2);

    if (numChannels < 1 || n <= 0)
        return;

    float* l = buffer.getWritePointer (0);
    float* r = numChannels > 1 ? buffer.getWritePointer (1) : nullptr;

    const float openLevel = std::pow (10.0f, pThreshold->load() * 0.05f);
    const float closeLevel = openLevel * kHysteresisLinear;
    const float range = pRange->load();
    const int holdSamples = (int) (0.001 * (double) pHold->load() * sampleRate);
    const float openStep = range / (float) juce::jmax (1.0, 0.001 * (double) pAttack->load() * sampleRate);
    const float closeStep = range / (float) juce::jmax (1.0, 0.001 * (double) pRelease->load() * sampleRate);
    const float detectorRelease = (float) std::exp (-1.0 / (0.008 * sampleRate));

    float maxAttenuation = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        float level = std::fabs (l[i]);

        if (r != nullptr)
            level = juce::jmax (level, std::fabs (r[i]));

        detector = juce::jmax (level, detector * detectorRelease);

        if (! open)
        {
            if (detector >= openLevel)
            {
                open = true;
                holdLeft = holdSamples;
            }
        }
        else if (detector >= closeLevel)
        {
            holdLeft = holdSamples;
        }
        else if (holdLeft > 0)
        {
            --holdLeft;
        }
        else
        {
            open = false;
        }

        if (open)
            attenuationDb = juce::jmax (0.0f, attenuationDb - openStep);
        else
            attenuationDb = juce::jmin (range, attenuationDb + closeStep);

        attenuationDb = juce::jmin (attenuationDb, range);      // the Range knob can be turned down while closed
        maxAttenuation = juce::jmax (maxAttenuation, attenuationDb);

        if (attenuationDb != lastAttenuationDb)
        {
            lastAttenuationDb = attenuationDb;
            lastGain = attenuationDb <= 0.0f ? 1.0f : std::exp (-attenuationDb * kDbToNeper);
        }

        if (lastGain != 1.0f)
        {
            l[i] *= lastGain;

            if (r != nullptr)
                r[i] *= lastGain;
        }
    }

    meter.store (maxAttenuation > 0.0f ? -maxAttenuation : 0.0f, std::memory_order_relaxed);
}
}
