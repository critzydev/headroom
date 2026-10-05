// Filter design for the EQ bands: analog-matched biquads (no cramping in the top octave) and their magnitude response.
#pragma once

#include "engine/dsp/Biquad.h"

#include <array>
#include <cmath>

namespace hr::dsp::eq
{
enum class Type : int { peak = 0, lowShelf, highShelf, lowCut, highCut, notch, bandPass, tiltShelf };
constexpr int kNumTypes = 8;

// Cut slopes, as the index of the "Slope" parameter: 6, 12, 24 and 48 dB/oct (Butterworth), plus the 24 dB/oct
// Linkwitz-Riley shape that the old two-stage high-pass had, so that saved states keep their exact sound.
constexpr int kNumSlopes = 5;
constexpr int kSlope12 = 1, kSlopeLinkwitzRiley24 = 4;

constexpr int kMaxSections = 4;                 // 48 dB/oct = four biquads

struct BandParams
{
    Type type = Type::peak;
    double freq = 1000.0;                       // Hz
    double gainDb = 0.0;                        // peak, shelves, tilt
    double q = 0.70710678;
    int slope = kSlope12;                       // cuts only
};

// One band = up to four biquads in series. count == 0 means "flat, nothing to run".
struct Sections
{
    int count = 0;
    std::array<BiquadCoeffs, kMaxSections> s;
};

// The design. Pure, allocation free, safe on the RT thread. Both the DSP and the response curves the UI draws go
// through exactly this function, so what you see is what runs.
//
// Method: each biquad has its poles placed by the matched-z transform of the analog prototype, and its numerator
// solved so that the magnitude equals the analog prototype's at DC, at the centre frequency and at Nyquist (after
// M. Vicanek, "Matched Second Order Digital Filters"). That removes the usual bilinear-transform cramping: a bell at
// 12 kHz keeps its shape instead of being squeezed against Nyquist. That solution exists for a bell boost, a low-shelf
// boost and a high-shelf cut in every case tried (Q 0.05 - 30, +-30 dB, every rate up to 192 kHz; the bilinear recipe
// is the fallback if it ever does not), so the opposite direction of each is designed as the exact reciprocal (the
// analog cut is the reciprocal of the boost, and so is this one). The 2nd-order high-pass matches its gain at the
// corner, the 6 dB/oct low-pass matches DC and Nyquist, the 6 dB/oct high-pass and the notch use the bilinear
// transform. Frequencies are limited to 0.49 x sampleRate.
//
//  peak / low shelf / high shelf: RBJ-cookbook prototypes (Q as in the cookbook; a shelf with Q 0.7071 is maximally flat)
//  low cut / high cut:            Butterworth cascades; Q is the resonance of the last stage (0.7071 = flat)
//  notch / band pass:             2nd order, Q = centre frequency / bandwidth
//  tilt shelf:                    a low shelf of -gain and a high shelf of +gain at the same frequency
Sections design (const BandParams& params, double sampleRate) noexcept;

// A frequency prepared for evaluating many biquads: sin^2 (w/2), cos^2 (w/2) and their product (w in radians per
// sample). Evaluating |H|^2 in this form has no cancellation near DC, where poles and zeros crowd around z = 1.
struct FreqPoint
{
    double phi0, phi1, phi2;

    static FreqPoint fromW (double w) noexcept
    {
        const double s = std::sin (0.5 * w), c = std::cos (0.5 * w);
        return { c * c, s * s, 4.0 * c * c * s * s };
    }

    static FreqPoint fromHz (double freq, double sampleRate) noexcept
    {
        constexpr double kTwoPi = 6.283185307179586476925286766559;
        const double f = freq < 0.0 ? 0.0 : (freq > 0.5 * sampleRate ? 0.5 * sampleRate : freq);
        return fromW (kTwoPi * f / sampleRate);
    }
};

double magnitudeSquared (const BiquadCoeffs& c, const FreqPoint& p) noexcept;

// Combined magnitude in dB, floored at -120 dB.
double responseDb (const Sections& sections, const FreqPoint& p) noexcept;
double responseDb (const Sections& sections, double freq, double sampleRate) noexcept;
}
