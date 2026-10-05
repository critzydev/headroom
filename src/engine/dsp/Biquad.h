// Direct-form-II-transposed biquad with double state, fed from juce::dsp::IIR::ArrayCoefficients (allocation-free, RT safe).
#pragma once

#include <array>
#include <cmath>

namespace hr::dsp
{
// Normalised coefficients (a0 = 1). Built from the {b0, b1, b2, a0, a1, a2} arrays that
// juce::dsp::IIR::ArrayCoefficients<float>::make* return, dividing in float exactly like
// juce::dsp::IIR::Coefficients<float> does, so the DSP and Coefficients<float>::getMagnitudeForFrequency agree.
struct BiquadCoeffs
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;

    static BiquadCoeffs fromArray (const std::array<float, 6>& c) noexcept
    {
        const float a0Inv = c[3] != 0.0f ? 1.0f / c[3] : 0.0f;

        BiquadCoeffs out;
        out.b0 = (double) (c[0] * a0Inv);
        out.b1 = (double) (c[1] * a0Inv);
        out.b2 = (double) (c[2] * a0Inv);
        out.a1 = (double) (c[4] * a0Inv);
        out.a2 = (double) (c[5] * a0Inv);
        return out;
    }
};

// State in double: an 80 Hz high-pass at 96 kHz in single precision is noisy, and this costs nothing here.
struct BiquadState
{
    double z1 = 0.0, z2 = 0.0;

    void reset() noexcept   { z1 = z2 = 0.0; }

    float tick (const BiquadCoeffs& c, float x) noexcept
    {
        const double y = c.b0 * (double) x + z1;
        z1 = c.b1 * (double) x - c.a1 * y + z2;
        z2 = c.b2 * (double) x - c.a2 * y;
        return (float) y;
    }
};

// Gain in dB -> linear factor without going through juce (used for filter recipes).
inline float dbToGain (float db) noexcept
{
    return std::pow (10.0f, db * 0.05f);
}
}
