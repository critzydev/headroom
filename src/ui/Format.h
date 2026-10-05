// Number formatting for the UI: dB, frequencies and effect parameters, with real minus signs.
#pragma once

#include "Juce.h"
#include "engine/EngineTypes.h"
#include "ui/Theme.h"

namespace hr::fmt
{
// "-6.0 dB" with a typographic minus; "-inf dB" at or below kMinusInfDb.
inline juce::String db (float value, bool withUnit = true, bool forceSign = false, int decimals = 1)
{
    juce::String text;

    if (value <= kMinusInfDb)
    {
        text = theme::glyph::minus + theme::glyph::infinity;
    }
    else
    {
        const auto rounded = juce::String (std::abs (value), decimals);
        const bool negative = value < 0.0f && rounded.getDoubleValue() != 0.0;

        text = negative ? theme::glyph::minus + rounded : (forceSign && rounded.getDoubleValue() != 0.0 ? "+" + rounded : rounded);
    }

    return withUnit ? text + " dB" : text;
}

inline juce::String freq (float hz)
{
    if (hz < 1000.0f)
        return juce::String (juce::roundToInt (hz)) + " Hz";

    const auto khz = hz / 1000.0f;
    return juce::String (khz, khz < 10.0f ? 2 : 1) + " kHz";
}

// Effect parameter text from its unit label (the strings BuiltinProcessor::makeFloat was given).
inline juce::String param (const juce::String& unit, float value, bool forceSign = false)
{
    if (unit == "dB")   return db (value, true, forceSign);
    if (unit == "Hz")   return freq (value);
    if (unit == "ms")   return juce::String (value, value < 10.0f ? 1 : 0) + " ms";
    if (unit == "%")    return juce::String (juce::roundToInt (value)) + "%";
    if (unit == ":1")   return juce::String (value, 1) + ":1";
    return juce::String (value, 2);
}
}
