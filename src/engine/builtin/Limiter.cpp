// Limiter: parameter layout and glue around dsp::LookaheadLimiter.
#include "engine/builtin/Limiter.h"

#include <cmath>

namespace hr
{
Limiter::Limiter()
    : BuiltinProcessor (kId, "Limiter", createLayout())
{
    pCeiling = apvts.getRawParameterValue ("ceiling");
    pRelease = apvts.getRawParameterValue ("release");

    limiter.prepare (48000.0, 1.5);     // valid state even if nobody prepares us
}

BuiltinProcessor::Layout Limiter::createLayout()
{
    Layout layout;

    layout.add (
        makeFloat ("ceiling", "Ceiling", -12.0f, 0.0f, -1.0f, "dB"),
        makeFloat ("release", "Release", 1.0f, 500.0f, 50.0f, "ms", 50.0f));

    return layout;
}

void Limiter::prepareToPlay (double sampleRate, int)
{
    limiter.prepare (sampleRate > 0.0 ? sampleRate : 48000.0, 1.5);
    lastCeiling = 1000.0f;              // force the next block to push the parameters
    lastRelease = -1.0f;
    meter.store (0.0f, std::memory_order_relaxed);
    setLatencySamples (limiter.getLatencySamples());
}

void Limiter::releaseResources()
{
}

void Limiter::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int n = buffer.getNumSamples();
    const int numChannels = juce::jmin (buffer.getNumChannels(), 2);

    if (numChannels < 1 || n <= 0)
        return;

    const float ceiling = pCeiling->load();
    const float release = pRelease->load();

    if (ceiling != lastCeiling)     { limiter.setCeilingDb (ceiling);   lastCeiling = ceiling; }
    if (release != lastRelease)     { limiter.setReleaseMs (release);   lastRelease = release; }

    limiter.process (buffer.getWritePointer (0), numChannels > 1 ? buffer.getWritePointer (1) : nullptr, n, true);

    const float minGain = limiter.getLastBlockMinGain();
    meter.store (minGain < 1.0f ? 20.0f * std::log10 (juce::jmax (minGain, 1.0e-6f)) : 0.0f, std::memory_order_relaxed);
}
}
