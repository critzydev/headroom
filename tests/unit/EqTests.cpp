// Unit tests for the 8-band EQ: filter design, band types, response vs DSP, old-state migration, analyzer feed, solo, robustness.
#include "Juce.h"

#include "AllocationGuard.h"
#include "TestHelpers.h"
#include "app/AppController.h"
#include "engine/builtin/BuiltinFx.h"
#include "engine/builtin/Equalizer.h"
#include "engine/builtin/Presets.h"
#include "engine/dsp/AnalyzerFifo.h"
#include "engine/dsp/EqDesign.h"
#include "engine/dsp/SpectrumAnalyzer.h"
#include "ui/fx/EqSheet.h"

#include <cmath>
#include <complex>
#include <functional>
#include <random>

namespace hr
{
using namespace test;

namespace
{
    using Type = Equalizer::Type;

    // The old filters were bilinear-transform designs, which squeeze everything against Nyquist; the new ones follow the
    // analog prototypes instead. So an old state and its conversion agree to 0.1 dB across the range that matters for a
    // voice (up to 2 kHz, whatever the old EQ was set to), to a quarter of a dB up to 6 kHz, and drift further apart
    // towards Nyquist, where the new EQ is the one that matches what was asked for (see testTopOctave). The states the
    // app itself creates (the default, the presets) are within a fraction of a dB everywhere.
    constexpr double kSameSoundLimitHz = 2000.0, kNearLimitHz = 6000.0;
    namespace eqd = dsp::eq;

    void setParam (juce::AudioProcessorValueTreeState& apvts, const juce::String& id, float plainValue)
    {
        auto* p = apvts.getParameter (id);
        jassert (p != nullptr);
        p->setValueNotifyingHost (p->convertTo0to1 (plainValue));
    }

    struct Band
    {
        int number = 1;                     // 1-based, like the parameter ids
        bool on = true;
        Type type = Type::peak;
        float freq = 1000.0f, gain = 0.0f, q = 1.0f;
        int slope = 1;
    };

    void apply (Equalizer& eq, const Band& b)
    {
        const auto id = [&] (const char* what) { return Equalizer::bandParamId (b.number - 1, what); };
        setParam (eq.apvts, id ("Type"), (float) (int) b.type);
        setParam (eq.apvts, id ("Freq"), b.freq);
        setParam (eq.apvts, id ("Gain"), b.gain);
        setParam (eq.apvts, id ("Q"), b.q);
        setParam (eq.apvts, id ("Slope"), (float) b.slope);
        setParam (eq.apvts, id ("On"), b.on ? 1.0f : 0.0f);
    }

    double measureGainDb (juce::AudioProcessor& p, double sampleRate, double frequency, double amplitude = 0.25, double seconds = 1.0, int blockSize = 480)
    {
        const int n = (int) (sampleRate * seconds);
        const auto x = makeSine (n, sampleRate, frequency, amplitude);
        const auto y = runMono (p, x, blockSize);
        const int start = n / 2;

        return toneDb (y.data() + start, n - start, sampleRate, frequency) - toneDb (x.data() + start, n - start, sampleRate, frequency);
    }

    // The same measurement, on a processor that already has its parameters set and has been prepared.
    double dspGainDb (Equalizer& eq, double f, double sr = kRate)
    {
        eq.prepareToPlay (sr, 512);
        return measureGainDb (eq, sr, f, 0.25, 1.0, 512);
    }

    // ---- Analog references for the design tests ---------------------------------------------------
    using C = std::complex<double>;

    double analogDb (Type type, double f, double f0, double gainDb, double q)
    {
        const C s (0.0, f / f0);
        const double A = std::pow (10.0, gainDb / 40.0), sA = std::sqrt (A);
        C h (1.0);

        switch (type)
        {
            case Type::peak:        h = (s * s + s * (A / q) + 1.0) / (s * s + s / (A * q) + 1.0); break;
            case Type::lowShelf:    h = A * (s * s + s * (sA / q) + A) / (A * s * s + s * (sA / q) + 1.0); break;
            case Type::highShelf:   h = A * (A * s * s + s * (sA / q) + 1.0) / (s * s + s * (sA / q) + A); break;
            case Type::bandPass:    h = (s / q) / (s * s + s / q + 1.0); break;
            case Type::notch:       h = (s * s + 1.0) / (s * s + s / q + 1.0); break;
            default:                break;
        }

        return 20.0 * std::log10 (std::max (std::abs (h), 1.0e-12));
    }

    // Butterworth of order n: |H|^2 = 1 / (1 + (fc/f)^(2n)) for the high-pass.
    double butterworthHighPassDb (double f, double fc, int order)
    {
        return -10.0 * std::log10 (1.0 + std::pow (fc / f, 2.0 * order));
    }

    double butterworthLowPassDb (double f, double fc, int order)
    {
        return -10.0 * std::log10 (1.0 + std::pow (f / fc, 2.0 * order));
    }

    bool sectionsStable (const eqd::Sections& s)
    {
        for (int i = 0; i < s.count; ++i)
        {
            const auto& c = s.s[(size_t) i];

            if (! (std::isfinite (c.b0) && std::isfinite (c.b1) && std::isfinite (c.b2) && std::isfinite (c.a1) && std::isfinite (c.a2)))
                return false;

            if (! (std::abs (c.a2) < 1.0 && std::abs (c.a1) < 1.0 + c.a2))
                return false;
        }

        return true;
    }

    // What the fixed-layout EQ of versions up to 0.1.1 did, from the juce::dsp recipes it was built from.
    struct OldEq
    {
        bool hpfOn = true;
        float hpfFreq = 80.0f;
        int hpfSlope = 0;                                                   // 0 = 12 dB/oct, 1 = 24 dB/oct
        float lowShelfFreq = 120.0f, lowShelfGain = 0.0f;
        float bellFreq[3] { 250.0f, 1000.0f, 4000.0f }, bellGain[3] {}, bellQ[3] { 1.0f, 1.0f, 1.0f };
        float highShelfFreq = 8000.0f, highShelfGain = 0.0f;
        bool lpfOn = false;
        float lpfFreq = 18000.0f;

        double responseDb (double f, double sr) const
        {
            using Co = juce::dsp::IIR::Coefficients<float>;
            const float q = 0.70710678f;
            const float lim = (float) (sr * 0.49);
            auto gain = [] (float db) { return std::pow (10.0f, db * 0.05f); };
            auto mag = [&] (const Co::Ptr& c) { return c->getMagnitudeForFrequency (f, sr); };

            double m = 1.0;

            if (hpfOn)
            {
                const double h = mag (Co::makeHighPass (sr, juce::jmin (hpfFreq, lim), q));
                m *= hpfSlope == 1 ? h * h : h;
            }

            m *= mag (Co::makeLowShelf (sr, juce::jmin (lowShelfFreq, lim), q, gain (lowShelfGain)));

            for (int i = 0; i < 3; ++i)
                m *= mag (Co::makePeakFilter (sr, juce::jmin (bellFreq[i], lim), bellQ[i], gain (bellGain[i])));

            m *= mag (Co::makeHighShelf (sr, juce::jmin (highShelfFreq, lim), q, gain (highShelfGain)));

            if (lpfOn)
                m *= mag (Co::makeLowPass (sr, juce::jmin (lpfFreq, lim), q));

            return toDb (m);
        }

        // The state a 0.1.1 build would have saved: every parameter, under the old ids.
        juce::MemoryBlock toState() const
        {
            juce::ValueTree tree ("PARAMS");

            auto add = [&tree] (const char* id, double v)
            {
                juce::ValueTree p ("PARAM");
                p.setProperty ("id", id, nullptr);
                p.setProperty ("value", v, nullptr);
                tree.appendChild (p, nullptr);
            };

            add ("hpfOn", hpfOn ? 1.0 : 0.0);
            add ("hpfFreq", hpfFreq);
            add ("hpfSlope", hpfSlope);
            add ("lowShelfFreq", lowShelfFreq);
            add ("lowShelfGain", lowShelfGain);

            for (int i = 0; i < 3; ++i)
            {
                const auto n = "bell" + juce::String (i + 1);
                add ((n + "Freq").toRawUTF8(), bellFreq[i]);
                add ((n + "Gain").toRawUTF8(), bellGain[i]);
                add ((n + "Q").toRawUTF8(), bellQ[i]);
            }

            add ("highShelfFreq", highShelfFreq);
            add ("highShelfGain", highShelfGain);
            add ("lpfOn", lpfOn ? 1.0 : 0.0);
            add ("lpfFreq", lpfFreq);

            juce::MemoryBlock block;

            if (auto xml = tree.createXml())
                juce::AudioProcessor::copyXmlToBinary (*xml, block);

            return block;
        }
    };
}

class EqTests final : public juce::UnitTest
{
public:
    EqTests() : juce::UnitTest ("EQ", "headroom") {}

    void runTest() override
    {
        beginTest ("EQ: eight bands, band 1 is the 80 Hz low cut, the rest wait where a voice needs them");
        testDefaults();

        beginTest ("EQ design: matches the analog prototype at DC, at the centre frequency and at Nyquist");
        testDesignMatchesAnalog();

        beginTest ("EQ design: stable and finite everywhere");
        testDesignStable();

        beginTest ("EQ design: the top octave is not cramped");
        testTopOctave();

        beginTest ("EQ bands: low cut and high cut slopes (6 / 12 / 24 / 48 dB/oct)");
        testCuts();

        beginTest ("EQ bands: shelf gain, peak, notch, band pass, tilt");
        testBandShapes();

        beginTest ("EQ: getResponseDb and getBandResponseDb match the DSP within 0.1 dB");
        testResponseVsDsp();

        beginTest ("EQ: old saved states load with the same sound");
        testMigration();

        beginTest ("EQ: an old state in the saved model comes up as bands");
        testOldStateThroughController();

        beginTest ("EQ: presets keep their sound");
        testPresets();

        beginTest ("EQ: no clicks when bands are switched, retyped or moved");
        testClicks();

        beginTest ("EQ: mono and stereo, block sizes");
        testFormats();

        beginTest ("EQ analyzer: off costs nothing, on feeds pre and post, a full FIFO never blocks");
        testAnalyzerFeed();

        beginTest ("EQ solo");
        testSolo();

        beginTest ("EQ: no allocation on the audio thread");
        testNoAllocation();

        beginTest ("EQ: random automation at odd block sizes stays finite and settles");
        testRandomAutomation();

        beginTest ("EQ: CPU cost");
        testCpu();

        beginTest ("spectrum analyzer");
        testSpectrumAnalyzer();

        beginTest ("EQ editor: typed values");
        testTypedValues();
    }

private:
    // -----------------------------------------------------------------------------------------------
    void testDefaults()
    {
        Equalizer eq;
        const float expectedFreq[] { 80.0f, 100.0f, 250.0f, 1000.0f, 3000.0f, 6000.0f, 10000.0f, 16000.0f };

        for (int b = 0; b < Equalizer::kNumBands; ++b)
        {
            const auto s = eq.getBandSettings (b);
            expect (s.on == (b == 0), "only band 1 is on by default (band " + juce::String (b + 1) + ")");
            expectWithinAbsoluteError (s.freq, expectedFreq[b], 0.01f);
            expectWithinAbsoluteError (s.gainDb, 0.0f, 1.0e-6f);
        }

        const auto b1 = eq.getBandSettings (0);
        expect (b1.type == Type::lowCut && b1.slope == eqd::kSlope12, "band 1 is a low cut at 12 dB/oct");
        expectWithinAbsoluteError (b1.q, 0.7071f, 0.001f);

        // The default chain is what the first-run mic chain always was: a 12 dB/oct high-pass at 80 Hz.
        eq.prepareToPlay (kRate, 512);
        const double at30 = measureGainDb (eq, kRate, 30.0), at1k = measureGainDb (eq, kRate, 1000.0);
        expect (at30 <= -15.0 && std::abs (at1k) < 0.05, "default is a plain 80 Hz high-pass (" + juce::String (at30, 2) + " / " + juce::String (at1k, 3) + " dB)");
        expectWithinAbsoluteError (eq.getResponseDb (80.0), -3.01f, 0.05f);
    }

    // -----------------------------------------------------------------------------------------------
    void testDesignMatchesAnalog()
    {
        double worst = 0.0;
        int cases = 0;

        std::mt19937 rng (7);
        std::uniform_real_distribution<double> u (0.0, 1.0);

        for (double sr : { 44100.0, 48000.0, 96000.0, 192000.0 })
        {
            for (Type type : { Type::peak, Type::lowShelf, Type::highShelf, Type::bandPass })
            {
                for (int trial = 0; trial < 300; ++trial)
                {
                    const double f0 = 20.0 * std::pow (0.49 * sr / 20.0, u (rng));
                    const double q = 0.1 * std::pow (180.0, u (rng));
                    const double g = type == Type::bandPass ? 0.0 : -24.0 + 48.0 * u (rng);

                    eqd::BandParams p;
                    p.type = type;
                    p.freq = f0;
                    p.q = q;
                    p.gainDb = g;
                    const auto s = eqd::design (p, sr);

                    if (s.count == 0)
                        continue;

                    ++cases;

                    // DC (0.01 Hz stands in), the centre frequency and Nyquist are matched exactly. (The band pass has a
                    // true zero at DC, where the two only agree that it is a zero.)
                    for (double f : { 0.01, f0, 0.5 * sr })
                    {
                        if (type == Type::bandPass && f < 1.0)
                            continue;

                        worst = juce::jmax (worst, std::abs (eqd::responseDb (s, f, sr) - analogDb (type, f, f0, g, q)));
                    }
                }
            }

            // Low-pass and high-pass: exact at the centre frequency (-3 dB for a Butterworth pair, Q at other Qs).
            for (int trial = 0; trial < 100; ++trial)
            {
                const double f0 = 20.0 * std::pow (0.45 * sr / 20.0, u (rng));
                const int slope = juce::jmin (3, (int) (u (rng) * 4.0));       // 6, 12, 24, 48

                for (Type type : { Type::lowCut, Type::highCut })
                {
                    eqd::BandParams p;
                    p.type = type;
                    p.freq = f0;
                    p.slope = slope;
                    const auto s = eqd::design (p, sr);
                    ++cases;

                    // Exactly -3.01 dB at the corner for the matched stages. (The 6 dB/oct low-pass follows the analog shape
                    // at DC and Nyquist instead, which puts its corner a little off when it sits near Nyquist.)
                    const double corner = eqd::responseDb (s, f0, sr) + 3.0103;

                    if (! (type == Type::highCut && slope == 0))
                        worst = juce::jmax (worst, std::abs (corner));
                    else
                        expect (std::abs (corner) < 1.0, "6 dB/oct low-pass corner within 1 dB even near Nyquist (" + juce::String (corner, 2) + ")");
                }
            }
        }

        logMessage ("EQ design: " + juce::String (cases) + " random designs, worst deviation from the analog prototype at DC / centre / Nyquist: "
                    + juce::String (worst, 5) + " dB");
        expect (worst < 0.001, "matched at DC, centre and Nyquist (" + juce::String (worst, 5) + " dB)");
    }

    // -----------------------------------------------------------------------------------------------
    void testDesignStable()
    {
        std::mt19937 rng (11);
        std::uniform_real_distribution<double> u (0.0, 1.0);
        int cases = 0, bad = 0;

        for (int trial = 0; trial < 60000; ++trial)
        {
            const double sr = trial % 4 == 0 ? 44100.0 : (trial % 4 == 1 ? 48000.0 : (trial % 4 == 2 ? 96000.0 : 192000.0));

            eqd::BandParams p;
            p.type = (Type) (int) (u (rng) * 8.0);
            p.freq = 20.0 * std::pow (1000.0, u (rng));
            p.q = 0.1 * std::pow (180.0, u (rng));
            p.gainDb = -24.0 + 48.0 * u (rng);
            p.slope = (int) (u (rng) * 5.0);

            const auto s = eqd::design (p, sr);
            ++cases;

            bool ok = sectionsStable (s);

            for (double f : { 20.0, 200.0, 2000.0, 20000.0 })
                ok = ok && std::isfinite (eqd::responseDb (s, f, sr));

            if (! ok)
            {
                if (++bad <= 3)
                    logMessage ("unstable design: type " + juce::String ((int) p.type) + " f " + juce::String (p.freq) + " Q " + juce::String (p.q)
                                + " gain " + juce::String (p.gainDb) + " slope " + juce::String (p.slope) + " sr " + juce::String (sr));
            }
        }

        expectEquals (bad, 0, juce::String (bad) + " of " + juce::String (cases) + " random designs were unstable or not finite");
    }

    // -----------------------------------------------------------------------------------------------
    void testTopOctave()
    {
        // A bell at 12 kHz on a 48 kHz system: the bilinear transform squeezes it against Nyquist (its gain there is 0 dB
        // whatever you ask for). Ours follows the analog bell, whose gain at 24 kHz is still well above 1.
        const double f0 = 12000.0, q = 1.0, g = 6.0;
        eqd::BandParams p;
        p.type = Type::peak;
        p.freq = f0;
        p.q = q;
        p.gainDb = g;
        const auto s = eqd::design (p, kRate);

        const double analogAtNyquist = analogDb (Type::peak, 0.5 * kRate, f0, g, q);
        const double ours = eqd::responseDb (s, 0.5 * kRate, kRate);
        logMessage ("EQ: +6 dB bell at 12 kHz, gain at Nyquist: analog " + juce::String (analogAtNyquist, 2) + " dB, ours " + juce::String (ours, 2)
                    + " dB (a bilinear-transform bell has 0 dB there)");
        expect (analogAtNyquist > 1.0 && std::abs (ours - analogAtNyquist) < 0.001, "Nyquist gain follows the analog bell");

        // ... and over the whole top octave the shape stays close to the analog one.
        double worst = 0.0;

        for (double f = 6000.0; f <= 21000.0; f += 250.0)
            worst = juce::jmax (worst, std::abs (eqd::responseDb (s, f, kRate) - analogDb (Type::peak, f, f0, g, q)));

        logMessage ("EQ: worst deviation from the analog bell between 6 and 21 kHz: " + juce::String (worst, 3) + " dB");
        expect (worst < 0.6, "top-octave shape close to analog (" + juce::String (worst, 3) + " dB)");
    }

    // -----------------------------------------------------------------------------------------------
    void testCuts()
    {
        struct Cut { int slope, order; };

        for (const Cut cut : { Cut { 0, 1 }, Cut { 1, 2 }, Cut { 2, 4 }, Cut { 3, 8 } })
        {
            for (Type type : { Type::lowCut, Type::highCut })
            {
                Equalizer eq;
                Band b;
                b.number = 1;
                b.type = type;
                b.freq = type == Type::lowCut ? 1000.0f : 3000.0f;
                b.slope = cut.slope;
                b.q = 0.70710678f;
                apply (eq, b);

                const double f0 = b.freq;
                const auto label = juce::String (type == Type::lowCut ? "low cut " : "high cut ") + juce::String (Equalizer::slopeName (cut.slope));

                for (double ratio : { 0.25, 0.5, 1.0, 2.0, 4.0 })
                {
                    const double f = type == Type::lowCut ? f0 * ratio : f0 / ratio;

                    if (f < 20.0 || f > 20000.0)
                        continue;

                    const double expected = type == Type::lowCut ? butterworthHighPassDb (f, f0, cut.order) : butterworthLowPassDb (f, f0, cut.order);

                    if (expected < -40.0)
                        continue;           // (deep in the stop band a digital filter follows the analog one to a few dB, not a few hundredths)

                    const double got = eq.getResponseDb (f, kRate);

                    // Matched at DC, the corner and Nyquist, so within a few tenths of a dB of the analog Butterworth in between.
                    const double tolerance = cut.slope == 0 && type == Type::highCut ? 0.8 : 0.35;
                    expect (std::abs (got - expected) < tolerance,
                            label + " at " + juce::String (f, 0) + " Hz: " + juce::String (got, 2) + " dB, Butterworth says " + juce::String (expected, 2));
                }

                expectWithinAbsoluteError (eq.getResponseDb (f0, kRate), -3.01f, type == Type::highCut && cut.slope == 0 ? 0.1f : 0.03f);
            }
        }

        // The DSP really does that: a 48 dB/oct low cut at 1 kHz, a sine at 500 Hz.
        {
            Equalizer eq;
            Band b;
            b.type = Type::lowCut;
            b.freq = 1000.0f;
            b.slope = 3;
            apply (eq, b);
            const double measured = dspGainDb (eq, 500.0);
            expect (std::abs (measured + 48.2) < 0.5, "48 dB/oct low cut measures -48 dB one octave below (" + juce::String (measured, 2) + ")");
        }

        // Q is the resonance of the last stage: 0.7071 is flat, more makes a peak at the corner.
        {
            Equalizer flat, resonant;
            Band b;
            b.type = Type::lowCut;
            b.freq = 1000.0f;
            b.slope = 2;
            b.q = 0.70710678f;
            apply (flat, b);
            b.q = 2.0f;
            apply (resonant, b);

            double peakFlat = -100.0, peakResonant = -100.0;

            for (double f = 500.0; f < 4000.0; f *= 1.02)
            {
                peakFlat = juce::jmax (peakFlat, (double) flat.getResponseDb (f, kRate));
                peakResonant = juce::jmax (peakResonant, (double) resonant.getResponseDb (f, kRate));
            }

            expect (peakFlat < 0.01, "Q 0.7071 has no peak (" + juce::String (peakFlat, 3) + ")");
            expect (peakResonant > 3.0, "Q 2 has a resonance (" + juce::String (peakResonant, 2) + ")");
        }
    }

    // -----------------------------------------------------------------------------------------------
    void testBandShapes()
    {
        auto response = [] (const Band& b, double f)
        {
            Equalizer eq;
            setParam (eq.apvts, "b1On", 0.0f);          // (the default low cut is not what is being looked at)
            apply (eq, b);
            return (double) eq.getResponseDb (f, kRate);
        };

        // Peak: the requested gain at the centre, nothing far away.
        for (float g : { -24.0f, -6.0f, 3.0f, 12.0f, 24.0f })
        {
            Band b;
            b.number = 4;
            b.type = Type::peak;
            b.freq = 1000.0f;
            b.gain = g;
            b.q = 1.4f;
            expect (std::abs (response (b, 1000.0) - g) < 0.01, "peak " + juce::String (g) + " dB at its centre");
            expect (std::abs (response (b, 20.0)) < 0.3 && std::abs (response (b, 20000.0)) < 0.5, "peak leaves the far ends alone");
        }

        // Shelves: the plateau is the gain, half of it at the corner (Q 0.7071).
        for (float g : { -12.0f, 6.0f })
        {
            Band low;
            low.number = 2;
            low.type = Type::lowShelf;
            low.freq = 500.0f;
            low.gain = g;
            low.q = 0.70710678f;
            expect (std::abs (response (low, 20.0) - g) < 0.1, "low shelf plateau " + juce::String (g));
            expect (std::abs (response (low, 500.0) - 0.5 * g) < 0.05, "low shelf is half its gain at the corner");
            expect (std::abs (response (low, 20000.0)) < 0.1, "low shelf leaves the highs alone");

            Band high = low;
            high.number = 7;
            high.type = Type::highShelf;
            expect (std::abs (response (high, 20000.0) - g) < 0.15, "high shelf plateau " + juce::String (g) + " (" + juce::String (response (high, 20000.0), 2) + ")");
            expect (std::abs (response (high, 500.0) - 0.5 * g) < 0.05, "high shelf is half its gain at the corner");
            expect (std::abs (response (high, 20.0)) < 0.1, "high shelf leaves the lows alone");
        }

        // Notch: a real null at the centre, flat elsewhere.
        {
            Band b;
            b.number = 3;
            b.type = Type::notch;
            b.freq = 1000.0f;
            b.q = 4.0f;
            expect (response (b, 1000.0) < -60.0, "notch is at least 60 dB deep (" + juce::String (response (b, 1000.0), 1) + ")");
            expect (std::abs (response (b, 100.0)) < 0.1 && std::abs (response (b, 10000.0)) < 0.1, "notch is flat away from its centre");
            expect (std::abs (response (b, 1000.0 * 1.125) + 3.0) < 1.5, "notch is about 3 dB down half a bandwidth away");

            Equalizer eq;
            apply (eq, b);
            expect (dspGainDb (eq, 1000.0) < -40.0, "the DSP notches a 1 kHz sine by more than 40 dB (" + juce::String (dspGainDb (eq, 1000.0), 1) + ")");
        }

        // Band pass: 0 dB at the centre, falling away on both sides.
        {
            Band b;
            b.number = 5;
            b.type = Type::bandPass;
            b.freq = 2000.0f;
            b.q = 2.0f;
            expect (std::abs (response (b, 2000.0)) < 0.02, "band pass is 0 dB at its centre");
            expect (response (b, 200.0) < -18.0 && response (b, 20000.0) < -18.0, "band pass falls away on both sides");
        }

        // Tilt: lows down, highs up by the same amount, nothing at the pivot.
        {
            Band b;
            b.number = 6;
            b.type = Type::tiltShelf;
            b.freq = 1000.0f;
            b.gain = 6.0f;
            b.q = 0.70710678f;
            expect (std::abs (response (b, 1000.0)) < 0.05, "tilt is 0 dB at the pivot");
            expect (std::abs (response (b, 20.0) + 6.0) < 0.15, "tilt: lows by -gain (" + juce::String (response (b, 20.0), 2) + ")");
            expect (std::abs (response (b, 20000.0) - 6.0) < 0.4, "tilt: highs by +gain (" + juce::String (response (b, 20000.0), 2) + ")");
            b.gain = -6.0f;
            expect (response (b, 20.0) > 5.8 && response (b, 20000.0) < -5.4, "negative tilt turns it around");
        }

        // A 0 dB peak or shelf is exactly flat and cheap.
        {
            Band b;
            b.number = 4;
            b.gain = 0.0f;
            expectEquals (response (b, 1000.0), 0.0);
            expectEquals (eqd::design ({ Type::peak, 1000.0, 0.0, 1.0, 1 }, kRate).count, 0);
        }
    }

    // -----------------------------------------------------------------------------------------------
    void testResponseVsDsp()
    {
        // A busy setting: every band type once, some of them close together.
        Equalizer eq;
        Band bands[8];
        bands[0] = { 1, true, Type::lowCut, 70.0f, 0.0f, 0.70710678f, 2 };
        bands[1] = { 2, true, Type::lowShelf, 180.0f, 3.5f, 0.8f, 1 };
        bands[2] = { 3, true, Type::peak, 320.0f, -4.0f, 2.0f, 1 };
        bands[3] = { 4, true, Type::peak, 2600.0f, 5.0f, 1.2f, 1 };
        bands[4] = { 5, true, Type::notch, 5200.0f, 0.0f, 6.0f, 1 };
        bands[5] = { 6, true, Type::tiltShelf, 900.0f, 2.0f, 0.7f, 1 };
        bands[6] = { 7, true, Type::highShelf, 9000.0f, -5.0f, 0.7f, 1 };
        bands[7] = { 8, true, Type::highCut, 17000.0f, 0.0f, 0.70710678f, 1 };

        for (auto& b : bands)
            apply (eq, b);

        double worst = 0.0;

        for (double sr : { 44100.0, 48000.0, 96000.0 })
        {
            eq.prepareToPlay (sr, 512);

            for (double f : { 25.0, 60.0, 110.0, 250.0, 500.0, 800.0, 1200.0, 2600.0, 4000.0, 4900.0, 5500.0, 7000.0, 12000.0, 16000.0, 19000.0 })
            {
                if (f > 0.45 * sr)
                    continue;

                const double shown = eq.getResponseDb (f, sr);
                const double measured = measureGainDb (eq, sr, f, 0.2, 1.0, 512);
                worst = juce::jmax (worst, std::abs (shown - measured));
            }
        }

        logMessage ("EQ: getResponseDb vs measured DSP, 8 bands, 3 sample rates: worst " + juce::String (worst, 4) + " dB");
        expect (worst < 0.1, "getResponseDb matches the DSP within 0.1 dB (" + juce::String (worst, 4) + ")");

        // The parts add up to the whole.
        eq.prepareToPlay (kRate, 512);

        for (double f : { 100.0, 1000.0, 8000.0 })
        {
            double sum = 0.0;

            for (int b = 0; b < 8; ++b)
                sum += eq.getBandResponseDb (b, f, kRate);

            expect (std::abs (sum - eq.getResponseDb (f, kRate)) < 1.0e-3, "band curves add up to the combined curve");
        }

        // A band that is off has no curve and no influence.
        setParam (eq.apvts, "b3On", 0.0f);
        expectEquals (eq.getBandResponseDb (2, 320.0, kRate), 0.0f);

        // Above the limit the frequency clamps instead of asserting.
        expect (std::isfinite (eq.getResponseDb (24000.0, 48000.0)));
    }

    // -----------------------------------------------------------------------------------------------
    // Old states -> new bands
    // -----------------------------------------------------------------------------------------------
    void loadOld (Equalizer& eq, const OldEq& old)
    {
        const auto block = old.toState();
        eq.setStateInformation (block.getData(), (int) block.getSize());
    }

    // Worst difference, in dB, between what the old EQ did and what the converted state does.
    double compareToOld (const OldEq& old, double lowHz, double highHz, double sr = kRate)
    {
        Equalizer eq;
        loadOld (eq, old);
        eq.prepareToPlay (sr, 512);

        double worst = 0.0;

        for (double f = lowHz; f <= highHz; f *= 1.05)
            worst = juce::jmax (worst, std::abs ((double) eq.getResponseDb (f, sr) - old.responseDb (f, sr)));

        return worst;
    }

    void testMigration()
    {
        // The state 0.1.1 saved for an untouched EQ.
        {
            OldEq old;
            Equalizer eq;
            loadOld (eq, old);

            for (int b = 0; b < 8; ++b)
            {
                const auto s = eq.getBandSettings (b);
                expect (s.on == (b == 0), "untouched old EQ: only the high-pass is on");
            }

            expect (eq.getBandSettings (0).type == Type::lowCut && eq.getBandSettings (0).slope == 1, "old high-pass -> band 1 low cut, 12 dB/oct");
            expect (compareToOld (old, 20.0, 20000.0) < 0.05, "untouched old EQ sounds the same (the whole range)");
        }

        // Every stage in use: the mapping, band by band.
        {
            OldEq old;
            old.hpfOn = true;
            old.hpfFreq = 95.0f;
            old.hpfSlope = 1;
            old.lowShelfFreq = 160.0f;
            old.lowShelfGain = 3.0f;
            old.bellFreq[0] = 320.0f;  old.bellGain[0] = -3.5f; old.bellQ[0] = 1.6f;
            old.bellFreq[1] = 1400.0f; old.bellGain[1] = 0.0f;  old.bellQ[1] = 1.0f;
            old.bellFreq[2] = 3800.0f; old.bellGain[2] = 4.0f;  old.bellQ[2] = 0.8f;
            old.highShelfFreq = 9000.0f;
            old.highShelfGain = 2.5f;
            old.lpfOn = true;
            old.lpfFreq = 15000.0f;

            Equalizer eq;
            loadOld (eq, old);

            const auto b1 = eq.getBandSettings (0), b2 = eq.getBandSettings (1), b3 = eq.getBandSettings (2), b4 = eq.getBandSettings (3),
                       b5 = eq.getBandSettings (4), b6 = eq.getBandSettings (5), b7 = eq.getBandSettings (6), b8 = eq.getBandSettings (7);

            expect (b1.on && b1.type == Type::lowCut && std::abs (b1.freq - 95.0f) < 0.01f && b1.slope == eqd::kSlopeLinkwitzRiley24,
                    "24 dB/oct high-pass -> band 1 low cut, 24 dB/oct (LR)");
            expect (b2.on && b2.type == Type::lowShelf && std::abs (b2.freq - 160.0f) < 0.01f && std::abs (b2.gainDb - 3.0f) < 0.001f, "low shelf -> band 2");
            expect (b3.on && b3.type == Type::peak && std::abs (b3.gainDb + 3.5f) < 0.001f && std::abs (b3.q - 1.6f) < 0.001f, "bell 1 -> band 3");
            expect (! b4.on && b4.type == Type::peak && std::abs (b4.freq - 1400.0f) < 0.1f, "a flat bell 2 -> band 4, off, where it was");
            expect (b5.on && b5.type == Type::peak && std::abs (b5.freq - 3800.0f) < 0.1f && std::abs (b5.q - 0.8f) < 0.001f, "bell 3 -> band 5");
            expect (b6.on && b6.type == Type::highShelf && std::abs (b6.freq - 9000.0f) < 0.1f && std::abs (b6.gainDb - 2.5f) < 0.001f, "high shelf -> band 6");
            expect (b7.on && b7.type == Type::highCut && std::abs (b7.freq - 15000.0f) < 0.1f && b7.slope == 1, "low-pass -> band 7 high cut, 12 dB/oct");
            expect (! b8.on, "band 8 stays off");

            // How far the two drift apart as the frequency goes up, one old stage at a time (informational).
            {
                struct Probe { const char* name; std::function<void (OldEq&)> only; };

                const Probe probes[] {
                    { "low shelf +3 dB at 160 Hz", [] (OldEq& o) { o.lowShelfFreq = 160.0f; o.lowShelfGain = 3.0f; } },
                    { "bell -3.5 dB at 320 Hz Q 1.6", [] (OldEq& o) { o.bellFreq[0] = 320.0f; o.bellGain[0] = -3.5f; o.bellQ[0] = 1.6f; } },
                    { "bell +4 dB at 3.8 kHz Q 0.8", [] (OldEq& o) { o.bellFreq[2] = 3800.0f; o.bellGain[2] = 4.0f; o.bellQ[2] = 0.8f; } },
                    { "high shelf +2.5 dB at 9 kHz", [] (OldEq& o) { o.highShelfFreq = 9000.0f; o.highShelfGain = 2.5f; } },
                    { "low-pass at 15 kHz", [] (OldEq& o) { o.lpfOn = true; o.lpfFreq = 15000.0f; } },
                    { "low-pass at 8 kHz", [] (OldEq& o) { o.lpfOn = true; o.lpfFreq = 8000.0f; } },
                };

                for (const auto& probe : probes)
                {
                    OldEq only;
                    only.hpfOn = false;
                    probe.only (only);
                    logMessage (juce::String ("EQ migration, ") + probe.name + ": worst difference up to 2 kHz " + juce::String (compareToOld (only, 20.0, 2000.0), 3)
                                + ", 4 kHz " + juce::String (compareToOld (only, 20.0, 4000.0), 3) + ", 6 kHz " + juce::String (compareToOld (only, 20.0, 6000.0), 3)
                                + ", 10 kHz " + juce::String (compareToOld (only, 20.0, 10000.0), 3) + ", 20 kHz " + juce::String (compareToOld (only, 20.0, 20000.0), 3) + " dB");
                }
            }

            const double audible = compareToOld (old, 20.0, kSameSoundLimitHz);
            const double near = compareToOld (old, 20.0, kNearLimitHz);
            const double wholeRange = compareToOld (old, 20.0, 20000.0);
            logMessage ("EQ migration, every old stage in use: worst difference 20 Hz - " + juce::String (kSameSoundLimitHz / 1000.0, 0) + " kHz "
                        + juce::String (audible, 4) + " dB, up to " + juce::String (kNearLimitHz / 1000.0, 0) + " kHz " + juce::String (near, 3)
                        + " dB, up to 20 kHz " + juce::String (wholeRange, 3) + " dB (the low-pass at 15 kHz was cramped against Nyquist)");
            expect (audible < 0.1, "old state sounds the same (" + juce::String (audible, 4) + " dB)");
            expect (near < 0.25, "and stays within a quarter of a dB up to 6 kHz (" + juce::String (near, 3) + " dB)");
            expect (wholeRange < 10.0, "and is sane at the very top (" + juce::String (wholeRange, 3) + " dB)");
        }

        // Filters that were off stay off; the old LPF at a frequency on the old scale.
        {
            OldEq old;
            old.hpfOn = false;
            old.lpfOn = false;
            old.lowShelfGain = -2.0f;
            Equalizer eq;
            loadOld (eq, old);
            expect (! eq.getBandSettings (0).on && ! eq.getBandSettings (6).on, "an off high-pass and low-pass stay off");
            expect (compareToOld (old, 20.0, kSameSoundLimitHz) < 0.1, "low shelf only");
        }

        // The converted state is a normal new-format state afterwards: saving and loading it again changes nothing.
        {
            OldEq old;
            old.hpfSlope = 1;
            old.bellGain[0] = 2.0f;
            Equalizer eq;
            loadOld (eq, old);

            juce::MemoryBlock saved;
            eq.getStateInformation (saved);

            Equalizer again;
            again.setStateInformation (saved.getData(), (int) saved.getSize());

            for (int b = 0; b < 8; ++b)
            {
                const auto a = eq.getBandSettings (b), c = again.getBandSettings (b);
                expect (a.on == c.on && a.type == c.type && a.slope == c.slope && a.freq == c.freq && a.gainDb == c.gainDb && a.q == c.q,
                        "a converted state round-trips (band " + juce::String (b + 1) + ")");
            }

            juce::ValueTree fresh = eq.apvts.copyState();
            expect (! Equalizer::migrateLegacyState (fresh), "a new-format state is not converted again");
        }

        // The old 24 dB/oct rumble filter is what it was: two cascaded 12 dB/oct stages.
        {
            OldEq old;
            old.hpfSlope = 1;
            old.hpfFreq = 120.0f;
            const double d = compareToOld (old, 20.0, 20000.0);
            expect (d < 0.1, "24 dB/oct high-pass keeps its Linkwitz-Riley shape (" + juce::String (d, 4) + " dB)");

            Equalizer eq;
            loadOld (eq, old);
            expectWithinAbsoluteError (eq.getResponseDb (120.0, kRate), -6.02f, 0.05f);     // LR4: -6 dB at the corner
        }

        // Other sample rates.
        {
            OldEq old;
            old.bellFreq[2] = 3000.0f;
            old.bellGain[2] = 5.0f;
            old.highShelfGain = 3.0f;
            for (double sr : { 44100.0, 96000.0 })
                expect (compareToOld (old, 20.0, kSameSoundLimitHz, sr) < 0.1, "migration at " + juce::String (sr) + " Hz");
        }

        // Garbage is ignored, as before.
        {
            Equalizer eq;
            const char junk[] = "not a state";
            eq.setStateInformation (junk, (int) sizeof (junk));
            expect (eq.getBandSettings (0).on && eq.getBandSettings (0).type == Type::lowCut);
        }
    }

    // -----------------------------------------------------------------------------------------------
    // The way it really happens: state.xml has an EQ slot with a base64 state from 0.1.1, the controller builds the chain.
    void testOldStateThroughController()
    {
        AppController::Options options;
        options.demo = true;
        options.persist = false;
        options.startBackend = false;

        AppController controller (options);
        controller.start();

        OldEq old;
        old.hpfFreq = 95.0f;
        old.hpfSlope = 1;
        old.bellFreq[0] = 320.0f;
        old.bellGain[0] = -3.5f;
        old.bellQ[0] = 1.6f;
        old.highShelfFreq = 9000.0f;
        old.highShelfGain = 2.0f;
        const auto block = old.toState();

        juce::ValueTree slot (ids::SLOT);
        slot.setProperty (ids::uid, "oldeq", nullptr);
        slot.setProperty (ids::crashId, 990, nullptr);
        slot.setProperty (ids::type, values::slotBuiltin, nullptr);
        slot.setProperty (ids::builtinId, "eq", nullptr);
        slot.setProperty (ids::name, "EQ", nullptr);
        slot.setProperty (ids::bypassed, 0, nullptr);
        slot.setProperty (ids::disabled, 0, nullptr);
        slot.setProperty (ids::state, juce::Base64::toBase64 (block.getData(), block.getSize()), nullptr);
        AppState::getFx (controller.getState().getMicChannel()).appendChild (slot, nullptr);
        controller.rebuildChain (values::micChannelId);

        auto* eq = dynamic_cast<Equalizer*> (controller.getFxProcessor ("oldeq"));
        expect (eq != nullptr, "the slot came up");

        if (eq == nullptr)
            return;

        const auto b1 = eq->getBandSettings (0), b3 = eq->getBandSettings (2), b6 = eq->getBandSettings (5);
        expect (b1.on && b1.type == Type::lowCut && std::abs (b1.freq - 95.0f) < 0.01f && b1.slope == eqd::kSlopeLinkwitzRiley24, "the high-pass is band 1, 24 dB/oct");
        expect (b3.on && b3.type == Type::peak && std::abs (b3.gainDb + 3.5f) < 0.001f, "the bell is band 3");
        expect (b6.on && b6.type == Type::highShelf && std::abs (b6.gainDb - 2.0f) < 0.001f, "the high shelf is band 6");

        // And what is saved from now on is the new format.
        controller.captureFxStates (false);
        auto saved = AppState::describeSlot (AppState::getFx (controller.getState().getMicChannel()).getChildWithProperty (ids::uid, "oldeq"));
        juce::MemoryOutputStream decoded;
        juce::Base64::convertFromBase64 (decoded, saved.stateBase64);
        auto xml = juce::AudioProcessor::getXmlFromBinary (decoded.getData(), (int) decoded.getDataSize());
        expect (xml != nullptr && xml->getChildByAttribute ("id", "b1Freq") != nullptr && xml->getChildByAttribute ("id", "hpfFreq") == nullptr,
                "the saved state uses the band parameters");
    }

    // -----------------------------------------------------------------------------------------------
    void testPresets()
    {
        // Applied the way AppController does it: a fresh EQ, then the preset's values.
        auto presetEq = [] (const juce::String& id)
        {
            auto fx = BuiltinFx::create ("eq");
            const auto* preset = findMicPreset (id);

            if (preset != nullptr)
                for (auto& slot : preset->slots)
                    if (slot.builtinId == "eq")
                        for (auto& [paramId, value] : slot.params)
                            if (auto* p = fx->apvts.getParameter (paramId))
                                p->setValueNotifyingHost (p->convertTo0to1 (value));

            return fx;
        };

        OldEq clean;

        OldEq broadcast;
        broadcast.hpfFreq = 90.0f;
        broadcast.hpfSlope = 1;
        broadcast.lowShelfFreq = 150.0f;
        broadcast.lowShelfGain = 2.0f;
        broadcast.bellFreq[0] = 300.0f;  broadcast.bellGain[0] = -2.5f;  broadcast.bellQ[0] = 1.2f;
        broadcast.bellFreq[2] = 4000.0f; broadcast.bellGain[2] = 2.5f;   broadcast.bellQ[2] = 0.9f;
        broadcast.highShelfFreq = 10000.0f;
        broadcast.highShelfGain = 2.0f;

        OldEq noisy;
        noisy.hpfFreq = 120.0f;
        noisy.hpfSlope = 1;
        noisy.bellFreq[2] = 3000.0f;
        noisy.bellGain[2] = 1.5f;
        noisy.bellQ[2] = 1.0f;

        const std::pair<const char*, const OldEq*> presets[] { { "clean", &clean }, { "broadcast", &broadcast }, { "noisy", &noisy } };

        for (const auto& [id, old] : presets)
        {
            auto fx = presetEq (id);
            auto* eq = dynamic_cast<Equalizer*> (fx.get());
            expect (eq != nullptr);

            if (eq == nullptr)
                continue;

            eq->prepareToPlay (kRate, 512);
            double audible = 0.0, all = 0.0;

            for (double f = 20.0; f <= 20000.0; f *= 1.05)
            {
                const double d = std::abs ((double) eq->getResponseDb (f, kRate) - old->responseDb (f, kRate));
                all = juce::jmax (all, d);

                if (f <= kSameSoundLimitHz)
                    audible = juce::jmax (audible, d);
            }

            logMessage (juce::String ("EQ preset ") + id + ": worst difference to the old EQ " + juce::String (audible, 4) + " dB up to "
                        + juce::String (kSameSoundLimitHz / 1000.0, 0) + " kHz, " + juce::String (all, 3) + " dB up to 20 kHz");
            expect (audible < 0.1, juce::String ("preset ") + id + " sounds the same as before (" + juce::String (audible, 4) + " dB)");
            expect (all < 0.25, juce::String ("preset ") + id + " is within a quarter of a dB of before everywhere (" + juce::String (all, 4) + " dB)");
        }
    }

    // -----------------------------------------------------------------------------------------------
    // Clicks
    // -----------------------------------------------------------------------------------------------
    static double maxAbsDiff (const std::vector<float>& v, int from, int to)
    {
        double worst = 0.0;

        for (int i = juce::jmax (1, from); i < to; ++i)
            worst = juce::jmax (worst, (double) std::fabs (v[(size_t) i] - v[(size_t) i - 1]));

        return worst;
    }

    double clickRatio (Equalizer& p, double frequency, double amplitude, const std::function<void()>& change)
    {
        constexpr int block = 256;
        const int n = ((int) (kRate * 1.4) / block) * block;
        const int changeAt = ((int) (kRate * 0.4) / block) * block;

        auto left = makeSine (n, kRate, frequency, amplitude);
        auto right = left;

        runProcessor (p, left.data(), right.data(), changeAt, block);
        change();
        runProcessor (p, left.data() + changeAt, right.data() + changeAt, n - changeAt, block);

        const int window = (int) (kRate * 0.1);
        const double before = maxAbsDiff (left, changeAt - window, changeAt);
        const double after = maxAbsDiff (left, n - window, n);
        const double during = maxAbsDiff (left, changeAt, n - window);

        return during / juce::jmax (1.0e-9, before, after);
    }

    void testClicks()
    {
        struct Case
        {
            const char* name;
            double freq;
            std::function<void (Equalizer&)> setup, change;
        };

        auto none = [] (Equalizer&) {};

        const Case cases[] {
            { "peak gain 0 -> +12 dB", 1000.0, none, [] (Equalizer& e) { apply (e, { 4, true, Type::peak, 1000.0f, 12.0f, 1.0f, 1 }); } },
            { "peak gain 0 -> -24 dB", 1000.0, none, [] (Equalizer& e) { apply (e, { 4, true, Type::peak, 1000.0f, -24.0f, 1.0f, 1 }); } },
            { "peak frequency sweep", 1000.0, [] (Equalizer& e) { apply (e, { 4, true, Type::peak, 1000.0f, 12.0f, 1.0f, 1 }); },
                                              [] (Equalizer& e) { setParam (e.apvts, "b4Freq", 200.0f); } },
            { "peak Q sweep", 1000.0, [] (Equalizer& e) { apply (e, { 4, true, Type::peak, 1000.0f, 12.0f, 0.3f, 1 }); },
                                      [] (Equalizer& e) { setParam (e.apvts, "b4Q", 12.0f); } },
            { "band on", 1000.0, [] (Equalizer& e) { apply (e, { 4, false, Type::peak, 1000.0f, 12.0f, 1.0f, 1 }); },
                                 [] (Equalizer& e) { setParam (e.apvts, "b4On", 1.0f); } },
            { "band off", 1000.0, [] (Equalizer& e) { apply (e, { 4, true, Type::peak, 1000.0f, 12.0f, 1.0f, 1 }); },
                                  [] (Equalizer& e) { setParam (e.apvts, "b4On", 0.0f); } },
            { "low cut 80 -> 400 Hz", 200.0, none, [] (Equalizer& e) { setParam (e.apvts, "b1Freq", 400.0f); } },
            { "low cut off", 200.0, [] (Equalizer& e) { setParam (e.apvts, "b1Freq", 400.0f); }, [] (Equalizer& e) { setParam (e.apvts, "b1On", 0.0f); } },
            { "low cut on", 200.0, [] (Equalizer& e) { setParam (e.apvts, "b1Freq", 400.0f); setParam (e.apvts, "b1On", 0.0f); },
                                   [] (Equalizer& e) { setParam (e.apvts, "b1On", 1.0f); } },
            { "low cut 12 -> 24 dB/oct", 200.0, [] (Equalizer& e) { setParam (e.apvts, "b1Freq", 400.0f); }, [] (Equalizer& e) { setParam (e.apvts, "b1Slope", 2.0f); } },
            { "low cut 24 -> 48 dB/oct", 200.0, [] (Equalizer& e) { setParam (e.apvts, "b1Freq", 400.0f); setParam (e.apvts, "b1Slope", 2.0f); },
                                                [] (Equalizer& e) { setParam (e.apvts, "b1Slope", 3.0f); } },
            { "peak -> low shelf (type change)", 500.0, [] (Equalizer& e) { apply (e, { 4, true, Type::peak, 600.0f, 9.0f, 1.0f, 1 }); },
                                                        [] (Equalizer& e) { setParam (e.apvts, "b4Type", 1.0f); } },
            { "low cut -> notch (type change)", 200.0, [] (Equalizer& e) { setParam (e.apvts, "b1Freq", 400.0f); },
                                                       [] (Equalizer& e) { setParam (e.apvts, "b1Type", 5.0f); setParam (e.apvts, "b1Q", 3.0f); } },
            { "high cut on at 2 kHz", 5000.0, [] (Equalizer& e) { apply (e, { 8, false, Type::highCut, 2000.0f, 0.0f, 0.70710678f, 1 }); },
                                              [] (Equalizer& e) { setParam (e.apvts, "b8On", 1.0f); } },
            { "low shelf 0 -> +18 dB", 100.0, none, [] (Equalizer& e) { apply (e, { 2, true, Type::lowShelf, 200.0f, 18.0f, 0.70710678f, 1 }); } },
            { "tilt 0 -> +12 dB", 300.0, none, [] (Equalizer& e) { apply (e, { 6, true, Type::tiltShelf, 1000.0f, 12.0f, 0.70710678f, 1 }); } },
        };

        for (const auto& c : cases)
        {
            Equalizer eq;
            c.setup (eq);
            eq.prepareToPlay (kRate, 512);

            const double ratio = clickRatio (eq, c.freq, 0.4, [&] { c.change (eq); });
            logMessage (juce::String ("EQ click ratio, ") + c.name + ": " + juce::String (ratio, 2));
            expect (ratio < 1.6, juce::String ("EQ change is click-free: ") + c.name + ", ratio " + juce::String (ratio));
        }
    }

    // -----------------------------------------------------------------------------------------------
    void testFormats()
    {
        auto busy = [] (Equalizer& eq)
        {
            apply (eq, { 2, true, Type::lowShelf, 150.0f, 4.0f, 0.7f, 1 });
            apply (eq, { 4, true, Type::peak, 1000.0f, 6.0f, 1.0f, 1 });
            apply (eq, { 6, true, Type::notch, 3000.0f, 0.0f, 3.0f, 1 });
            apply (eq, { 8, true, Type::highCut, 12000.0f, 0.0f, 0.7f, 2 });
        };

        // Mono bus == left channel of the stereo run.
        {
            Equalizer stereo, mono;
            busy (stereo);
            busy (mono);
            stereo.prepareToPlay (kRate, 512);
            mono.prepareToPlay (kRate, 512);

            const auto x = makeNoise (4800, 0.3, 3);
            const auto ref = runMono (stereo, x, 480);

            juce::AudioBuffer<float> buffer (1, (int) x.size());
            buffer.copyFrom (0, 0, x.data(), (int) x.size());
            juce::MidiBuffer midi;
            mono.processBlock (buffer, midi);

            float worst = 0.0f;

            for (int i = 0; i < (int) x.size(); ++i)
                worst = juce::jmax (worst, std::abs (buffer.getSample (0, i) - ref[(size_t) i]));

            expectEquals (worst, 0.0f);
        }

        // The output does not depend on the block size (including sizes that straddle the 32-sample update grid).
        {
            const auto x = makeNoise (9600, 0.3, 4);
            std::vector<float> reference;

            for (int block : { 480, 1, 7, 33, 64, 1000, 4097 })
            {
                Equalizer eq;
                busy (eq);
                eq.prepareToPlay (kRate, 1024);
                auto y = runMono (eq, x, block);

                if (reference.empty())
                    reference = y;
                else
                    expect (y == reference, "EQ output is independent of the block size (" + juce::String (block) + ")");
            }
        }

        // Identical channels share one filter; the moment they differ the right one carries on from the same state.
        {
            const int n = 24000;
            auto left = makeNoise (n, 0.3, 21);
            auto right = left;

            for (int i = n / 2; i < n; ++i)                                     // second half: a different right channel
                right[(size_t) i] = 0.5f * left[(size_t) i] + 0.1f * std::sin (0.05f * (float) i);

            Equalizer together;
            busy (together);
            together.prepareToPlay (kRate, 512);
            auto l = left, r = right;
            runProcessor (together, l.data(), r.data(), n, 480);

            Equalizer leftOnly, rightOnly;
            busy (leftOnly);
            busy (rightOnly);
            leftOnly.prepareToPlay (kRate, 512);
            rightOnly.prepareToPlay (kRate, 512);
            const auto refL = runMono (leftOnly, left, 480);
            const auto refR = runMono (rightOnly, right, 480);

            expect (l == refL, "left channel is the same whether or not the right one is shared");
            expect (r == refR, "right channel picks up exactly where the shared state left off");
        }

        // Sample rates 44.1 - 192 kHz.
        for (double sr : { 44100.0, 88200.0, 96000.0, 192000.0 })
        {
            Equalizer eq;
            eq.prepareToPlay (sr, 512);
            const double at30 = measureGainDb (eq, sr, 30.0, 0.25, 1.0, 512);
            const double at1k = measureGainDb (eq, sr, 1000.0, 0.25, 1.0, 512);
            expect (at30 <= -15.0 && std::abs (at1k) < 0.2, "EQ at " + juce::String (sr) + " Hz: " + juce::String (at30) + " / " + juce::String (at1k));
        }
    }

    // -----------------------------------------------------------------------------------------------
    void testAnalyzerFeed()
    {
        // Off: nothing is written, whatever runs.
        {
            Equalizer eq;
            eq.prepareToPlay (kRate, 512);
            const auto x = makeNoise (48000, 0.3, 5);
            runMono (eq, x, 480);
            expectEquals ((int) eq.getAnalyzerFifo().getNumWritten(), 0);
            expectEquals (eq.getAnalyzerFifo().getNumReady(), 0);
            expect (! eq.analyzerActive.load(), "off by default");
        }

        // On: pre and post arrive, in order, as the mono sum.
        {
            Equalizer eq;
            apply (eq, { 4, true, Type::peak, 1000.0f, 9.0f, 1.0f, 1 });
            eq.prepareToPlay (kRate, 512);
            eq.analyzerActive = true;

            const int n = 3000;                                    // more than one internal chunk
            auto l = makeNoise (n, 0.3, 8), r = makeNoise (n, 0.3, 9);
            const auto l0 = l, r0 = r;
            runProcessor (eq, l.data(), r.data(), n, 3000);

            std::vector<float> pre ((size_t) n), post ((size_t) n);
            const int got = eq.getAnalyzerFifo().read (pre.data(), post.data(), n);
            expectEquals (got, n);

            bool exact = true;

            for (int i = 0; i < n; ++i)
                exact = exact && pre[(size_t) i] == 0.5f * (l0[(size_t) i] + r0[(size_t) i]) && post[(size_t) i] == 0.5f * (l[(size_t) i] + r[(size_t) i]);

            expect (exact, "the FIFO carries the pre- and post-EQ mono sum");
            expect (post != pre, "and post differs from pre");
            expectEquals ((int) eq.getAnalyzerFifo().getNumWritten(), n);
            expectEquals ((int) eq.getAnalyzerFifo().getNumDropped(), 0);
        }

        // Nobody reads: the FIFO fills, the rest is dropped, the audio is not touched and nothing waits.
        {
            Equalizer on, off;
            apply (on, { 4, true, Type::peak, 1000.0f, 9.0f, 1.0f, 1 });
            apply (off, { 4, true, Type::peak, 1000.0f, 9.0f, 1.0f, 1 });
            on.prepareToPlay (kRate, 512);
            off.prepareToPlay (kRate, 512);
            on.analyzerActive = true;

            const auto x = makeNoise ((int) kRate * 4, 0.3, 12);       // 4 s = far more than the FIFO holds
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            const auto withAnalyzer = runMono (on, x, 256);
            const auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
            const auto without = runMono (off, x, 256);

            expect (withAnalyzer == without, "the analyzer does not change the audio");
            expect (on.getAnalyzerFifo().getNumDropped() > 0, "a full FIFO drops instead of waiting");
            expectEquals (on.getAnalyzerFifo().getNumReady(), dsp::AnalyzerFifo::kUsable);
            expect (on.getAnalyzerFifo().getNumWritten() + on.getAnalyzerFifo().getNumDropped() == (uint64_t) x.size(), "every frame is either written or counted as dropped");
            logMessage ("EQ analyzer: 4 s of audio through a FIFO nobody reads took " + juce::String (ms, 1) + " ms; dropped "
                        + juce::String ((int) on.getAnalyzerFifo().getNumDropped()) + " frames");
            expect (ms < 4000.0, "a full FIFO does not slow the audio thread down");

            // Reading catches up afterwards.
            std::vector<float> a (4096), b (4096);
            int total = 0, got;

            while ((got = on.getAnalyzerFifo().read (a.data(), b.data(), 4096)) > 0)
                total += got;

            expectEquals (total, dsp::AnalyzerFifo::kUsable);
        }
    }

    // -----------------------------------------------------------------------------------------------
    void testSolo()
    {
        auto makeEq = []
        {
            auto eq = std::make_unique<Equalizer>();
            setParam (eq->apvts, "b1On", 0.0f);
            apply (*eq, { 4, true, Type::peak, 1000.0f, 6.0f, 2.0f, 1 });
            apply (*eq, { 2, true, Type::lowShelf, 150.0f, 4.0f, 0.7f, 1 });
            eq->prepareToPlay (kRate, 512);
            return eq;
        };

        // Solo on the 1 kHz bell: the 1 kHz tone passes, far-away tones do not.
        {
            auto eq = makeEq();
            eq->setSoloBand (3);
            const double at1k = measureGainDb (*eq, kRate, 1000.0);
            eq->setSoloBand (3);
            const double at100 = measureGainDb (*eq, kRate, 100.0);
            eq->setSoloBand (3);
            const double at10k = measureGainDb (*eq, kRate, 10000.0);
            logMessage ("EQ solo on the 1 kHz bell: 100 Hz " + juce::String (at100, 1) + " dB, 1 kHz " + juce::String (at1k, 1) + " dB, 10 kHz " + juce::String (at10k, 1) + " dB");
            expect (at100 < -25.0 && at10k < -25.0, "far-away tones are gone");
            expect (at1k > -1.0 && at1k < 8.0, "the band's own frequency stays (with the band's boost) (" + juce::String (at1k, 2) + ")");
        }

        // Solo on the low shelf plays what is below it.
        {
            auto eq = makeEq();
            eq->setSoloBand (1);
            const double at50 = measureGainDb (*eq, kRate, 50.0);
            eq->setSoloBand (1);
            const double at2k = measureGainDb (*eq, kRate, 2000.0);
            expect (at50 > 0.0 && at2k < -25.0, "a low shelf solo is a low-pass (" + juce::String (at50, 1) + " / " + juce::String (at2k, 1) + ")");
        }

        // Releasing it gives the EQ back, exactly.
        {
            auto eq = makeEq();
            auto reference = makeEq();
            const auto x = makeNoise (48000, 0.2, 33);

            eq->setSoloBand (3);
            runMono (*eq, x, 480);
            eq->setSoloBand (-1);
            runMono (*eq, x, 480);          // 1 s later the solo has faded out and gone

            const auto after = runMono (*eq, x, 480);
            runMono (*reference, x, 480);
            runMono (*reference, x, 480);
            const auto expected = runMono (*reference, x, 480);

            double worst = 0.0;

            for (size_t i = 0; i < after.size(); ++i)
                worst = juce::jmax (worst, (double) std::abs (after[i] - expected[i]));

            expect (worst < 1.0e-6, "after release the output is the plain EQ again (" + juce::String (worst) + ")");
        }

        // No click when it engages or releases.
        {
            for (bool engage : { true, false })
            {
                auto eq = makeEq();

                if (! engage)
                    eq->setSoloBand (3);

                const double ratio = clickRatio (*eq, 1000.0, 0.4, [&] { eq->setSoloBand (engage ? 3 : -1); });
                logMessage (juce::String ("EQ solo ") + (engage ? "engage" : "release") + " click ratio " + juce::String (ratio, 2));
                expect (ratio < 1.6, juce::String ("solo ") + (engage ? "engage" : "release") + " is click-free (" + juce::String (ratio) + ")");
            }
        }

        // Soloing an off band does nothing.
        {
            auto eq = makeEq();
            eq->setSoloBand (6);
            expect (std::abs (measureGainDb (*eq, kRate, 100.0) - eq->getResponseDb (100.0, kRate)) < 0.1, "solo on a band that is off is ignored");
        }
    }

    // -----------------------------------------------------------------------------------------------
    void testNoAllocation()
    {
        Equalizer eq;
        eq.prepareToPlay (kRate, 1024);
        eq.analyzerActive = true;
        eq.setSoloBand (3);

        auto l = makeNoise (1024, 0.3, 40), r = makeNoise (1024, 0.3, 41);
        std::mt19937 rng (3);
        std::uniform_real_distribution<float> u (0.0f, 1.0f);

        // Warm-up outside the guard (first block snaps the state).
        runProcessor (eq, l.data(), r.data(), 1024, 512);

        int allocations = 0;

        for (int pass = 0; pass < 200; ++pass)
        {
            // Move things around: bands on and off, types, values, solo.
            const int band = (int) (u (rng) * 8.0f);
            setParam (eq.apvts, Equalizer::bandParamId (band, "On"), u (rng) > 0.3f ? 1.0f : 0.0f);
            setParam (eq.apvts, Equalizer::bandParamId (band, "Type"), std::floor (u (rng) * 7.99f));
            setParam (eq.apvts, Equalizer::bandParamId (band, "Freq"), 20.0f * std::pow (1000.0f, u (rng)));
            setParam (eq.apvts, Equalizer::bandParamId (band, "Gain"), -24.0f + 48.0f * u (rng));
            setParam (eq.apvts, Equalizer::bandParamId (band, "Slope"), std::floor (u (rng) * 4.99f));
            eq.setSoloBand (pass % 5 == 0 ? -1 : (int) (u (rng) * 8.0f));

            auto lc = l, rc = r;

            if (pass % 3 == 0)
                rc = lc;                                        // mono blocks as well

            const ScopedNoAllocation guard;
            runProcessor (eq, lc.data(), rc.data(), 700, 350);      // (vector copies above are outside the guard scope)
            allocations += guard.allocations();
        }

        expectEquals (allocations, 0, "no operator new on the audio thread (analyzer on, bands moving, solo, mono and stereo)");
    }

    // -----------------------------------------------------------------------------------------------
    void testRandomAutomation()
    {
        for (double sr : { 44100.0, 48000.0, 96000.0 })
        {
            Equalizer eq;
            eq.prepareToPlay (sr, 1024);
            eq.analyzerActive = true;

            std::mt19937 rng (99);
            std::uniform_real_distribution<float> u (0.0f, 1.0f);
            std::uniform_int_distribution<int> sizes (1, 777);

            bool finite = true;
            float worst = 0.0f;

            for (int pass = 0; pass < 400; ++pass)
            {
                // A handful of parameter moves, as fast as automation could do them.
                for (int k = 0; k < 6; ++k)
                {
                    const int band = (int) (u (rng) * 8.0f);
                    const auto id = [&] (const char* what) { return Equalizer::bandParamId (band, what); };

                    switch ((int) (u (rng) * 6.0f))
                    {
                        case 0: setParam (eq.apvts, id ("On"), u (rng) > 0.35f ? 1.0f : 0.0f); break;
                        case 1: setParam (eq.apvts, id ("Type"), std::floor (u (rng) * 7.99f)); break;
                        case 2: setParam (eq.apvts, id ("Freq"), 20.0f * std::pow (1000.0f, u (rng))); break;
                        case 3: setParam (eq.apvts, id ("Gain"), -24.0f + 48.0f * u (rng)); break;
                        case 4: setParam (eq.apvts, id ("Q"), 0.1f * std::pow (180.0f, u (rng))); break;
                        default: setParam (eq.apvts, id ("Slope"), std::floor (u (rng) * 4.99f)); break;
                    }
                }

                if (pass % 7 == 0)
                    eq.setSoloBand ((int) (u (rng) * 9.0f) - 1);

                const int n = sizes (rng);
                std::vector<float> l ((size_t) n), r ((size_t) n);

                // silence, noise, +-1 sine, DC, mono / stereo
                for (int i = 0; i < n; ++i)
                {
                    switch (pass % 5)
                    {
                        case 0:  l[(size_t) i] = r[(size_t) i] = 0.0f; break;
                        case 1:  l[(size_t) i] = 2.0f * u (rng) - 1.0f; r[(size_t) i] = -l[(size_t) i]; break;
                        case 2:  l[(size_t) i] = r[(size_t) i] = std::sin (0.05f * (float) i); break;
                        case 3:  l[(size_t) i] = r[(size_t) i] = 0.9f; break;
                        default: l[(size_t) i] = r[(size_t) i] = 2.0f * u (rng) - 1.0f; break;      // mono noise
                    }
                }

                runProcessor (eq, l.data(), r.data(), n, 0);
                finite = finite && allFinite (l.data(), n) && allFinite (r.data(), n);
                worst = juce::jmax (worst, peakAbs (l.data(), n), peakAbs (r.data(), n));
            }

            expect (finite, "random automation at " + juce::String (sr) + " Hz: finite output");
            logMessage ("EQ random automation at " + juce::String (sr) + " Hz: largest output " + juce::String (worst, 1));
            expect (worst < 1.0e6f, "bounded output (" + juce::String (worst) + ")");

            // Everything neutral again: the filters must settle to silence, not ring or blow up.
            for (int b = 0; b < 8; ++b)
                setParam (eq.apvts, Equalizer::bandParamId (b, "On"), 0.0f);

            eq.setSoloBand (-1);
            std::vector<float> l ((size_t) sr), r ((size_t) sr);
            runProcessor (eq, l.data(), r.data(), (int) sr, 480);
            expect (peakAbs (l.data() + (int) sr - 480, 480) == 0.0f, "silence in, silence out once everything is off again");
        }
    }

    // -----------------------------------------------------------------------------------------------
    void testCpu()
    {
        auto busyStereo = [] (Equalizer& eq)
        {
            apply (eq, { 1, true, Type::lowCut, 80.0f, 0.0f, 0.70710678f, 3 });            // 48 dB/oct = four sections
            apply (eq, { 2, true, Type::lowShelf, 150.0f, 3.0f, 0.7f, 1 });
            apply (eq, { 3, true, Type::peak, 300.0f, -3.0f, 1.5f, 1 });
            apply (eq, { 4, true, Type::peak, 1000.0f, 2.0f, 1.0f, 1 });
            apply (eq, { 5, true, Type::peak, 3000.0f, 4.0f, 1.2f, 1 });
            apply (eq, { 6, true, Type::notch, 6000.0f, 0.0f, 8.0f, 1 });
            apply (eq, { 7, true, Type::highShelf, 10000.0f, 2.0f, 0.7f, 1 });
            apply (eq, { 8, true, Type::highCut, 16000.0f, 0.0f, 0.70710678f, 2 });
            eq.prepareToPlay (kRate, 256);
        };

        const int seconds = 10;
        auto l = makeNoise ((int) kRate * seconds, 0.2, 61);
        auto r = makeNoise ((int) kRate * seconds, 0.2, 62);

        double stereoMs = 0.0, monoMs = 0.0, movingMs = 0.0, analyzerMs = 0.0;

        {
            Equalizer eq;
            busyStereo (eq);
            auto a = l, b = r;
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            runProcessor (eq, a.data(), b.data(), (int) a.size(), 256);
            stereoMs = juce::Time::getMillisecondCounterHiRes() - t0;
        }

        {
            Equalizer eq;
            busyStereo (eq);
            auto a = l, b = l;
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            runProcessor (eq, a.data(), b.data(), (int) a.size(), 256);
            monoMs = juce::Time::getMillisecondCounterHiRes() - t0;
        }

        {
            Equalizer eq;
            busyStereo (eq);
            eq.analyzerActive = true;
            auto a = l, b = r;
            std::vector<float> pre (4096), post (4096);
            const auto t0 = juce::Time::getMillisecondCounterHiRes();

            for (int offset = 0; offset < (int) a.size(); offset += 256)
            {
                float* channels[2] { a.data() + offset, b.data() + offset };
                juce::AudioBuffer<float> buffer (channels, 2, 256);
                juce::MidiBuffer midi;
                eq.processBlock (buffer, midi);

                if (offset % 2048 == 0)                     // the UI drains it now and then
                    while (eq.getAnalyzerFifo().read (pre.data(), post.data(), 4096) > 0) {}
            }

            analyzerMs = juce::Time::getMillisecondCounterHiRes() - t0;
        }

        {
            // Worst case: every band moving all the time (nobody can automate faster than this).
            Equalizer eq;
            busyStereo (eq);
            std::mt19937 rng (5);
            std::uniform_real_distribution<float> u (0.0f, 1.0f);
            auto a = l, b = r;
            const auto t0 = juce::Time::getMillisecondCounterHiRes();

            for (int offset = 0; offset < (int) a.size(); offset += 256)
            {
                const int band = (offset / 256) % 8;
                setParam (eq.apvts, Equalizer::bandParamId (band, "Freq"), 20.0f * std::pow (1000.0f, u (rng)));
                setParam (eq.apvts, Equalizer::bandParamId (band, "Gain"), -12.0f + 24.0f * u (rng));

                float* channels[2] { a.data() + offset, b.data() + offset };
                juce::AudioBuffer<float> buffer (channels, 2, 256);
                juce::MidiBuffer midi;
                eq.processBlock (buffer, midi);
            }

            movingMs = juce::Time::getMillisecondCounterHiRes() - t0;
        }

        const double denominator = 1000.0 * seconds / 100.0;        // ms of audio / 100 = ms per percent
        logMessage ("EQ CPU, 8 bands (48 dB/oct low cut, shelves, 3 bells, notch, high cut at 12 dB/oct), 48 kHz, 256-frame blocks, % of one core: stereo "
                    + juce::String (stereoMs / denominator, 3) + ", mono mic " + juce::String (monoMs / denominator, 3) + ", with analyzer "
                    + juce::String (analyzerMs / denominator, 3) + ", every band moving " + juce::String (movingMs / denominator, 3));

       #if ! JUCE_DEBUG
        expect (stereoMs / denominator < 1.0, "8 bands stereo stay under 1 % of a core");
        expect (movingMs / denominator < 1.0, "even with every band moving all the time");
       #endif
    }

    // -----------------------------------------------------------------------------------------------
    void testTypedValues()
    {
        auto parse = [] (EqValueKind kind, const char* text, float& out) { return parseEqValue (kind, juce::String::fromUTF8 (text), out); };
        float v = 0.0f;

        expect (parse (EqValueKind::frequency, "2.4k", v) && std::abs (v - 2400.0f) < 0.01f, "2.4k");
        expect (parse (EqValueKind::frequency, "2.4 kHz", v) && std::abs (v - 2400.0f) < 0.01f, "2.4 kHz");
        expect (parse (EqValueKind::frequency, "250", v) && v == 250.0f, "250");
        expect (parse (EqValueKind::frequency, " 80 hz ", v) && v == 80.0f, "80 hz");
        expect (parse (EqValueKind::frequency, "1,5k", v) && std::abs (v - 1500.0f) < 0.01f, "a decimal comma");
        expect (parse (EqValueKind::gain, "-3.5", v) && v == -3.5f, "-3.5");
        expect (parse (EqValueKind::gain, "+3 dB", v) && v == 3.0f, "+3 dB");
        expect (parse (EqValueKind::gain, "\xe2\x88\x92" "4.5", v) && v == -4.5f, "a typographic minus");
        expect (parse (EqValueKind::q, "Q 0.7", v) && std::abs (v - 0.7f) < 1.0e-6f, "Q 0.7");
        expect (parse (EqValueKind::q, "12", v) && v == 12.0f, "12");
        expect (! parse (EqValueKind::frequency, "", v), "empty");
        expect (! parse (EqValueKind::gain, "abc", v), "no number");
        expect (! parse (EqValueKind::gain, "dB", v), "a unit alone");
    }

    // -----------------------------------------------------------------------------------------------
    void testSpectrumAnalyzer()
    {
        dsp::SpectrumAnalyzer analyzer;
        analyzer.prepare (kRate);
        analyzer.setTilt (0.0f);
        expectEquals (analyzer.getFftSize(), 4096);
        expectEquals (analyzer.getHopSize(), 1024);

        // A sine: the loudest column is at its frequency, a couple of octaves away it is far below.
        const auto sine = makeSine (48000, kRate, 1000.0, 0.5);
        analyzer.push (sine.data(), (int) sine.size());
        expect (analyzer.getNumFrames() > 40, "frames were analysed");

        const int columns = 900;
        std::vector<float> db ((size_t) columns);
        analyzer.getColumns (db.data(), columns, 20.0f, 20000.0f);

        auto columnFor = [&] (double f) { return (int) (std::log10 (f / 20.0) / 3.0 * columns); };
        int loudest = 0;

        for (int i = 0; i < columns; ++i)
            if (db[(size_t) i] > db[(size_t) loudest])
                loudest = i;

        expect (std::abs (loudest - columnFor (1000.0)) <= 12, "the peak sits at 1 kHz (column " + juce::String (loudest) + " vs " + juce::String (columnFor (1000.0)) + ")");
        logMessage ("spectrum analyzer: a -6 dBFS sine at 1 kHz reads " + juce::String (db[(size_t) loudest], 1) + " dB after the 1/6-octave smoothing");
        expect (db[(size_t) loudest] > -20.0f && db[(size_t) loudest] < -8.0f, "level is about a -6 dB sine, softened by the smoothing (" + juce::String (db[(size_t) loudest], 1) + ")");
        expect (db[(size_t) columnFor (100.0)] < db[(size_t) loudest] - 40.0f && db[(size_t) columnFor (10000.0)] < db[(size_t) loudest] - 40.0f, "far from the tone it is quiet");

        for (auto v : db)
            expect (std::isfinite (v) && v >= dsp::SpectrumAnalyzer::kFloorDb - 1.0f, "finite, above the floor");

        // Ballistics: after the tone stops the level falls at about 20 dB/s.
        const std::vector<float> silence (4096 * 3, 0.0f);
        const float before = db[(size_t) loudest];
        analyzer.push (silence.data(), (int) silence.size());      // 0.256 s
        analyzer.getColumns (db.data(), columns, 20.0f, 20000.0f);
        const float fell = before - db[(size_t) loudest];
        const double seconds = 3.0 * 4096.0 / kRate;
        logMessage ("spectrum analyzer: fell " + juce::String (fell, 2) + " dB in " + juce::String (seconds, 3) + " s = " + juce::String (fell / seconds, 1) + " dB/s");
        expect (fell > 0.6f * 20.0f * (float) seconds && fell < 1.5f * 20.0f * (float) seconds + 3.0f, "falls at about 20 dB/s");

        // ... and rises at once.
        analyzer.push (sine.data(), 4096 + 1024);
        analyzer.getColumns (db.data(), columns, 20.0f, 20000.0f);
        expect (db[(size_t) loudest] > before - 1.0f, "rise is immediate");

        // White noise reads flat (after smoothing) across the audio band.
        {
            dsp::SpectrumAnalyzer noise;
            noise.prepare (kRate);
            noise.setTilt (0.0f);
            const auto x = makeNoise (48000 * 2, 0.3, 77);
            noise.push (x.data(), (int) x.size());
            std::vector<float> n ((size_t) columns);
            noise.getColumns (n.data(), columns, 20.0f, 20000.0f);

            double lo = 1e9, hi = -1e9;

            for (int i = columnFor (300.0); i < columnFor (16000.0); ++i)
            {
                lo = juce::jmin (lo, (double) n[(size_t) i]);
                hi = juce::jmax (hi, (double) n[(size_t) i]);
            }

            logMessage ("spectrum analyzer: white noise, spread across 300 Hz - 16 kHz " + juce::String (hi - lo, 1) + " dB");
            expect (hi - lo < 14.0, "white noise is flat to within 14 dB (peak per pixel of a smoothed, peak-following spectrum): " + juce::String (hi - lo, 1));
            noise.setTilt (4.5f);
            noise.getColumns (n.data(), columns, 20.0f, 20000.0f);
            expect (n[(size_t) columnFor (10000.0)] - n[(size_t) columnFor (100.0)] > 20.0f, "the display tilt lifts the highs");
        }

        // Every width works, narrow columns interpolate, and asking again allocates nothing.
        for (int width : { 1, 7, 240, 1800 })
        {
            std::vector<float> w ((size_t) width);
            analyzer.getColumns (w.data(), width, 20.0f, 20000.0f);

            for (auto v : w)
                expect (std::isfinite (v));
        }

        {
            std::vector<float> w (900);
            analyzer.getColumns (w.data(), 900, 20.0f, 20000.0f);      // (the map for 900 exists now)
            int allocations = 0;

            {
                const ScopedNoAllocation guard;
                analyzer.push (sine.data(), 8192);
                analyzer.getColumns (w.data(), 900, 20.0f, 20000.0f);
                allocations = guard.allocations();
            }

            expectEquals (allocations, 0, "analysing and reading the columns allocates nothing");
        }

        // Other sample rates pick a larger frame.
        analyzer.prepare (96000.0);
        expectEquals (analyzer.getFftSize(), 8192);
    }
};

static EqTests eqTests;
}
