// Unit tests for the built-in effects and the shared DSP blocks: EQ, compressor, gate, de-esser, limiters, noise removal.
#include "Juce.h"

#include "TestHelpers.h"
#include "engine/builtin/BuiltinFx.h"
#include "engine/builtin/Compressor.h"
#include "engine/builtin/DeEsser.h"
#include "engine/builtin/Equalizer.h"
#include "engine/builtin/Gate.h"
#include "engine/builtin/Limiter.h"
#include "engine/builtin/NoiseRemoval.h"
#include "engine/denoise/RnnoiseEngine.h"
#include "engine/dsp/LookaheadLimiter.h"
#include "engine/dsp/Resampler.h"
#include "engine/dsp/Sanitise.h"

#include <cmath>
#include <functional>

namespace hr
{
using namespace test;

namespace
{
    void setParam (juce::AudioProcessorValueTreeState& apvts, const char* id, float plainValue)
    {
        auto* p = apvts.getParameter (id);
        jassert (p != nullptr);
        p->setValueNotifyingHost (p->convertTo0to1 (plainValue));
    }

    // These tests are about the RNNoise path, which is the "Light" mode (Fast and Best are in NoiseRemovalTests).
    void useLight (NoiseRemoval& nr)
    {
        setParam (nr.apvts, "mode", (float) (int) NoiseRemoval::Mode::light);
    }

    // Steady-state gain in dB of a processor at one frequency: sine in, tone level out minus tone level in.
    double measureGainDb (juce::AudioProcessor& p, double sampleRate, double frequency, double amplitude = 0.25,
                          double seconds = 1.0, int blockSize = 480)
    {
        const int n = (int) (sampleRate * seconds);
        const auto x = makeSine (n, sampleRate, frequency, amplitude);
        const auto y = runMono (p, x, blockSize);
        const int start = n / 2;

        return toneDb (y.data() + start, n - start, sampleRate, frequency) - toneDb (x.data() + start, n - start, sampleRate, frequency);
    }

    double maxAbsDiff (const std::vector<float>& v, int from, int to)
    {
        double worst = 0.0;

        for (int i = juce::jmax (1, from); i < to; ++i)
            worst = juce::jmax (worst, (double) std::fabs (v[(size_t) i] - v[(size_t) i - 1]));

        return worst;
    }

    // Runs a sine through `p`, calls `change` (a parameter move) after 0.4 s and returns the largest sample-to-sample
    // step during the change relative to the largest step in the steady state before and after. A click shows up as a
    // ratio far above 1; a smooth glide stays near 1.
    double clickRatio (juce::AudioProcessor& p, double sampleRate, double frequency, double amplitude, const std::function<void()>& change)
    {
        constexpr int block = 256;
        const int n = ((int) (sampleRate * 1.4) / block) * block;
        const int changeAt = ((int) (sampleRate * 0.4) / block) * block;

        auto left = makeSine (n, sampleRate, frequency, amplitude);
        auto right = left;

        runProcessor (p, left.data(), right.data(), changeAt, block);
        change();
        runProcessor (p, left.data() + changeAt, right.data() + changeAt, n - changeAt, block);

        const int window = (int) (sampleRate * 0.1);
        const double before = maxAbsDiff (left, changeAt - window, changeAt);
        const double after = maxAbsDiff (left, n - window, n);
        const double during = maxAbsDiff (left, changeAt, n - window);

        return during / juce::jmax (1.0e-9, before, after);
    }

    // A voiced-speech stand-in that RNNoise accepts as voice: harmonics of a wobbling 120 Hz pitch between 700 Hz and
    // 5 kHz, shaped by a 2 Hz syllable envelope, plus a little noise.
    std::vector<float> makeVowels (int numSamples, double sampleRate, double syllableHz = 2.0, uint32_t noiseSeed = 5,
                                   double noiseAmp = 0.01)
    {
        std::vector<float> x ((size_t) numSamples);
        const auto noise = makeNoise (numSamples, noiseAmp, noiseSeed);
        double phase = 0.0;

        for (int i = 0; i < numSamples; ++i)
        {
            const double t = (double) i / sampleRate;
            const double envelope = syllableHz > 0.0 ? 0.5 - 0.5 * std::cos (2.0 * kPi * syllableHz * t) : 1.0;
            const double f0 = 120.0 + 10.0 * std::sin (2.0 * kPi * 3.0 * t);
            phase += 2.0 * kPi * f0 / sampleRate;

            double s = 0.0;

            for (int h = 1; h <= 60; ++h)
                if ((double) h * f0 > 700.0 && (double) h * f0 < 5000.0)
                    s += 0.1 * std::sin (phase * (double) h);

            x[(size_t) i] = (float) (0.15 * envelope * s) + noise[(size_t) i];
        }

        return x;
    }

    // Normalised cross-correlation of y with x delayed by `lag`, over [from, to).
    double correlationAtLag (const std::vector<float>& x, const std::vector<float>& y, int lag, int from, int to)
    {
        double c = 0.0, ex = 0.0, ey = 0.0;

        for (int i = from; i < to; ++i)
        {
            const double a = x[(size_t) (i - lag)], b = y[(size_t) i];
            c += a * b;
            ex += a * a;
            ey += b * b;
        }

        return c / std::sqrt (ex * ey + 1.0e-30);
    }

    struct BlockedRunResult
    {
        std::vector<float> l, r;
    };

    BlockedRunResult runStereo (juce::AudioProcessor& p, std::vector<float> l, std::vector<float> r, int blockSize)
    {
        runProcessor (p, l.data(), r.data(), (int) l.size(), blockSize);
        return { std::move (l), std::move (r) };
    }
}

class DspTests final : public juce::UnitTest
{
public:
    DspTests() : juce::UnitTest ("DSP", "headroom") {}

    void runTest() override
    {
        beginTest ("sanitise: non-finite -> 0, hard clip at +-4");
        testSanitise();

        beginTest ("EQ: high-pass 80 Hz / 12 dB");
        testEqHighPass();

        beginTest ("EQ: bell +6 dB at 1 kHz");
        testEqBell();

        beginTest ("EQ: getResponseDb matches the DSP");
        testEqResponse();

        beginTest ("EQ: parameter changes glide (no clicks)");
        testEqClicks();

        beginTest ("EQ: mono buses, block sizes, sample rates");
        testEqFormats();

        beginTest ("compressor: static curve and makeup");
        testCompressorCurve();

        beginTest ("compressor: knee, stereo link, attack, release, meter");
        testCompressorDynamics();

        beginTest ("gate");
        testGate();

        beginTest ("de-esser");
        testDeEsser();

        beginTest ("limiter effect: lookahead brickwall");
        testLimiterEffect();

        beginTest ("LookaheadLimiter across rates and block sizes");
        testLookaheadLimiter();

        beginTest ("resampler");
        testResampler();

        beginTest ("noise removal: attenuates noise");
        testNoiseAttenuation();

        beginTest ("noise removal: latency and wet/dry alignment");
        testNoiseLatency();

        beginTest ("noise removal: block sizes do not matter");
        testNoiseBlockSizes();

        beginTest ("noise removal: mono optimisation is exact");
        testNoiseMono();

        beginTest ("noise removal: sample rates");
        testNoiseRates();

        beginTest ("noise removal: voice gate");
        testNoiseGate();

        beginTest ("noise removal: CPU cost");
        testNoiseCpu();

        beginTest ("CPU cost of every built-in");
        testCpuCosts();

        beginTest ("every built-in survives silence, extremes, mono and odd blocks");
        testRobustness();
    }

private:
    // -----------------------------------------------------------------------------------------------
    void testSanitise()
    {
        const float inf = std::numeric_limits<float>::infinity();
        float v[] { std::numeric_limits<float>::quiet_NaN(), inf, -inf, 5.0f, -7.0f, 1.0f, -4.0f, 4.0f, 3.99f };
        const float expected[] { 0.0f, 0.0f, 0.0f, 4.0f, -4.0f, 1.0f, -4.0f, 4.0f, 3.99f };

        expect (dsp::sanitise (v, 9), "reports that it changed something");

        for (int i = 0; i < 9; ++i)
            expectEquals (v[i], expected[i]);

        float clean[] { 0.5f, -0.5f, 0.0f, 3.5f };
        expect (! dsp::sanitise (clean, 4), "clean audio is left alone and reported as such");
    }

    // -----------------------------------------------------------------------------------------------
    void testEqHighPass()
    {
        Equalizer eq;
        eq.prepareToPlay (kRate, 512);

        const double at30 = measureGainDb (eq, kRate, 30.0);
        const double at1k = measureGainDb (eq, kRate, 1000.0);
        logMessage ("EQ HPF 80 Hz/12 dB: 30 Hz " + juce::String (at30, 2) + " dB, 1 kHz " + juce::String (at1k, 3) + " dB");
        expect (at30 <= -15.0, "30 Hz is attenuated by at least 15 dB (" + juce::String (at30) + ")");
        expect (std::abs (at1k) < 0.2, "1 kHz passes within 0.2 dB (" + juce::String (at1k) + ")");

        // 24 dB/oct: twice as steep.
        Equalizer steep;
        setParam (steep.apvts, "b1Slope", 2.0f);
        steep.prepareToPlay (kRate, 512);
        const double steep30 = measureGainDb (steep, kRate, 30.0);
        expect (steep30 <= 2.0 * at30 + 1.0, "24 dB/oct is about twice as steep (" + juce::String (steep30) + " vs " + juce::String (at30) + ")");
        expect (std::abs (measureGainDb (steep, kRate, 1000.0)) < 0.2);

        // Off: flat.
        Equalizer off;
        setParam (off.apvts, "b1On", 0.0f);
        off.prepareToPlay (kRate, 512);
        expect (std::abs (measureGainDb (off, kRate, 30.0)) < 0.05, "high-pass off is flat");
    }

    void testEqBell()
    {
        Equalizer eq;
        eq.prepareToPlay (kRate, 512);
        setParam (eq.apvts, "b4On", 1.0f);
        setParam (eq.apvts, "b4Gain", 6.0f);
        const double boost = measureGainDb (eq, kRate, 1000.0);
        logMessage ("EQ bell +6 dB at 1 kHz measures " + juce::String (boost, 3) + " dB");
        expect (std::abs (boost - 6.0) < 0.3, "bell +6 dB measures +6 +- 0.3 (" + juce::String (boost) + ")");

        setParam (eq.apvts, "b4Gain", -9.0f);
        const double cut = measureGainDb (eq, kRate, 1000.0);
        expect (std::abs (cut + 9.0) < 0.3, "bell -9 dB measures -9 +- 0.3 (" + juce::String (cut) + ")");

        // A narrow bell leaves the neighbours alone.
        setParam (eq.apvts, "b4Gain", 12.0f);
        setParam (eq.apvts, "b4Q", 8.0f);
        expect (std::abs (measureGainDb (eq, kRate, 3000.0)) < 0.5, "narrow bell does not touch 3 kHz");
    }

    void testEqResponse()
    {
        // Every band type at once, on top of each other in places. (The EQ tests in EqTests.cpp go deeper.)
        Equalizer eq;
        setParam (eq.apvts, "b1Freq", 100.0f);
        setParam (eq.apvts, "b1Slope", 2.0f);
        setParam (eq.apvts, "b2On", 1.0f);
        setParam (eq.apvts, "b2Freq", 150.0f);
        setParam (eq.apvts, "b2Gain", 4.0f);
        setParam (eq.apvts, "b3On", 1.0f);
        setParam (eq.apvts, "b3Freq", 300.0f);
        setParam (eq.apvts, "b3Gain", -3.0f);
        setParam (eq.apvts, "b3Q", 2.0f);
        setParam (eq.apvts, "b4On", 1.0f);
        setParam (eq.apvts, "b4Gain", 5.0f);
        setParam (eq.apvts, "b4Q", 1.5f);
        setParam (eq.apvts, "b5On", 1.0f);
        setParam (eq.apvts, "b5Gain", 3.0f);
        setParam (eq.apvts, "b5Q", 0.7f);
        setParam (eq.apvts, "b6On", 1.0f);
        setParam (eq.apvts, "b6Type", 5.0f);        // notch
        setParam (eq.apvts, "b6Freq", 5300.0f);
        setParam (eq.apvts, "b6Q", 4.0f);
        setParam (eq.apvts, "b7On", 1.0f);
        setParam (eq.apvts, "b7Type", 2.0f);        // high shelf
        setParam (eq.apvts, "b7Freq", 8000.0f);
        setParam (eq.apvts, "b7Gain", -4.0f);
        setParam (eq.apvts, "b7Q", 0.7071f);
        setParam (eq.apvts, "b8On", 1.0f);
        setParam (eq.apvts, "b8Type", 4.0f);        // high cut
        setParam (eq.apvts, "b8Freq", 12000.0f);

        double worstMeasured = 0.0;

        for (double sr : { 44100.0, 48000.0, 96000.0 })
        {
            eq.prepareToPlay (sr, 512);

            for (double f : { 50.0, 100.0, 200.0, 400.0, 800.0, 1500.0, 3000.0, 6000.0, 10000.0, 16000.0 })
            {
                const double shown = eq.getResponseDb (f, sr);
                const double measured = measureGainDb (eq, sr, f);
                worstMeasured = juce::jmax (worstMeasured, std::abs (shown - measured));
            }
        }

        logMessage ("EQ response vs measured DSP: worst " + juce::String (worstMeasured, 3) + " dB");
        expect (worstMeasured < 0.1, "getResponseDb matches the measured DSP within 0.1 dB (" + juce::String (worstMeasured) + ")");

        // Frequencies above the limit clamp instead of asserting.
        expect (std::isfinite (eq.getResponseDb (24000.0, 48000.0)));
    }

    void testEqClicks()
    {
        struct Case
        {
            const char* name;
            double freq;
            std::function<void (Equalizer&)> setup;     // state before the change (applied before the snap)
            std::function<void (Equalizer&)> change;    // the parameter move under test
        };

        auto none = [] (Equalizer&) {};

        const Case cases[] {
            { "bell gain 0 -> +12 dB", 1000.0, none, [] (Equalizer& e) { setParam (e.apvts, "b4On", 1.0f); setParam (e.apvts, "b4Gain", 12.0f); } },
            { "bell gain 0 -> -18 dB", 1000.0, none, [] (Equalizer& e) { setParam (e.apvts, "b4On", 1.0f); setParam (e.apvts, "b4Gain", -18.0f); } },
            { "bell frequency sweep", 1000.0, [] (Equalizer& e) { setParam (e.apvts, "b4On", 1.0f); setParam (e.apvts, "b4Gain", 12.0f); },
                                               [] (Equalizer& e) { setParam (e.apvts, "b4Freq", 200.0f); } },
            { "high-pass 80 -> 400 Hz", 200.0, none, [] (Equalizer& e) { setParam (e.apvts, "b1Freq", 400.0f); } },
            { "high-pass off", 200.0, [] (Equalizer& e) { setParam (e.apvts, "b1Freq", 400.0f); },
                                      [] (Equalizer& e) { setParam (e.apvts, "b1On", 0.0f); } },
            { "high-pass on", 200.0, [] (Equalizer& e) { setParam (e.apvts, "b1Freq", 400.0f); setParam (e.apvts, "b1On", 0.0f); },
                                     [] (Equalizer& e) { setParam (e.apvts, "b1On", 1.0f); } },
            { "high-pass 12 -> 24 dB/oct", 200.0, [] (Equalizer& e) { setParam (e.apvts, "b1Freq", 400.0f); },
                                                  [] (Equalizer& e) { setParam (e.apvts, "b1Slope", 2.0f); } },
            { "low-pass on at 2 kHz", 5000.0, [] (Equalizer& e) { setParam (e.apvts, "b8Type", 4.0f); setParam (e.apvts, "b8Freq", 2000.0f); },
                                              [] (Equalizer& e) { setParam (e.apvts, "b8On", 1.0f); } },
            { "low shelf 0 -> +18 dB", 100.0, none, [] (Equalizer& e) { setParam (e.apvts, "b2On", 1.0f); setParam (e.apvts, "b2Gain", 18.0f); } },
        };

        for (const auto& c : cases)
        {
            Equalizer eq;
            c.setup (eq);
            eq.prepareToPlay (kRate, 512);      // snaps to the starting values

            const double ratio = clickRatio (eq, kRate, c.freq, 0.4, [&] { c.change (eq); });
            logMessage (juce::String ("EQ click ratio, ") + c.name + ": " + juce::String (ratio, 2));
            expect (ratio < 1.6, juce::String ("EQ parameter change is click-free: ") + c.name + ", ratio " + juce::String (ratio));
        }
    }

    void testEqFormats()
    {
        // Mono bus == left channel of the stereo run.
        {
            Equalizer stereo, mono;
            setParam (stereo.apvts, "b4On", 1.0f);
            setParam (stereo.apvts, "b4Gain", 6.0f);
            setParam (mono.apvts, "b4On", 1.0f);
            setParam (mono.apvts, "b4Gain", 6.0f);
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

        // Block size does not change the result.
        {
            const auto x = makeNoise (9600, 0.3, 4);
            std::vector<float> reference;

            for (int block : { 480, 1, 7, 64, 1024 })
            {
                Equalizer eq;
                setParam (eq.apvts, "b4On", 1.0f);
                setParam (eq.apvts, "b4Gain", 6.0f);
                eq.prepareToPlay (kRate, 1024);
                auto y = runMono (eq, x, block);

                if (reference.empty())
                    reference = y;
                else
                    expect (y == reference, "EQ output is independent of the block size (" + juce::String (block) + ")");
            }
        }

        // Sample rates 44.1 - 96 kHz.
        for (double sr : { 44100.0, 88200.0, 96000.0 })
        {
            Equalizer eq;
            eq.prepareToPlay (sr, 512);
            const double at30 = measureGainDb (eq, sr, 30.0, 0.25, 1.0, 512);
            const double at1k = measureGainDb (eq, sr, 1000.0, 0.25, 1.0, 512);
            expect (at30 <= -15.0 && std::abs (at1k) < 0.2, "EQ at " + juce::String (sr) + " Hz: " + juce::String (at30) + " / " + juce::String (at1k));
        }
    }

    // -----------------------------------------------------------------------------------------------
    void testCompressorCurve()
    {
        Compressor comp;
        comp.prepareToPlay (kRate, 512);
        setParam (comp.apvts, "threshold", -20.0f);
        setParam (comp.apvts, "ratio", 4.0f);
        setParam (comp.apvts, "attack", 10.0f);
        setParam (comp.apvts, "release", 120.0f);
        setParam (comp.apvts, "knee", 0.0f);
        setParam (comp.apvts, "makeup", 0.0f);

        // -10 dBFS sine: 10 dB over, ratio 4 -> 7.5 dB of reduction -> -17.5 dBFS.
        const int n = (int) kRate;
        const auto x = makeSine (n, kRate, 1000.0, dbToLinear (-10.0f));
        const auto y = runMono (comp, x, 256);
        const double level = toneDb (y.data() + n / 2, n / 2, kRate, 1000.0);
        logMessage ("Compressor: -10 dBFS in, threshold -20, 4:1, hard knee -> " + juce::String (level, 2) + " dBFS out; meter "
                    + juce::String (comp.getMeterValue(), 2) + " dB");
        expect (std::abs (level + 17.5) < 0.5, "output about -17.5 dBFS (" + juce::String (level) + ")");
        expect (std::abs (comp.getMeterValue() + 7.5) < 0.5, "meter shows the gain reduction (" + juce::String (comp.getMeterValue()) + ")");

        // Makeup is added on top.
        setParam (comp.apvts, "makeup", 4.0f);
        const auto y2 = runMono (comp, x, 256);
        expect (std::abs (toneDb (y2.data() + n / 2, n / 2, kRate, 1000.0) + 13.5) < 0.5, "plus 4 dB makeup");

        // Below the threshold nothing happens (makeup 0).
        setParam (comp.apvts, "makeup", 0.0f);
        Compressor quiet;
        quiet.prepareToPlay (kRate, 512);
        setParam (quiet.apvts, "makeup", 0.0f);
        setParam (quiet.apvts, "knee", 0.0f);
        expect (std::abs (measureGainDb (quiet, kRate, 1000.0, dbToLinear (-30.0f))) < 0.05, "below threshold: unity");
        expectEquals (quiet.getMeterValue(), 0.0f);

        // Ratio 1:1 is transparent, whatever the level.
        Compressor unity;
        unity.prepareToPlay (kRate, 512);
        setParam (unity.apvts, "ratio", 1.0f);
        setParam (unity.apvts, "makeup", 0.0f);
        expect (std::abs (measureGainDb (unity, kRate, 1000.0, 0.9)) < 0.05, "ratio 1:1 is transparent");

        // Makeup on its own is a plain gain.
        Compressor gainOnly;
        gainOnly.prepareToPlay (kRate, 512);
        setParam (gainOnly.apvts, "ratio", 1.0f);
        setParam (gainOnly.apvts, "makeup", 6.0f);
        expect (std::abs (measureGainDb (gainOnly, kRate, 1000.0, 0.1) - 6.0) < 0.05, "+6 dB makeup");

        // Silence in, silence out, exactly.
        Compressor silent;
        silent.prepareToPlay (kRate, 512);
        const auto s = runMono (silent, std::vector<float> (4800, 0.0f), 300);
        expectEquals (peakAbs (s.data(), (int) s.size()), 0.0f);
    }

    void testCompressorDynamics()
    {
        // Soft knee: exactly at the threshold the reduction is (1 - 1/R) * W / 8 = 0.5625 dB for R 4, W 6.
        {
            Compressor comp;
            comp.prepareToPlay (kRate, 512);
            setParam (comp.apvts, "threshold", -20.0f);
            setParam (comp.apvts, "ratio", 4.0f);
            setParam (comp.apvts, "knee", 6.0f);
            setParam (comp.apvts, "makeup", 0.0f);
            const double gain = measureGainDb (comp, kRate, 1000.0, dbToLinear (-20.0f));
            expect (std::abs (gain + 0.5625) < 0.15, "soft knee at the threshold (" + juce::String (gain) + ")");
        }

        // Stereo link: a loud left channel turns the quiet right channel down by the same amount.
        {
            Compressor comp;
            comp.prepareToPlay (kRate, 512);
            setParam (comp.apvts, "threshold", -20.0f);
            setParam (comp.apvts, "ratio", 4.0f);
            setParam (comp.apvts, "knee", 0.0f);
            setParam (comp.apvts, "makeup", 0.0f);

            const int n = (int) kRate;
            auto result = runStereo (comp, makeSine (n, kRate, 1000.0, dbToLinear (-10.0f)), makeSine (n, kRate, 1000.0, dbToLinear (-40.0f)), 256);
            const double right = toneDb (result.r.data() + n / 2, n / 2, kRate, 1000.0);
            expect (std::abs (right - (-40.0 - 7.5)) < 0.6, "right channel follows the left channel's reduction (" + juce::String (right) + ")");
        }

        // Attack and release times (10 ms / 120 ms): watch the meter block by block (1 ms blocks).
        {
            Compressor comp;
            comp.prepareToPlay (kRate, 512);
            setParam (comp.apvts, "threshold", -20.0f);
            setParam (comp.apvts, "ratio", 4.0f);
            setParam (comp.apvts, "attack", 10.0f);
            setParam (comp.apvts, "release", 120.0f);
            setParam (comp.apvts, "knee", 0.0f);
            setParam (comp.apvts, "makeup", 0.0f);

            const auto loud = makeSine ((int) kRate, kRate, 1000.0, dbToLinear (-10.0f));
            const auto quiet = makeSine ((int) kRate, kRate, 1000.0, dbToLinear (-50.0f));
            std::vector<float> l, r;
            juce::MidiBuffer midi;
            std::vector<float> meter;

            auto feed = [&] (const std::vector<float>& signal, int ms)
            {
                for (int b = 0; b < ms; ++b)
                {
                    std::vector<float> a (signal.begin() + b * 48, signal.begin() + (b + 1) * 48), c = a;
                    float* ch[2] { a.data(), c.data() };
                    juce::AudioBuffer<float> buffer (ch, 2, 48);
                    comp.processBlock (buffer, midi);
                    meter.push_back (comp.getMeterValue());
                }
            };

            feed (quiet, 100);                                      // nothing happening
            const size_t onset = meter.size();
            feed (loud, 200);
            const size_t drop = meter.size();
            feed (quiet, 800);

            expect (meter[onset] > -3.0f, "reduction has hardly started 1 ms into the attack (" + juce::String (meter[onset]) + ")");
            expect (meter[onset + 60] < -7.0f, "and is (almost) complete after 60 ms (" + juce::String (meter[onset + 60]) + ")");
            expect (meter[drop + 30] < -4.5f && meter[drop + 30] > -7.5f, "release is slow: still reducing 30 ms after the loud part (" + juce::String (meter[drop + 30]) + ")");
            expect (meter[drop + 700] > -0.3f, "and finished well within a second (" + juce::String (meter[drop + 700]) + ")");

            bool monotonic = true;
            for (size_t i = drop + 1; i < meter.size(); ++i)
                monotonic = monotonic && meter[i] >= meter[i - 1] - 1.0e-4f;

            expect (monotonic, "the release never overshoots");
        }

        // No gain ripple at audio rate for a steady low tone (this is what the peak-hold stage is for).
        {
            Compressor comp;
            comp.prepareToPlay (kRate, 512);
            setParam (comp.apvts, "threshold", -20.0f);
            setParam (comp.apvts, "ratio", 4.0f);
            setParam (comp.apvts, "knee", 0.0f);
            setParam (comp.apvts, "makeup", 0.0f);

            const int n = (int) kRate * 2;
            const auto x = makeSine (n, kRate, 100.0, dbToLinear (-10.0f));
            const auto y = runMono (comp, x, 256);
            const double fundamental = toneDb (y.data() + n / 2, n / 2, kRate, 100.0);
            const double third = toneDb (y.data() + n / 2, n / 2, kRate, 300.0);
            expect (third - fundamental < -45.0, "100 Hz tone: third harmonic " + juce::String (third - fundamental, 1) + " dB below the fundamental");
        }
    }

    // -----------------------------------------------------------------------------------------------
    void testGate()
    {
        // Above the threshold: untouched.
        {
            Gate gate;
            gate.prepareToPlay (kRate, 512);
            expect (std::abs (measureGainDb (gate, kRate, 1000.0, dbToLinear (-30.0f))) < 0.05, "a -30 dBFS tone passes a -50 dB gate");
            expectEquals (gate.getMeterValue(), 0.0f);
        }

        // Below the threshold: turned down by Range (default 40 dB).
        {
            Gate gate;
            gate.prepareToPlay (kRate, 512);
            const int n = (int) kRate * 2;
            const auto x = makeNoise (n, 0.0006, 11);                       // about -70 dBFS
            const auto y = runMono (gate, x, 256);
            const double reduction = toDb (rms (y.data() + n / 2, n / 2)) - toDb (rms (x.data() + n / 2, n / 2));
            expect (std::abs (reduction + 40.0) < 1.0, "quiet noise is 40 dB down (" + juce::String (reduction) + ")");
            expect (std::abs (gate.getMeterValue() + 40.0f) < 0.5f, "meter shows the attenuation (" + juce::String (gate.getMeterValue()) + ")");
        }

        // Timing: tone, then silence. Open through the hold time, then fade out over the release time, monotonically.
        {
            Gate gate;
            gate.prepareToPlay (kRate, 512);
            setParam (gate.apvts, "hold", 100.0f);
            setParam (gate.apvts, "release", 150.0f);

            const auto tone = makeSine ((int) kRate, kRate, 1000.0, dbToLinear (-30.0f));
            const auto floorNoise = makeNoise ((int) kRate, 1.0e-5, 2);
            std::vector<float> meter;
            juce::MidiBuffer midi;

            auto feed = [&] (const std::vector<float>& signal, int ms)
            {
                for (int b = 0; b < ms; ++b)
                {
                    std::vector<float> a (signal.begin() + b * 48, signal.begin() + (b + 1) * 48), c = a;
                    float* ch[2] { a.data(), c.data() };
                    juce::AudioBuffer<float> buffer (ch, 2, 48);
                    gate.processBlock (buffer, midi);
                    meter.push_back (gate.getMeterValue());
                }
            };

            feed (tone, 300);
            const size_t stop = meter.size();
            feed (floorNoise, 600);

            expectEquals (meter[stop - 1], 0.0f);
            expectEquals (meter[stop + 100], 0.0f);         // detector decay (~22 ms) + hold (100 ms) is not over yet
            expect (meter[stop + 135] < -0.5f, "closing has started by ~135 ms (" + juce::String (meter[stop + 135]) + ")");
            expect (meter[stop + 200] < -10.0f && meter[stop + 200] > -35.0f, "and is half way at ~200 ms (" + juce::String (meter[stop + 200]) + ")");
            expect (meter[stop + 400] <= -39.9f, "closed after the release (" + juce::String (meter[stop + 400]) + ")");

            bool monotonic = true;
            for (size_t i = stop + 1; i < meter.size(); ++i)
                monotonic = monotonic && meter[i] <= meter[i - 1] + 1.0e-4f;

            expect (monotonic, "the gate closes without wobbling");
        }

        // Bursts: the gate opens within a millisecond or two and neither edge steps.
        {
            Gate gate;
            gate.prepareToPlay (kRate, 512);

            const int burst = (int) (kRate * 0.3);
            std::vector<float> x;

            for (int b = 0; b < 3; ++b)
            {
                const auto tone = makeSine (burst, kRate, 1000.0, dbToLinear (-30.0f));
                x.insert (x.end(), tone.begin(), tone.end());
                x.insert (x.end(), (size_t) burst, 0.0f);
            }

            const auto y = runMono (gate, x, 128);

            // Third burst: the gate has been closed for 0.3 s.
            const int third = 4 * burst;
            double peakEarly = 0.0;

            for (int i = third + (int) (kRate * 0.004); i < third + (int) (kRate * 0.006); ++i)
                peakEarly = juce::jmax (peakEarly, (double) std::fabs (y[(size_t) i]));

            expect (peakEarly > 0.9 * dbToLinear (-30.0f), "fully open within ~4 ms of the onset (" + juce::String (peakEarly) + ")");

            const double toneStep = 2.0 * kPi * 1000.0 / kRate * dbToLinear (-30.0f);
            expect (maxAbsDiff (y, 0, (int) y.size()) < 2.5 * toneStep, "no click on any edge (" + juce::String (maxAbsDiff (y, 0, (int) y.size()) / toneStep, 2) + " x the tone's own slope)");
        }

        // Range 0 dB: the gate does nothing at all.
        {
            Gate gate;
            gate.prepareToPlay (kRate, 512);
            setParam (gate.apvts, "range", 0.0f);
            const auto x = makeNoise (4800, 0.0005, 3);
            expect (runMono (gate, x, 100) == x, "range 0 dB is bit-transparent");
        }
    }

    // -----------------------------------------------------------------------------------------------
    void testDeEsser()
    {
        auto makeDeEsser = [] (float threshold, float amount)
        {
            auto d = std::make_unique<DeEsser>();
            setParam (d->apvts, "threshold", threshold);
            setParam (d->apvts, "amount", amount);
            d->prepareToPlay (kRate, 512);
            return d;
        };

        // Sibilance: a 6.5 kHz tone is turned down by close to the maximum (12 dB at Amount 100 %).
        {
            auto d = makeDeEsser (-40.0f, 100.0f);
            const double gain = measureGainDb (*d, kRate, 6500.0, dbToLinear (-20.0f));
            logMessage ("De-esser 6.5 kHz at -20 dBFS, amount 100 %: " + juce::String (gain, 2) + " dB, meter " + juce::String (d->getMeterValue(), 2));
            expect (gain < -10.0 && gain > -13.0, "sibilance is reduced by about 12 dB (" + juce::String (gain) + ")");
            expect (d->getMeterValue() < -10.0f, "meter shows the reduction (" + juce::String (d->getMeterValue()) + ")");
        }

        // Amount scales the maximum: 50 % -> 6 dB.
        {
            auto d = makeDeEsser (-40.0f, 50.0f);
            const double gain = measureGainDb (*d, kRate, 6500.0, dbToLinear (-20.0f));
            expect (std::abs (gain + 6.0) < 1.0, "amount 50 % is about 6 dB (" + juce::String (gain) + ")");
        }

        // Only the sibilance band: a low tone at the same level is not touched, and neither is a tone below the threshold.
        {
            auto d = makeDeEsser (-40.0f, 100.0f);
            expect (std::abs (measureGainDb (*d, kRate, 500.0, dbToLinear (-20.0f))) < 0.3, "500 Hz is left alone");

            auto quiet = makeDeEsser (-40.0f, 100.0f);
            expect (std::abs (measureGainDb (*quiet, kRate, 6500.0, dbToLinear (-60.0f))) < 0.3, "sibilance band below the threshold is left alone");
        }

        // The frequency knob moves the band: at 4 kHz a 6.5 kHz tone is much less affected.
        {
            auto d = makeDeEsser (-40.0f, 100.0f);
            setParam (d->apvts, "frequency", 3000.0f);
            d->prepareToPlay (kRate, 512);
            const double gain = measureGainDb (*d, kRate, 6500.0, dbToLinear (-20.0f));
            expect (gain > -8.0, "6.5 kHz with the band at 3 kHz is reduced less (" + juce::String (gain) + ")");
        }

        // Amount 0: bit-transparent. Transparent at rest in general: no reduction, no change.
        {
            auto d = makeDeEsser (-40.0f, 0.0f);
            const auto x = makeNoise (9600, 0.3, 21);
            expect (runMono (*d, x, 250) == x, "amount 0 % is bit-transparent");

            auto rest = makeDeEsser (0.0f, 100.0f);
            const auto y = runMono (*rest, x, 250);
            expect (y == x, "nothing above the threshold: exact identity");
        }
    }

    // -----------------------------------------------------------------------------------------------
    void testLimiterEffect()
    {
        for (double sr : { 44100.0, 48000.0, 96000.0 })
        {
            Limiter lim;
            lim.prepareToPlay (sr, 512);
            const int latency = lim.getLatencySamples();
            expectEquals (latency, (int) std::lround (0.0015 * sr));
            const float ceiling = dbToLinear (-1.0f);

            // Random peaks up to +-2.0 never exceed the ceiling.
            const int n = (int) sr * 2;
            auto x = makeNoise (n, 2.0, 31);
            x[1000] = 2.0f;
            x[1001] = -2.0f;
            const auto y = runMono (lim, x, 256);
            const float peak = peakAbs (y.data(), n);
            expect (peak <= ceiling + 1.0e-6f, "peak " + juce::String (peak) + " <= ceiling " + juce::String (ceiling) + " at " + juce::String (sr));
            expect (peak > 0.99f * ceiling, "and it does use the headroom (" + juce::String (peak) + ")");
            expect (lim.getMeterValue() < -3.0f, "meter shows the reduction (" + juce::String (lim.getMeterValue()) + ")");
            expect (allFinite (y.data(), n));
        }

        // Silence stays silence, exactly.
        {
            Limiter lim;
            lim.prepareToPlay (kRate, 512);
            const auto y = runMono (lim, std::vector<float> (48000, 0.0f), 300);
            expectEquals (peakAbs (y.data(), (int) y.size()), 0.0f);
            expectEquals (lim.getMeterValue(), 0.0f);
        }

        // Below the ceiling it only delays: bit-transparent, and the delay is the reported latency.
        {
            Limiter lim;
            lim.prepareToPlay (kRate, 512);
            const int latency = lim.getLatencySamples();
            const auto x = makeNoise (24000, 0.5, 32);      // peaks 0.5 < 0.89
            const auto y = runMono (lim, x, 100);
            bool identical = true;

            for (int i = latency; i < (int) x.size(); ++i)
                identical = identical && y[(size_t) i] == x[(size_t) (i - latency)];

            expect (identical, "below the ceiling the output is the input delayed by getLatencySamples()");
        }

        // Recovery: after a burst the gain returns to exactly 1 (no lingering pumping).
        {
            Limiter lim;
            lim.prepareToPlay (kRate, 512);
            std::vector<float> x (96000, 0.3f);
            for (int i = 1000; i < 1200; ++i)
                x[(size_t) i] = 3.0f;

            const auto y = runMono (lim, x, 128);
            const int latency = lim.getLatencySamples();
            expectEquals (y.back(), 0.3f);
            expect (y[(size_t) (latency + 1000 + 200 + 12000)] > 0.295f, "recovers with the 50 ms release");
        }

        // Parameters: ceiling and release move; a lower ceiling holds too.
        {
            Limiter lim;
            setParam (lim.apvts, "ceiling", -6.0f);
            setParam (lim.apvts, "release", 5.0f);
            lim.prepareToPlay (kRate, 512);
            const auto y = runMono (lim, makeNoise (48000, 1.5, 33), 200);
            expect (peakAbs (y.data(), 48000) <= dbToLinear (-6.0f) + 1.0e-6f);
        }
    }

    void testLookaheadLimiter()
    {
        // The master-bus configuration: -0.3 dBFS, 1.5 ms lookahead, 50 ms release, across rates and odd block sizes.
        for (double sr : { 44100.0, 48000.0, 96000.0 })
        {
            dsp::LookaheadLimiter limiter;
            limiter.prepare (sr, 1.5);
            limiter.setCeilingDb (-0.3f);
            limiter.setReleaseMs (50.0f);

            const float ceiling = dbToLinear (-0.3f);
            std::mt19937 rng (77);
            std::uniform_int_distribution<int> sizes (1, 2000);
            std::uniform_real_distribution<float> dist (-2.0f, 2.0f);

            float peak = 0.0f;

            for (int block = 0; block < 400; ++block)
            {
                const int n = sizes (rng);
                std::vector<float> l ((size_t) n), r ((size_t) n);

                for (int i = 0; i < n; ++i)
                {
                    l[(size_t) i] = dist (rng);
                    r[(size_t) i] = dist (rng);
                }

                // Sprinkle isolated full-size spikes and a DC step.
                if (n > 10)
                {
                    l[(size_t) (n / 2)] = 2.0f;
                    r[(size_t) (n / 3)] = -2.0f;
                }

                limiter.process (l.data(), r.data(), n, true);
                peak = juce::jmax (peak, peakAbs (l.data(), n), peakAbs (r.data(), n));
            }

            expect (peak <= ceiling + 1.0e-6f, "master limiter at " + juce::String (sr) + ": peak " + juce::String (peak) + " <= " + juce::String (ceiling));
            expect (peak > 0.99f * ceiling);
        }

        // A step from silence to +2.0 (worst case for the attack) never overshoots.
        {
            dsp::LookaheadLimiter limiter;
            limiter.prepare (kRate, 1.5);
            limiter.setCeilingDb (-0.3f);
            std::vector<float> x (4800, 0.0f);
            std::fill (x.begin() + 100, x.end(), 2.0f);
            auto r = x;
            limiter.process (x.data(), r.data(), (int) x.size(), true);
            expect (peakAbs (x.data(), (int) x.size()) <= dbToLinear (-0.3f) + 1.0e-6f);
        }

        // No pumping on normal material: peaks below the ceiling pass bit for bit, delayed.
        {
            dsp::LookaheadLimiter limiter;
            limiter.prepare (kRate, 1.5);
            limiter.setCeilingDb (-0.3f);
            const auto x = makeNoise (48000, 0.7, 41);
            auto l = x, r = x;
            limiter.process (l.data(), r.data(), (int) x.size(), true);
            const int d = limiter.getLatencySamples();
            bool same = true;

            for (int i = d; i < (int) x.size(); ++i)
                same = same && l[(size_t) i] == x[(size_t) (i - d)] && r[(size_t) i] == x[(size_t) (i - d)];

            expect (same, "transparent below the ceiling");
        }

        // A tone 2 dB over the ceiling: reduced by about 2 dB, and clean (release is slow against a 100 Hz cycle).
        {
            dsp::LookaheadLimiter limiter;
            limiter.prepare (kRate, 1.5);
            limiter.setCeilingDb (-0.3f);
            const int n = (int) kRate * 2;
            auto x = makeSine (n, kRate, 100.0, dbToLinear (1.7f));
            auto r = x;
            limiter.process (x.data(), r.data(), n, true);
            const double fundamental = toneDb (x.data() + n / 2, n / 2, kRate, 100.0);
            const double third = toneDb (x.data() + n / 2, n / 2, kRate, 300.0);
            logMessage ("Limiter 100 Hz, 2 dB over: fundamental " + juce::String (fundamental, 2) + " dBFS, 3rd harmonic " + juce::String (third - fundamental, 1) + " dB");
            expect (std::abs (fundamental + 0.3) < 0.4, "limited to about the ceiling (" + juce::String (fundamental) + ")");
            expect (third - fundamental < -40.0, "low distortion (3rd harmonic " + juce::String (third - fundamental) + " dB)");
        }

        // Switching off keeps the delay (no timing jump) and lets the signal through after the ~5 ms fade.
        {
            dsp::LookaheadLimiter limiter;
            limiter.prepare (kRate, 1.5);
            limiter.setCeilingDb (-0.3f);
            const int d = limiter.getLatencySamples();

            std::vector<float> hot (9600, 1.8f);
            auto hotR = hot;
            limiter.process (hot.data(), hotR.data(), 9600, true);
            expect (peakAbs (hot.data(), 9600) <= dbToLinear (-0.3f) + 1.0e-6f);

            hot.assign (9600, 1.8f);
            hotR = hot;
            limiter.process (hot.data(), hotR.data(), 9600, false);
            expect (std::abs (hot[4000] - 1.8f) < 1.0e-4f, "limiter off: unattenuated after the fade");

            // Flush with silence (long enough for the release to finish), then an impulse: it comes out exactly `d`
            // samples later with the limiter off.
            std::vector<float> quiet (48000, 0.0f);
            auto quietR = quiet;
            limiter.process (quiet.data(), quietR.data(), 48000, false);

            std::vector<float> impulse (2000, 0.0f);
            impulse[500] = 0.1f;
            auto impulseR = impulse;
            limiter.process (impulse.data(), impulseR.data(), 2000, false);
            expectEquals (impulse[(size_t) (500 + d)], 0.1f);

            // And the same with it on (the follower has been idle, so the protection engages within ~5 ms).
            std::vector<float> flush (9600, 0.0f);
            auto flushR = flush;
            limiter.process (flush.data(), flushR.data(), 9600, true);

            std::vector<float> impulseOn (2000, 0.0f);
            impulseOn[500] = 0.1f;
            auto impulseOnR = impulseOn;
            limiter.process (impulseOn.data(), impulseOnR.data(), 2000, true);
            expectEquals (impulseOn[(size_t) (500 + d)], 0.1f);
        }
    }

    // -----------------------------------------------------------------------------------------------
    void testResampler()
    {
        struct Rates { double in, out; };

        for (auto rates : { Rates { 44100.0, 48000.0 }, Rates { 48000.0, 44100.0 }, Rates { 96000.0, 48000.0 }, Rates { 48000.0, 96000.0 },
                            Rates { 22050.0, 48000.0 }, Rates { 192000.0, 48000.0 } })
        {
            dsp::Resampler rs;
            rs.prepare (rates.in, rates.out, 1, 1024);

            const int n = (int) rates.in;
            const auto x = makeSine (n, rates.in, 1000.0, 0.5);
            std::vector<float> y;
            std::vector<float> chunk ((size_t) rs.maxOutputFor (1024) + 8);

            for (int offset = 0; offset < n; offset += 333)
            {
                const int len = juce::jmin (333, n - offset);
                const int produced = rs.process (0, x.data() + offset, len, chunk.data());
                y.insert (y.end(), chunk.begin(), chunk.begin() + produced);
            }

            expect (std::abs ((double) y.size() - (double) n * rates.out / rates.in) < 2.0 + rs.getHalfWidth() * rates.out / rates.in,
                    "sample count follows the ratio (" + juce::String ((int) y.size()) + ")");

            const int half = (int) y.size() / 2;
            const double gain = toneDb (y.data() + half, (int) y.size() - half, rates.out, 1000.0) - toneDb (x.data() + n / 2, n / 2, rates.in, 1000.0);
            expect (std::abs (gain) < 0.05, "1 kHz passes at unity: " + juce::String (rates.in) + " -> " + juce::String (rates.out) + ": " + juce::String (gain, 3) + " dB");

            // Output sample k is the input at time k * step (zero delay in sample-index terms): a sine stays in phase.
            const double step = rates.in / rates.out;
            double worst = 0.0;

            for (int k = 2000; k < juce::jmin ((int) y.size(), 6000); ++k)
                worst = juce::jmax (worst, std::abs ((double) y[(size_t) k] - 0.5 * std::sin (2.0 * kPi * 1000.0 * (double) k * step / rates.in)));

            expect (worst < 2.0e-3, "phase and shape are exact (worst " + juce::String (worst, 5) + ")");
        }

        // Anti-aliasing: 30 kHz at 96 kHz must not appear at 18 kHz after decimation to 48 kHz.
        {
            dsp::Resampler rs;
            rs.prepare (96000.0, 48000.0, 1, 1024);
            const auto x = makeSine (96000, 96000.0, 30000.0, 0.5);
            std::vector<float> y, chunk ((size_t) rs.maxOutputFor (1024));

            for (int offset = 0; offset < 96000; offset += 1000)
            {
                const int produced = rs.process (0, x.data() + offset, juce::jmin (1000, 96000 - offset), chunk.data());
                y.insert (y.end(), chunk.begin(), chunk.begin() + produced);
            }

            expect (peakAbs (y.data() + 4800, (int) y.size() - 4800) < 0.5f * 0.001f, "30 kHz is removed before decimation (> 60 dB)");
        }

        // The stream does not depend on how it is cut into blocks.
        {
            const auto x = makeNoise (20000, 0.5, 51);
            std::vector<float> reference;

            for (int block : { 1024, 1, 7, 333 })
            {
                dsp::Resampler rs;
                rs.prepare (44100.0, 48000.0, 1, 1024);
                std::vector<float> y, chunk ((size_t) rs.maxOutputFor (1024) + 8);

                for (int offset = 0; offset < (int) x.size(); offset += block)
                {
                    const int produced = rs.process (0, x.data() + offset, juce::jmin (block, (int) x.size() - offset), chunk.data());
                    y.insert (y.end(), chunk.begin(), chunk.begin() + produced);
                }

                if (reference.empty())
                    reference = y;
                else
                    expect (y == reference, "resampler output independent of the block size (" + juce::String (block) + ")");
            }
        }
    }

    // -----------------------------------------------------------------------------------------------
    // Noise removal
    // -----------------------------------------------------------------------------------------------
    void testNoiseAttenuation()
    {
        // White noise at -30 dBFS (RMS): at least 10 dB down at strength 100 % / threshold 60 %.
        const int n = (int) kRate * 4;
        const auto x = makeNoise (n, 0.0316 * std::sqrt (3.0), 61);
        const double inputDb = toDb (rms (x.data() + n / 2, n / 2));
        expect (std::abs (inputDb + 30.0) < 0.3, "test signal is -30 dBFS RMS (" + juce::String (inputDb) + ")");

        {
            NoiseRemoval nr;
            useLight (nr);
            nr.prepareToPlay (kRate, 512);
            const auto y = runMono (nr, x, 256);
            const double outputDb = toDb (rms (y.data() + n / 2, n / 2));
            logMessage ("Noise removal, -30 dBFS white noise, 100 % / 60 %: " + juce::String (outputDb, 1) + " dBFS out");
            expect (outputDb <= inputDb - 10.0, "attenuated by at least 10 dB (" + juce::String (inputDb - outputDb) + ")");
            expect (allFinite (y.data(), n));
        }

        // The network itself does the work: with the gate held open it still removes the noise.
        {
            NoiseRemoval nr;
            useLight (nr);
            nr.prepareToPlay (kRate, 512);
            setParam (nr.apvts, "threshold", 0.0f);
            const auto y = runMono (nr, x, 256);
            const double outputDb = toDb (rms (y.data() + n / 2, n / 2));
            logMessage ("Noise removal with the gate open: " + juce::String (outputDb, 1) + " dBFS out");
            expect (outputDb <= inputDb - 10.0, "RNNoise alone removes at least 10 dB (" + juce::String (inputDb - outputDb) + ")");
        }

        // Strength is the attenuation limit (100 % = 30 dB): 50 % leaves the input 15 dB down, and the network takes it further.
        {
            NoiseRemoval nr;
            useLight (nr);
            setParam (nr.apvts, "strength", 50.0f);
            nr.prepareToPlay (kRate, 512);
            const auto y = runMono (nr, x, 256);
            const double outputDb = toDb (rms (y.data() + n / 2, n / 2));
            expect (outputDb - inputDb < -12.0 && outputDb - inputDb > -22.0, "50 % strength on noise is about -15 dB (" + juce::String (outputDb - inputDb) + ")");
        }

        // Voiced material passes at (about) full level (gate held open: it has its own test).
        {
            NoiseRemoval nr;
            useLight (nr);
            setParam (nr.apvts, "threshold", 0.0f);
            nr.prepareToPlay (kRate, 512);
            const auto v = makeVowels ((int) kRate * 3, kRate, 2.0, 5, 0.002);
            const auto y = runMono (nr, v, 480);
            const int from = (int) kRate, to = (int) kRate * 3 - 1;
            const double loss = toDb (rms (y.data() + from, to - from)) - toDb (rms (v.data() + from - nr.getLatencySamples(), to - from));
            expect (loss > -3.0, "voice-like material keeps its level (" + juce::String (loss) + " dB)");
        }
    }

    void testNoiseLatency()
    {
        expectEquals (NoiseRemoval::kNativeLatency, 1440);

        for (double sr : { 44100.0, 48000.0, 88200.0, 96000.0 })
        {
            // Strength 0 %: the dry path alone. An impulse comes out exactly latency samples later, whatever the block size.
            for (int block : { 1, 37, 480, 1024 })
            {
                NoiseRemoval nr;
                useLight (nr);
                setParam (nr.apvts, "strength", 0.0f);
                nr.prepareToPlay (sr, 1024);
                const int latency = nr.getLatencySamples();
                expect (latency > 0, "latency is reported");

                std::vector<float> x ((size_t) (latency + 6000), 0.0f);
                x[1000] = 0.5f;
                const auto y = runMono (nr, x, block);

                int at = -1;
                float peak = 0.0f;

                for (int i = 0; i < (int) y.size(); ++i)
                    if (std::abs (y[(size_t) i]) > peak) { peak = std::abs (y[(size_t) i]); at = i; }

                expectEquals (at, 1000 + latency, "impulse delay at " + juce::String (sr) + " Hz, block " + juce::String (block));
                expectEquals (peak, 0.5f);
                expectEquals ((int) nr.getUnderruns(), 0);

                if (block == 480 && sr == kRate)
                    expectEquals (latency, 1440);
            }

            // Strength 100 %, gate open: the wet path is delayed by the same amount (correlation peak at that lag).
            NoiseRemoval nr;
            useLight (nr);
            setParam (nr.apvts, "threshold", 0.0f);
            nr.prepareToPlay (sr, 1024);
            const int latency = nr.getLatencySamples();
            const int n = (int) sr * 3;
            const auto x = makeVowels (n, sr, 2.0, 9, 0.0);
            // A band above 700 Hz only, so RNNoise's own low-frequency high-pass does not bias the delay.
            const auto y = runMono (nr, x, 333);

            int bestLag = -1;
            double best = -2.0;

            for (int lag = latency - 6; lag <= latency + 6; ++lag)
            {
                const double c = correlationAtLag (x, y, lag, (int) sr, n - 1);
                if (c > best) { best = c; bestLag = lag; }
            }

            logMessage ("Noise removal wet path at " + juce::String (sr) + " Hz: reported latency " + juce::String (latency) + ", correlation peaks at "
                        + juce::String (bestLag) + " (r = " + juce::String (best, 4) + ")");
            expectEquals (bestLag, latency, "wet path is aligned with the dry path at " + juce::String (sr) + " Hz");
            expect (best > 0.95, "wet path follows the input");
        }
    }

    void testNoiseBlockSizes()
    {
        for (double sr : { 48000.0, 44100.0 })
        {
            const int n = (int) sr;
            const auto x = makeVowels (n, sr, 4.0, 15, 0.02);
            std::vector<float> reference;

            for (int block : { 480, 1, 7, 100, 481, 1023, 1024, 1025 })
            {
                NoiseRemoval nr;
                useLight (nr);
                setParam (nr.apvts, "strength", 70.0f);
                nr.prepareToPlay (sr, 1024);
                const auto y = runMono (nr, x, block);

                expect (allFinite (y.data(), n), "no NaN/Inf with blocks of " + juce::String (block));
                expectEquals ((int) nr.getUnderruns(), 0);

                if (reference.empty())
                    reference = y;
                else
                    expect (y == reference, "output does not depend on the block size (" + juce::String (block) + " at " + juce::String (sr) + " Hz)");
            }
        }

        // Every size from 1 to 1024 once, back to back, on live input: finite.
        NoiseRemoval nr;
        useLight (nr);
        nr.prepareToPlay (kRate, 1024);
        auto noise = makeNoise (1024, 0.3, 71);
        auto copy = noise;
        bool finite = true;

        for (int size = 1; size <= 1024; ++size)
        {
            auto a = noise, b = copy;
            runProcessor (nr, a.data(), b.data(), size, 0);
            finite = finite && allFinite (a.data(), size) && allFinite (b.data(), size);
        }

        expect (finite, "sizes 1..1024 produce finite audio");
    }

    void testNoiseMono()
    {
        for (double sr : { 48000.0, 44100.0 })
        {
            const int n = (int) sr * 3;
            const auto voice = makeVowels (n, sr, 3.0, 25, 0.02);
            auto right = voice;

            // The last two seconds differ between the channels; the first second is identical (mono microphone).
            const auto other = makeNoise (n, 0.02, 26);
            for (int i = (int) sr; i < n; ++i)
                right[(size_t) i] = 0.4f * voice[(size_t) i] + other[(size_t) i];

            NoiseRemoval optimised, reference;
            useLight (optimised);
            useLight (reference);
            setParam (optimised.apvts, "strength", 80.0f);
            setParam (reference.apvts, "strength", 80.0f);
            optimised.prepareToPlay (sr, 1024);
            reference.prepareToPlay (sr, 1024);
            reference.setForceStereo (true);

            const auto a = runStereo (optimised, voice, right, 300);
            const auto b = runStereo (reference, voice, right, 300);

            expect (a.l == b.l && a.r == b.r, "one shared state gives exactly what two states would (" + juce::String (sr) + " Hz)");

            // While the input is identical, so is the output.
            bool same = true;
            for (int i = 0; i < (int) sr - 300; ++i)
                same = same && a.l[(size_t) i] == a.r[(size_t) i];

            expect (same, "identical channels stay identical");
            expect (allFinite (a.l.data(), n) && allFinite (a.r.data(), n));
        }
    }

    void testNoiseRates()
    {
        // Resampled rates: still removes noise, still finite, latency reported.
        for (double sr : { 22050.0, 32000.0, 44100.0, 88200.0, 96000.0, 192000.0 })
        {
            NoiseRemoval nr;
            useLight (nr);
            nr.prepareToPlay (sr, 1024);
            expect (nr.getRateMode() == NoiseRemoval::RateMode::resampled);
            expect (nr.getLatencySamples() > 0);

            const int n = (int) sr * 3;
            const auto x = makeNoise (n, 0.0316 * std::sqrt (3.0), 81);
            const auto y = runMono (nr, x, 256);
            const double reduction = toDb (rms (x.data() + n / 2, n / 2)) - toDb (rms (y.data() + n / 2, n / 2));
            expect (allFinite (y.data(), n));
            expect (reduction >= 10.0, "noise at " + juce::String (sr) + " Hz reduced by " + juce::String (reduction) + " dB");
            expectEquals ((int) nr.getUnderruns(), 0);
        }

        // A long run at an awkward ratio and block size: the FIFO between the converters neither drains nor fills.
        {
            NoiseRemoval nr;
            useLight (nr);
            nr.prepareToPlay (44100.0, 1024);
            const auto chunk = makeNoise (44100, 0.1, 85);
            auto l = chunk, r = chunk;

            for (int second = 0; second < 90; ++second)
            {
                l = chunk;
                r = chunk;
                runProcessor (nr, l.data(), r.data(), (int) l.size(), 333);
            }

            expectEquals ((int) nr.getUnderruns(), 0, "90 s at 44.1 kHz with 333-frame blocks: no underrun");
            expect (allFinite (l.data(), (int) l.size()));
        }

        // Native rate.
        {
            NoiseRemoval nr;
            useLight (nr);
            nr.prepareToPlay (48000.0, 512);
            expect (nr.getRateMode() == NoiseRemoval::RateMode::native);
            expect (! nr.isPassingThrough());
        }

        // Out of range: untouched pass-through, no latency, and the UI can tell.
        for (double sr : { 4000.0, 384000.0 })
        {
            NoiseRemoval nr;
            useLight (nr);
            nr.prepareToPlay (sr, 512);
            expect (nr.getRateMode() == NoiseRemoval::RateMode::unsupported);
            expect (nr.isPassingThrough());
            expectEquals (nr.getLatencySamples(), 0);
            const auto x = makeNoise (2000, 0.3, 82);
            expect (runMono (nr, x, 100) == x, "audio passes through untouched at " + juce::String (sr) + " Hz");
        }

        // Re-preparing at a new rate works (the engine does this on a sample-rate change).
        {
            NoiseRemoval nr;
            useLight (nr);
            nr.prepareToPlay (48000.0, 512);
            runMono (nr, makeNoise (4800, 0.2, 83), 300);
            nr.releaseResources();
            nr.prepareToPlay (44100.0, 512);
            expect (nr.getRateMode() == NoiseRemoval::RateMode::resampled);
            expect (allFinite (runMono (nr, makeNoise (4800, 0.2, 84), 300).data(), 4800));
        }
    }

    void testNoiseGate()
    {
        // 0.6 s of voice-like sound, 0.4 s of quiet room, repeated.
        const int n = (int) kRate * 5;
        auto x = makeVowels (n, kRate, 0.0, 5, 0.0);           // flat envelope: the on/off window below shapes it
        const auto room = makeNoise (n, 0.004, 6);

        for (int i = 0; i < n; ++i)
        {
            const int inPeriod = i % (int) kRate;
            const int voiceEnd = (int) (kRate * 0.6);
            const float on = inPeriod < voiceEnd ? 1.0f : 0.0f;
            const float edge = juce::jmin (1.0f, (float) juce::jmin (inPeriod, std::abs (inPeriod - voiceEnd)) / 240.0f);
            x[(size_t) i] = x[(size_t) i] * on * edge + room[(size_t) i];
        }

        auto runTrace = [&] (NoiseRemoval& p, std::vector<float>& gateTrace, std::vector<float>& vadTrace)
        {
            auto left = x;
            auto right = x;
            juce::MidiBuffer midi;

            for (int offset = 0; offset + 480 <= n; offset += 480)
            {
                float* ch[2] { left.data() + offset, right.data() + offset };
                juce::AudioBuffer<float> buffer (ch, 2, 480);
                p.processBlock (buffer, midi);
                gateTrace.push_back (p.getGateGain());
                vadTrace.push_back (p.getMeterValue());
            }

            return left;
        };

        NoiseRemoval open, gated, held;
        useLight (open);
        useLight (gated);
        useLight (held);
        setParam (open.apvts, "threshold", 0.0f);
        setParam (gated.apvts, "threshold", 60.0f);
        setParam (gated.apvts, "hold", 0.0f);
        setParam (held.apvts, "threshold", 60.0f);
        setParam (held.apvts, "hold", 500.0f);
        open.prepareToPlay (kRate, 512);
        gated.prepareToPlay (kRate, 512);
        held.prepareToPlay (kRate, 512);

        std::vector<float> openGate, openVad, gatedGate, gatedVad, heldGate, heldVad;
        runTrace (open, openGate, openVad);
        runTrace (gated, gatedGate, gatedVad);
        runTrace (held, heldGate, heldVad);

        // The gate acts on the network's output before the dry part is added back (Strength), so its gain is measured
        // there: the same engine twice, once with the gate held open and once as the "gated" effect above has it.
        auto wetOf = [&] (float threshold, int holdFrames)
        {
            auto engine = denoise::RnnoiseEngine::create();
            engine->setGate (threshold, holdFrames);
            std::vector<float> y (x.size());

            for (int offset = 0; offset + 480 <= n; offset += 480)
            {
                const float* in[1] { x.data() + offset };
                float* out[1] { y.data() + offset };
                engine->process (in, out, 1, 480);
            }

            return y;
        };

        const auto a = wetOf (0.0f, 17);
        const auto b = wetOf (0.6f, 2);

        const float maxVad = *std::max_element (openVad.begin(), openVad.end());
        const float minVad = *std::min_element (openVad.begin() + 20, openVad.end());
        logMessage ("Noise gate: voice probability " + juce::String (minVad, 2) + " .. " + juce::String (maxVad, 2));
        expect (maxVad > 0.9f, "voice probability reaches 1 during the voice-like part");
        expect (minVad < 0.3f, "and falls towards 0 in the quiet room");
        expect (minVad >= 0.0f && maxVad <= 1.0f);

        expect (*std::min_element (openGate.begin(), openGate.end()) == 1.0f, "threshold 0 %: the gate never closes");

        // Timing, from the network's own voice probability: the gate must be open for `holdFrames` frames after the
        // last frame at or above the threshold (well above it, to stay clear of rounding), and fully closed a little
        // after that (the close fade is 60 ms = 6 frames; the first block of a decision ramps under the old one).
        auto checkGate = [&] (const std::vector<float>& gate, int holdFrames, const juce::String& what)
        {
            int lastStrict = -100000, lastLoose = -100000, openChecks = 0, closedChecks = 0, wrong = 0;

            for (int k = 1; k < (int) gate.size(); ++k)
            {
                // The decision made at the end of block k - 1 shapes the gain during block k.
                if (openVad[(size_t) k - 1] >= 0.62f) lastStrict = k - 1;
                if (openVad[(size_t) k - 1] >= 0.58f) lastLoose = k - 1;

                if (lastStrict > 0 && k - 1 - lastStrict <= holdFrames - 1 && k - 1 > lastStrict)
                {
                    ++openChecks;
                    wrong += gate[(size_t) k] < 0.99f ? 1 : 0;
                }

                if (lastLoose > 0 && k - 1 - lastLoose >= holdFrames + 6)
                {
                    ++closedChecks;
                    wrong += gate[(size_t) k] > 0.01f ? 1 : 0;
                }
            }

            logMessage ("Noise gate (" + what + "): " + juce::String (openChecks) + " open checks, " + juce::String (closedChecks) + " closed checks");
            expect (openChecks > 5 && closedChecks > 20, what + ": the check is not vacuous");
            expectEquals (wrong, 0, what + ": gate open for the hold time after the last voice frame and closed after it");
        };

        checkGate (gatedGate, 2, "threshold 60 %, hold 0");
        checkGate (heldGate, 52, "threshold 60 %, hold 500 ms");

        // The gain applied by the gate is the ratio of the two runs (same network state, only the gate differs).
        double worstStep = 0.0;
        float previous = 0.0f;
        int previousIndex = -10, compared = 0;

        for (int i = 2000; i < n; ++i)
        {
            if (std::abs (a[(size_t) i]) > 5.0e-4f)
            {
                const float gain = b[(size_t) i] / a[(size_t) i];

                if (i == previousIndex + 1)         // consecutive samples only
                    worstStep = juce::jmax (worstStep, (double) std::abs (gain - previous));

                previous = gain;
                previousIndex = i;
                ++compared;
            }
        }

        logMessage ("Noise gate: largest per-sample gain step " + juce::String (worstStep, 5) + " over " + juce::String (compared) + " samples");
        expect (compared > 20000, "enough audio to compare (" + juce::String (compared) + ")");
        expect (worstStep < 0.0075, "fades are smooth (5 ms open / 60 ms close): largest step " + juce::String (worstStep));
    }

    void testCpuCosts()
    {
        // Informational (and a generous safety net): cost of each effect on 10 s of stereo audio in 256-frame blocks.
        for (const auto& info : BuiltinFx::list())
        {
            auto fx = BuiltinFx::create (info.id);
            fx->prepareToPlay (kRate, 256);
            auto l = makeVowels ((int) kRate * 10, kRate, 3.0, 5, 0.01);
            auto r = l;

            // Make the second channel differ so the noise removal really runs two states.
            for (size_t i = 0; i < r.size(); i += 2)
                r[i] *= 0.9f;

            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            runProcessor (*fx, l.data(), r.data(), (int) l.size(), 256);
            const auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
            logMessage ("CPU: " + info.id + " stereo 48 kHz: " + juce::String (ms / 100.0, 2) + " % of one core");
           #if ! JUCE_DEBUG
            expect (ms < 2000.0, info.id + " is far below real time");      // (unoptimised debug builds are not judged)
           #endif
        }

        for (double sr : { 44100.0, 96000.0 })
        {
            NoiseRemoval nr;
            useLight (nr);
            nr.prepareToPlay (sr, 256);
            auto l = makeNoise ((int) sr * 10, 0.1, 3);
            auto r = l;
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            runProcessor (nr, l.data(), r.data(), (int) l.size(), 256);
            logMessage ("CPU: noise removal mono mic at " + juce::String (sr) + " Hz (resampled): "
                        + juce::String ((juce::Time::getMillisecondCounterHiRes() - t0) / 100.0, 2) + " % of one core");
        }
    }

    void testNoiseCpu()
    {
        NoiseRemoval nr;
        useLight (nr);
        nr.prepareToPlay (kRate, 512);
        auto x = makeNoise ((int) kRate * 5, 0.2, 91);
        auto copy = x;
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        runProcessor (nr, x.data(), copy.data(), (int) x.size(), 256);
        const auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
        logMessage ("Noise removal (mono mic): 5 s of audio in " + juce::String (ms, 1) + " ms = " + juce::String (ms / 50.0, 2) + " % of one core");
       #if ! JUCE_DEBUG
        expect (ms < 2500.0, "far below real time");
       #endif
    }

    // -----------------------------------------------------------------------------------------------
    void testRobustness()
    {
        for (const auto& info : BuiltinFx::list())
        {
            for (double sr : { 44100.0, 48000.0, 96000.0 })
            {
                for (int extreme = 0; extreme < 3; ++extreme)
                {
                    auto fx = BuiltinFx::create (info.id);

                    // extreme 0: defaults, 1: every parameter at its minimum, 2: at its maximum
                    if (extreme > 0)
                        for (auto* param : fx->getParameters())
                            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param))
                                ranged->setValueNotifyingHost ((float) (extreme - 1));

                    // Eight bands all at +24 dB and Q 18 on top of each other really are a 300 dB boost: keep the EQ's gains
                    // at +6 dB and its Q at 1; types, frequencies, slopes and the on switches stay extreme. Random gains,
                    // types and Q are in EqTests.
                    if (info.id == "eq")
                        for (int b = 0; b < Equalizer::kNumBands; ++b)
                        {
                            if (auto* gain = fx->apvts.getParameter (Equalizer::bandParamId (b, "Gain")))
                                gain->setValueNotifyingHost (gain->convertTo0to1 (6.0f));

                            if (auto* q = fx->apvts.getParameter (Equalizer::bandParamId (b, "Q")))
                                q->setValueNotifyingHost (q->convertTo0to1 (1.0f));
                        }

                    fx->prepareToPlay (sr, 1024);

                    std::mt19937 rng (5);
                    std::uniform_int_distribution<int> sizes (1, 1024);
                    bool finite = true;
                    float worst = 0.0f;

                    for (int pass = 0; pass < 60; ++pass)
                    {
                        const int n = sizes (rng);
                        std::vector<float> l ((size_t) n), r ((size_t) n);

                        // silence, full-scale noise, +-4 sine, DC: rotating through them
                        for (int i = 0; i < n; ++i)
                        {
                            switch (pass % 4)
                            {
                                case 0:  l[(size_t) i] = r[(size_t) i] = 0.0f; break;
                                case 1:  l[(size_t) i] = 2.0f * (float) rng() / (float) rng.max() - 1.0f; r[(size_t) i] = -l[(size_t) i]; break;
                                case 2:  l[(size_t) i] = r[(size_t) i] = 4.0f * std::sin (0.05f * (float) i); break;
                                default: l[(size_t) i] = r[(size_t) i] = 0.9f; break;
                            }
                        }

                        runProcessor (*fx, l.data(), r.data(), n, 0);
                        finite = finite && allFinite (l.data(), n) && allFinite (r.data(), n);
                        worst = juce::jmax (worst, peakAbs (l.data(), n), peakAbs (r.data(), n));
                    }

                    expect (finite, info.id + " at " + juce::String (sr) + " Hz, parameter set " + juce::String (extreme) + ": finite output");
                    expect (worst < 1.0e4f, info.id + ": bounded output (" + juce::String (worst) + ")");

                    // Mono bus.
                    juce::AudioBuffer<float> mono (1, 300);
                    for (int i = 0; i < 300; ++i)
                        mono.setSample (0, i, 0.3f * std::sin (0.1f * (float) i));

                    juce::MidiBuffer midi;
                    fx->processBlock (mono, midi);
                    expect (allFinite (mono.getReadPointer (0), 300), info.id + ": mono bus is finite");

                    expect (std::isfinite (fx->getMeterValue()), info.id + ": meter value is finite");
                }
            }
        }
    }
};

static DspTests dspTests;
}
