// Small real-time-safe helpers shared by the engine: dB conversion, peak measurement, block gain ramps.
#pragma once

#include "Juce.h"
#include "engine/EngineTypes.h"

#include <cmath>

namespace hr
{
inline float dbToLinear (float db) noexcept
{
    return db <= kMinusInfDb ? 0.0f : std::pow (10.0f, db * 0.05f);
}

inline float linearToDb (float gain) noexcept
{
    return gain <= 1.0e-5f ? kMinusInfDb : 20.0f * std::log10 (gain);
}

// Largest absolute sample in a block.
inline float peakOf (const float* data, int numSamples) noexcept
{
    auto range = juce::FloatVectorOperations::findMinAndMax (data, numSamples);
    return juce::jmax (std::abs (range.getStart()), std::abs (range.getEnd()));
}

// Linear-ramped gain with a fixed ramp time; wraps juce::SmoothedValue with the call pattern the engine uses.
class SmoothedGain
{
public:
    void prepare (double sampleRate, double rampSeconds = 0.020)
    {
        value.reset (sampleRate, rampSeconds);
        value.setCurrentAndTargetValue (value.getTargetValue());
    }

    void snapTo (float v) noexcept          { value.setCurrentAndTargetValue (v); }
    void setTarget (float v) noexcept       { if (value.getTargetValue() != v) value.setTargetValue (v); }
    bool isRamping() const noexcept         { return value.isSmoothing(); }
    float getTarget() const noexcept        { return value.getTargetValue(); }

    // Fills ramp[0..n) with per-sample gains and returns true if the gain is moving during this block.
    // When it returns false, use getTarget() as a constant and ignore ramp.
    bool fillRamp (float* ramp, int n) noexcept
    {
        if (! value.isSmoothing())
            return false;

        for (int i = 0; i < n; ++i)
            ramp[i] = value.getNextValue();

        return true;
    }

private:
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> value { 1.0f };
};
}
