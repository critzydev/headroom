// EQ band design: matched-magnitude biquads, cut cascades, and the magnitude response of the result.
#include "engine/dsp/EqDesign.h"

#include <algorithm>
#include <cmath>

namespace hr::dsp::eq
{
namespace
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kButterworthQ = 0.70710678118654752;
    constexpr double kNeutralDb = 0.005;                    // below this a gain band is flat and costs nothing

    inline double sq (double x) noexcept                    { return x * x; }

    // ---- Analog prototypes ------------------------------------------------------------------------
    // H(s) = (n0 + n1 s + n2 s^2) / (d0 + d1 s + d2 s^2) with s normalised to the centre frequency.
    struct Analog
    {
        double n0, n1, n2, d0, d1, d2;
    };

    inline double analogMagSq (const Analog& h, double w2) noexcept
    {
        const double nr = h.n0 - h.n2 * w2;
        const double dr = h.d0 - h.d2 * w2;
        return (nr * nr + h.n1 * h.n1 * w2) / (dr * dr + h.d1 * h.d1 * w2);
    }

    // Matched-z poles: z = exp (s T) for the roots of the analog denominator; w0 = centre frequency in rad/sample.
    inline void matchedPoles (const Analog& h, double w0, double& a1, double& a2) noexcept
    {
        const double disc = h.d1 * h.d1 - 4.0 * h.d2 * h.d0;

        if (disc < 0.0)
        {
            const double re = -h.d1 / (2.0 * h.d2), im = std::sqrt (-disc) / (2.0 * h.d2);
            const double r = std::exp (re * w0);
            a1 = -2.0 * r * std::cos (im * w0);
            a2 = r * r;
        }
        else
        {
            const double sqrtDisc = std::sqrt (disc);
            const double p1 = -2.0 * h.d0 / (h.d1 + sqrtDisc);         // the root nearer zero, without cancellation
            const double p2 = -(h.d1 + sqrtDisc) / (2.0 * h.d2);
            const double z1 = std::exp (p1 * w0), z2 = std::exp (p2 * w0);
            a1 = -(z1 + z2);
            a2 = z1 * z2;
        }
    }

    // The digital denominator's |A|^2 as A0 phi0 + A1 phi1 + A2 phi2 (Vicanek's notation, phi1 = sin^2 (w/2)).
    struct Denominator
    {
        double A0, A1, A2, phi0, phi1, phi2, atCentre;

        Denominator (double a1, double a2, double w0) noexcept
            : A0 (sq (1.0 + a1 + a2)), A1 (sq (1.0 - a1 + a2)), A2 (-4.0 * a2),
              phi0 (sq (std::cos (0.5 * w0))), phi1 (sq (std::sin (0.5 * w0))), phi2 (4.0 * phi0 * phi1),
              atCentre (A0 * phi0 + A1 * phi1 + A2 * phi2)
        {
        }
    };

    // A numerator (b0, b1, b2) whose |B|^2 = B0 phi0 + B1 phi1 + B2 phi2, where B0 = (b0+b1+b2)^2, B1 = (b0-b1+b2)^2 and
    // B2 = -4 b0 b2. The larger root goes to b0, which puts both zeros inside the unit circle (minimum phase).
    inline bool solveNumerator (double B0, double B1, double B2, BiquadCoeffs& c) noexcept
    {
        const double s0 = std::sqrt (std::max (B0, 0.0)), s1 = std::sqrt (std::max (B1, 0.0));
        const double w = 0.5 * (s0 + s1);
        const double disc = w * w + B2;

        if (disc < -1.0e-9 * (w * w + std::abs (B2)))
            return false;                                       // no real numerator has this magnitude

        const double r = std::sqrt (std::max (disc, 0.0));
        c.b0 = 0.5 * (w + r);
        c.b1 = 0.5 * (s0 - s1);
        c.b2 = 0.5 * (w - r);
        return true;
    }

    // Magnitude equal to the analog prototype at DC, at w0 and at Nyquist.
    inline bool matchedThreePoint (const Analog& h, double w0, BiquadCoeffs& c) noexcept
    {
        matchedPoles (h, w0, c.a1, c.a2);
        const Denominator d (c.a1, c.a2, w0);

        const double nyquist = kPi / w0;
        const double B0 = analogMagSq (h, 0.0) * d.A0;
        const double B1 = analogMagSq (h, nyquist * nyquist) * d.A1;
        const double atCentre = analogMagSq (h, 1.0) * d.atCentre;
        const double B2 = (atCentre - B0 * d.phi0 - B1 * d.phi1) / d.phi2;

        return solveNumerator (B0, B1, B2, c);
    }

    // ---- Bilinear-transform recipes (RBJ cookbook): the notch, the first-order cuts and the fallback ---------------
    struct Rbj
    {
        double c, alpha;

        Rbj (double w0, double q) noexcept : c (std::cos (w0)), alpha (std::sin (w0) / (2.0 * q)) {}
    };

    inline BiquadCoeffs normalised (double b0, double b1, double b2, double a0, double a1, double a2) noexcept
    {
        BiquadCoeffs out;
        out.b0 = b0 / a0;
        out.b1 = b1 / a0;
        out.b2 = b2 / a0;
        out.a1 = a1 / a0;
        out.a2 = a2 / a0;
        return out;
    }

    BiquadCoeffs rbjPeak (double w0, double q, double A) noexcept
    {
        const Rbj r (w0, q);
        return normalised (1.0 + r.alpha * A, -2.0 * r.c, 1.0 - r.alpha * A, 1.0 + r.alpha / A, -2.0 * r.c, 1.0 - r.alpha / A);
    }

    BiquadCoeffs rbjLowShelf (double w0, double q, double A) noexcept
    {
        const Rbj r (w0, q);
        const double t = 2.0 * std::sqrt (A) * r.alpha;
        return normalised (A * ((A + 1.0) - (A - 1.0) * r.c + t), 2.0 * A * ((A - 1.0) - (A + 1.0) * r.c), A * ((A + 1.0) - (A - 1.0) * r.c - t),
                           (A + 1.0) + (A - 1.0) * r.c + t, -2.0 * ((A - 1.0) + (A + 1.0) * r.c), (A + 1.0) + (A - 1.0) * r.c - t);
    }

    BiquadCoeffs rbjHighShelf (double w0, double q, double A) noexcept
    {
        const Rbj r (w0, q);
        const double t = 2.0 * std::sqrt (A) * r.alpha;
        return normalised (A * ((A + 1.0) + (A - 1.0) * r.c + t), -2.0 * A * ((A - 1.0) + (A + 1.0) * r.c), A * ((A + 1.0) + (A - 1.0) * r.c - t),
                           (A + 1.0) - (A - 1.0) * r.c + t, 2.0 * ((A - 1.0) - (A + 1.0) * r.c), (A + 1.0) - (A - 1.0) * r.c - t);
    }

    BiquadCoeffs rbjLowPass (double w0, double q) noexcept
    {
        const Rbj r (w0, q);
        return normalised (0.5 * (1.0 - r.c), 1.0 - r.c, 0.5 * (1.0 - r.c), 1.0 + r.alpha, -2.0 * r.c, 1.0 - r.alpha);
    }

    BiquadCoeffs rbjBandPass (double w0, double q) noexcept
    {
        const Rbj r (w0, q);
        return normalised (r.alpha, 0.0, -r.alpha, 1.0 + r.alpha, -2.0 * r.c, 1.0 - r.alpha);
    }

    BiquadCoeffs rbjNotch (double w0, double q) noexcept
    {
        const Rbj r (w0, q);
        return normalised (1.0, -2.0 * r.c, 1.0, 1.0 + r.alpha, -2.0 * r.c, 1.0 - r.alpha);
    }

    BiquadCoeffs highPass1 (double w0) noexcept
    {
        const double k = std::tan (0.5 * w0);
        BiquadCoeffs c;
        c.b0 = 1.0 / (1.0 + k);
        c.b1 = -c.b0;
        c.a1 = (k - 1.0) / (k + 1.0);
        return c;
    }

    // Matched at DC (gain 1) and at Nyquist (the analog magnitude there), with the analog pole. The bilinear low-pass
    // would have a zero at Nyquist and squeeze the roll-off; this one keeps the analog 6 dB/oct shape.
    BiquadCoeffs lowPass1 (double w0) noexcept
    {
        const double p = std::exp (-w0);
        const double atNyquist = 1.0 / std::sqrt (1.0 + sq (kPi / w0));
        BiquadCoeffs c;
        c.b0 = 0.5 * ((1.0 - p) + atNyquist * (1.0 + p));
        c.b1 = (1.0 - p) - c.b0;
        c.a1 = -p;
        return c;
    }

    // The exact reciprocal of a minimum-phase section (numerator and denominator swap): a cut is the inverse of the
    // boost with the same frequency and Q, in the analog world and here.
    BiquadCoeffs invert (const BiquadCoeffs& c) noexcept
    {
        if (c.b0 < 1.0e-12)
            return {};

        const double g = 1.0 / c.b0;
        BiquadCoeffs r;
        r.b0 = g;
        r.b1 = c.a1 * g;
        r.b2 = c.a2 * g;
        r.a1 = c.b1 * g;
        r.a2 = c.b2 * g;
        return r;
    }

    // ---- The sections --------------------------------------------------------------------------------------------
    // Bell. Designed as a boost (which always has a matched solution) and inverted for a cut.
    BiquadCoeffs peakSection (double w0, double q, double gainDb) noexcept
    {
        const bool cut = gainDb < 0.0;
        const double A = std::pow (10.0, std::abs (gainDb) / 40.0);
        const Analog h { 1.0, A / q, 1.0, 1.0, 1.0 / (A * q), 1.0 };

        BiquadCoeffs c;

        if (! matchedThreePoint (h, w0, c))
            c = rbjPeak (w0, q, A);

        return cut ? invert (c) : c;
    }

    // Low shelf: the boost is designed directly, a cut is its inverse.
    BiquadCoeffs lowShelfSection (double w0, double q, double gainDb) noexcept
    {
        const bool cut = gainDb < 0.0;
        const double A = std::pow (10.0, std::abs (gainDb) / 40.0);
        const double sA = std::sqrt (A);
        const Analog h { A * A, A * sA / q, A, 1.0, sA / q, A };        // A (s^2 + sqrt(A)/Q s + A) / (A s^2 + sqrt(A)/Q s + 1)

        BiquadCoeffs c;

        if (! matchedThreePoint (h, w0, c))
            c = rbjLowShelf (w0, q, A);

        return cut ? invert (c) : c;
    }

    // High shelf: the cut is designed directly (that is the direction that always has a solution), a boost is its inverse.
    BiquadCoeffs highShelfSection (double w0, double q, double gainDb) noexcept
    {
        const bool boost = gainDb > 0.0;
        const double A = std::pow (10.0, -std::abs (gainDb) / 40.0);     // < 1: the cut
        const double sA = std::sqrt (A);
        const Analog h { A, A * sA / q, A * A, A, sA / q, 1.0 };        // A (A s^2 + sqrt(A)/Q s + 1) / (s^2 + sqrt(A)/Q s + A)

        BiquadCoeffs c;

        if (! matchedThreePoint (h, w0, c))
            c = rbjHighShelf (w0, q, A);

        return boost ? invert (c) : c;
    }

    BiquadCoeffs lowPass2 (double w0, double q) noexcept
    {
        const Analog h { 1.0, 0.0, 0.0, 1.0, 1.0 / q, 1.0 };
        BiquadCoeffs c;

        if (! matchedThreePoint (h, w0, c))
            c = rbjLowPass (w0, q);

        return c;
    }

    // A high-pass has a double zero at DC, which leaves one number to choose: the gain at the centre frequency.
    BiquadCoeffs highPass2 (double w0, double q) noexcept
    {
        const Analog h { 0.0, 0.0, 1.0, 1.0, 1.0 / q, 1.0 };
        BiquadCoeffs c;
        matchedPoles (h, w0, c.a1, c.a2);

        const Denominator d (c.a1, c.a2, w0);
        const double k = q * std::sqrt (d.atCentre) / (4.0 * d.phi1);      // the analog magnitude at w0 is Q
        c.b0 = k;
        c.b1 = -2.0 * k;
        c.b2 = k;
        return c;
    }

    BiquadCoeffs bandPass2 (double w0, double q) noexcept
    {
        const Analog h { 0.0, 1.0 / q, 0.0, 1.0, 1.0 / q, 1.0 };
        BiquadCoeffs c;

        if (! matchedThreePoint (h, w0, c))
            c = rbjBandPass (w0, q);

        return c;
    }

    // Butterworth stage Qs, lowest first; the last one is the one the Q knob acts on.
    constexpr double kStages24[] { 0.5411961001461969, 1.3065629648763766 };
    constexpr double kStages48[] { 0.5097955791041592, 0.6013448869350453, 0.8999762231364156, 2.5629154477415055 };

    void cutSections (bool highPass, double w0, double q, int slope, Sections& out) noexcept
    {
        auto add = [&] (double stageQ) noexcept
        {
            out.s[(size_t) out.count++] = highPass ? highPass2 (w0, stageQ) : lowPass2 (w0, stageQ);
        };

        const double resonance = q / kButterworthQ;

        switch (slope)
        {
            case 0:     out.s[0] = highPass ? highPass1 (w0) : lowPass1 (w0); out.count = 1; break;
            case 2:     add (kStages24[0]); add (kStages24[1] * resonance); break;
            case 3:     add (kStages48[0]); add (kStages48[1]); add (kStages48[2]); add (kStages48[3] * resonance); break;
            case kSlopeLinkwitzRiley24: add (q); add (q); break;
            default:    add (q); break;
        }
    }
}

Sections design (const BandParams& p, double sampleRate) noexcept
{
    Sections out;

    const double f = std::clamp (p.freq, 1.0, 0.49 * sampleRate);
    const double w0 = 2.0 * kPi * f / sampleRate;
    const double q = std::clamp (p.q, 0.05, 50.0);
    const double g = std::clamp (p.gainDb, -48.0, 48.0);
    const bool audible = std::abs (g) >= kNeutralDb;

    switch (p.type)
    {
        case Type::peak:
            if (audible)
            {
                out.s[0] = peakSection (w0, q, g);
                out.count = 1;
            }
            break;

        case Type::lowShelf:
            if (audible)
            {
                out.s[0] = lowShelfSection (w0, q, g);
                out.count = 1;
            }
            break;

        case Type::highShelf:
            if (audible)
            {
                out.s[0] = highShelfSection (w0, q, g);
                out.count = 1;
            }
            break;

        case Type::tiltShelf:
            if (audible)
            {
                out.s[0] = lowShelfSection (w0, q, -g);
                out.s[1] = highShelfSection (w0, q, g);
                out.count = 2;
            }
            break;

        case Type::lowCut:      cutSections (true, w0, q, p.slope, out); break;
        case Type::highCut:     cutSections (false, w0, q, p.slope, out); break;

        case Type::notch:
            out.s[0] = rbjNotch (w0, q);
            out.count = 1;
            break;

        case Type::bandPass:
            out.s[0] = bandPass2 (w0, q);
            out.count = 1;
            break;
    }

    return out;
}

double magnitudeSquared (const BiquadCoeffs& c, const FreqPoint& p) noexcept
{
    const double num = sq (c.b0 + c.b1 + c.b2) * p.phi0 + sq (c.b0 - c.b1 + c.b2) * p.phi1 - 4.0 * c.b0 * c.b2 * p.phi2;
    const double den = sq (1.0 + c.a1 + c.a2) * p.phi0 + sq (1.0 - c.a1 + c.a2) * p.phi1 - 4.0 * c.a2 * p.phi2;
    return num / std::max (den, 1.0e-300);
}

double responseDb (const Sections& sections, const FreqPoint& p) noexcept
{
    double m = 1.0;

    for (int i = 0; i < sections.count; ++i)
        m *= magnitudeSquared (sections.s[(size_t) i], p);

    return 10.0 * std::log10 (std::max (m, 1.0e-12));
}

double responseDb (const Sections& sections, double freq, double sampleRate) noexcept
{
    return responseDb (sections, FreqPoint::fromHz (freq, sampleRate));
}
}
