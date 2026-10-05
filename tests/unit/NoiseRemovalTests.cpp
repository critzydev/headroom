// Unit tests for the noise removal: the three modes, latency, strength, switching, fallback, meters, listening to the
// removed part, quality on a speech-like signal, and that the audio thread never allocates.
#include "Juce.h"

#include "AllocationGuard.h"
#include "MallocGuard.h"
#include "NoiseTestSignals.h"
#include "TestHelpers.h"
#include "app/AppController.h"
#include "engine/builtin/NoiseRemoval.h"
#include "engine/denoise/DfnEngine.h"
#include "engine/denoise/DfnRuntime.h"
#include "engine/denoise/RnnoiseEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numeric>
#include <atomic>
#include <random>
#include <thread>
#include <vector>

namespace hr
{
using namespace test;

namespace
{
    using Mode = NoiseRemoval::Mode;

    bool dfnPresent()   { return dfn::isAvailable (dfn::Model::fast) && dfn::isAvailable (dfn::Model::best); }

    void setParam (juce::AudioProcessorValueTreeState& apvts, const char* id, float plainValue)
    {
        auto* p = apvts.getParameter (id);
        jassert (p != nullptr);
        p->setValueNotifyingHost (p->convertTo0to1 (plainValue));
    }

    void setMode (NoiseRemoval& nr, Mode mode)     { setParam (nr.apvts, "mode", (float) (int) mode); }

    // Offline processing is faster than real time: without this the worker thread would be late for many frames.
    struct Lockstep
    {
        Lockstep()  { denoise::DfnEngine::setTestLockstep (true); }
        ~Lockstep() { denoise::DfnEngine::setTestLockstep (false); }
    };

    void setEnvironment (const char* name, const juce::String& value)
    {
       #if JUCE_WINDOWS
        _putenv_s (name, value.toRawUTF8());
       #else
        if (value.isEmpty())
            unsetenv (name);
        else
            setenv (name, value.toRawUTF8(), 1);
       #endif
    }

    // ---- Signals: see NoiseTestSignals.h ----------------------------------------------------------------
    // Signal-to-noise ratio of y with respect to the clean signal s, y delayed by `lag`: y = a * s + residual, so the
    // gain the effect applied to the speech does not count as noise.
    double snrDb (const std::vector<float>& y, const std::vector<float>& s, int lag, int from, int to)
    {
        double ys = 0.0, ss = 0.0, yy = 0.0;

        for (int i = from; i < to; ++i)
        {
            const double a = (double) y[(size_t) (i + lag)], b = (double) s[(size_t) i];
            ys += a * b;
            ss += b * b;
            yy += a * a;
        }

        const double gain = ys / ss;
        const double residual = yy - 2.0 * gain * ys + gain * gain * ss;
        return 10.0 * std::log10 (gain * gain * ss / juce::jmax (1.0e-30, residual));
    }

    double maxSecondDifference (const std::vector<float>& v, int from, int to)
    {
        double worst = 0.0;

        for (int i = juce::jmax (2, from); i < to; ++i)
            worst = juce::jmax (worst, (double) std::abs (v[(size_t) i] - 2.0f * v[(size_t) i - 1] + v[(size_t) i - 2]));

        return worst;
    }

    // Normalised correlation of y with x delayed by `lag`, over [from, to).
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

    // Runs `nr` on a mono signal in blocks; before every block `everyBlock` may act. Optionally paced like real
    // time, which mode switches need (the builder thread works in wall-clock time).
    std::vector<float> runPaced (NoiseRemoval& nr, const std::vector<float>& x, int block, bool paced,
                                 const std::function<bool (int)>& afterBlock = {})
    {
        std::vector<float> l (x), r (x);
        juce::MidiBuffer midi;

        for (int offset = 0; offset < (int) x.size(); offset += block)
        {
            const int n = juce::jmin (block, (int) x.size() - offset);
            float* channels[2] { l.data() + offset, r.data() + offset };
            juce::AudioBuffer<float> buffer (channels, 2, n);
            nr.processBlock (buffer, midi);

            if (paced)
                juce::Thread::sleep (juce::jmax (1, (int) (1000.0 * n / kRate * 0.5)));

            if (afterBlock && afterBlock (offset + n))
                break;
        }

        return l;
    }
}

class NoiseRemovalTests final : public juce::UnitTest
{
public:
    NoiseRemovalTests() : juce::UnitTest ("NoiseRemoval", "noise") {}          // also part of the default run; `headroom_tests noise` runs just these

    void runTest() override
    {
        beginTest ("strength maps to the attenuation limit; defaults and old states");
        testMappingAndMigration();

        beginTest ("the effect works without model files: Light runs and says so");
        testFallback();

        if (! dfnPresent())
        {
            logMessage ("DeepFilterNet models are not available here: the tests of Fast and Best are skipped");
            beginTest ("light: latency, blend and gate");
            testLightBasics();
            beginTest ("hear what's removed, taps and meters");
            testHearRemovedAndTaps();
            beginTest ("AppController: hear what's removed routes and restores");
            testControllerHearRemoved();
            return;
        }

        beginTest ("latency: reported = measured impulse delay, for every mode and rate");
        testLatency();

        beginTest ("the network's output is aligned with the delayed input (both models)");
        testEngineAlignment();

        beginTest ("light: latency, blend and gate");
        testLightBasics();

        beginTest ("strength limits how much noise is removed (all modes)");
        testStrengthOnNoise();

        beginTest ("mode switches are click-free");
        testModeSwitches();

        beginTest ("blocks of any size: finite, and the result does not depend on them");
        testBlockSizes();

        beginTest ("stereo: the second lane starts cleanly");
        testStereo();

        beginTest ("worker thread: priority hook and quick shutdown");
        testWorkerThread();

        beginTest ("missed frames: the input plays in place, then the network again");
        testMissedFrames();

        beginTest ("hear what's removed, taps and meters");
        testHearRemovedAndTaps();

        beginTest ("AppController: hear what's removed routes and restores");
        testControllerHearRemoved();

        beginTest ("the audio thread never allocates (also across a mode switch)");
        testNoAllocation();

        beginTest ("quality: speech in room noise at 5 dB, every mode");
        testQuality();

        beginTest ("CPU and timing per mode");
        testCpu();
    }

private:
    // -----------------------------------------------------------------------------------------------
    void testMappingAndMigration()
    {
        expectWithinAbsoluteError (NoiseRemoval::attenuationDbForStrength (0.0f), 0.0f, 1.0e-6f);
        expectWithinAbsoluteError (NoiseRemoval::attenuationDbForStrength (100.0f), 30.0f, 1.0e-5f);
        expectWithinAbsoluteError (NoiseRemoval::attenuationDbForStrength (80.0f), 24.0f, 1.0e-5f);
        expectWithinAbsoluteError (NoiseRemoval::attenuationDbForStrength (50.0f), 15.0f, 1.0e-5f);
        expectWithinAbsoluteError (NoiseRemoval::attenuationDbForStrength (-5.0f), 0.0f, 1.0e-6f);
        expectWithinAbsoluteError (NoiseRemoval::attenuationDbForStrength (250.0f), 30.0f, 1.0e-5f);
        expectWithinAbsoluteError (NoiseRemoval::dryMixForStrength (0.0f), 1.0f, 1.0e-6f);
        expectWithinAbsoluteError (NoiseRemoval::dryMixForStrength (100.0f), std::pow (10.0f, -1.5f), 1.0e-5f);
        expectWithinAbsoluteError (NoiseRemoval::dryMixForStrength (80.0f), std::pow (10.0f, -1.2f), 1.0e-5f);

        {
            NoiseRemoval nr;
            expectEquals ((int) nr.getRequestedMode(), (int) Mode::best, "new slots are Best");
            expectWithinAbsoluteError (nr.apvts.getRawParameterValue ("strength")->load(), 80.0f, 1.0e-4f);
            expectWithinAbsoluteError (nr.apvts.getRawParameterValue ("postFilter")->load(), 0.0f, 1.0e-4f);
            expectWithinAbsoluteError (nr.apvts.getRawParameterValue ("threshold")->load(), 60.0f, 1.0e-4f);
            expectWithinAbsoluteError (nr.apvts.getRawParameterValue ("hold")->load(), 150.0f, 1.0e-4f);

            // A whole-percent choice: the three modes are what the panel offers.
            auto* choice = dynamic_cast<juce::AudioParameterChoice*> (nr.apvts.getParameter ("mode"));
            expect (choice != nullptr && choice->choices == juce::StringArray ({ "Fast", "Best", "Light" }));
        }

        // A state saved by version 0.1.x has no "mode" and no "postFilter": it loads their defaults (Best, post-filter
        // off), strength kept, and whatever the processor was set to before does not survive the missing entry.
        {
            NoiseRemoval fresh;
            auto oldState = fresh.apvts.copyState();

            for (const char* removed : { "mode", "postFilter" })
                if (auto child = oldState.getChildWithProperty ("id", removed); child.isValid())
                    oldState.removeChild (child, nullptr);

            auto xml = oldState.createXml();
            expect (xml != nullptr);

            for (auto* param : xml->getChildIterator())
                if (param->getStringAttribute ("id") == "strength")
                    param->setAttribute ("value", 70.0);

            expect (xml->getChildByAttribute ("id", "mode") == nullptr, "the old state has no mode");

            juce::MemoryBlock blob;
            BlobMaker::make (*xml, blob);

            NoiseRemoval loaded;
            setMode (loaded, Mode::light);                          // whatever it was, the missing entry must not keep it
            setParam (loaded.apvts, "postFilter", 40.0f);
            loaded.setStateInformation (blob.getData(), (int) blob.getSize());
            expectEquals ((int) loaded.getRequestedMode(), (int) Mode::best, "an old state without mode loads as Best");
            expectWithinAbsoluteError (loaded.apvts.getRawParameterValue ("postFilter")->load(), 0.0f, 0.01f);
            expectWithinAbsoluteError (loaded.apvts.getRawParameterValue ("strength")->load(), 70.0f, 0.2f);
            expectWithinAbsoluteError (loaded.apvts.getRawParameterValue ("hold")->load(), 150.0f, 0.01f);

            // The same when the processor had been left on the default (Fast is no longer it): still Best.
            NoiseRemoval untouched;
            untouched.setStateInformation (blob.getData(), (int) blob.getSize());
            expectEquals ((int) untouched.getRequestedMode(), (int) Mode::best);
            logMessage ("old state -> mode " + juce::String (NoiseRemoval::getModeName (loaded.getRequestedMode()))
                        + ", strength " + juce::String (loaded.apvts.getRawParameterValue ("strength")->load(), 1));
        }

        // Nothing has to be touched for the mode to be listed: a saved state names every parameter. That is why a slot that
        // 0.2.x saved while it was still on the default has mode = Fast written out, and stays on Fast.
        {
            NoiseRemoval untouched;
            juce::MemoryBlock blob;
            untouched.getStateInformation (blob);
            auto xml = juce::AudioProcessor::getXmlFromBinary (blob.getData(), (int) blob.getSize());
            auto* listed = xml != nullptr ? xml->getChildByAttribute ("id", "mode") : nullptr;
            expect (listed != nullptr, "an untouched Noise Removal lists its mode");

            if (listed != nullptr)
                expectEquals (listed->getIntAttribute ("value", -1), (int) Mode::best, "and it is the default, Best");
        }

        // A state that has "mode" keeps what was chosen, all three ways, into a processor that starts on Best (the
        // default) and into one that starts on something else. A state saved by 0.2.x while Fast was the default
        // has mode = 0 written out (the parameter tree always lists every parameter), so it stays on Fast.
        for (auto chosen : { Mode::fast, Mode::best, Mode::light })
        {
            NoiseRemoval a;
            setMode (a, chosen);
            juce::MemoryBlock blob;
            a.getStateInformation (blob);

            auto xml = juce::AudioProcessor::getXmlFromBinary (blob.getData(), (int) blob.getSize());
            expect (xml != nullptr && xml->getChildByAttribute ("id", "mode") != nullptr, "a saved state lists the mode explicitly");

            for (auto startedOn : { Mode::fast, Mode::best, Mode::light })
            {
                NoiseRemoval b;
                setMode (b, startedOn);
                b.setStateInformation (blob.getData(), (int) blob.getSize());
                expectEquals ((int) b.getRequestedMode(), (int) chosen,
                              juce::String ("saved on ") + NoiseRemoval::getModeName (chosen) + ", loaded over " + NoiseRemoval::getModeName (startedOn));
            }

            NoiseRemoval fresh;
            fresh.setStateInformation (blob.getData(), (int) blob.getSize());
            expectEquals ((int) fresh.getRequestedMode(), (int) chosen, "loaded into a new slot");
        }

        // The whole thing round-trips.
        {
            NoiseRemoval a;
            setMode (a, Mode::best);
            setParam (a.apvts, "strength", 35.0f);
            setParam (a.apvts, "postFilter", 20.0f);

            juce::MemoryBlock blob;
            a.getStateInformation (blob);

            NoiseRemoval b;
            b.setStateInformation (blob.getData(), (int) blob.getSize());
            expectEquals ((int) b.getRequestedMode(), (int) Mode::best);
            expectWithinAbsoluteError (b.apvts.getRawParameterValue ("strength")->load(), 35.0f, 0.2f);
            expectWithinAbsoluteError (b.apvts.getRawParameterValue ("postFilter")->load(), 20.0f, 0.2f);
        }
    }

    // AudioProcessor::copyXmlToBinary is protected: this borrows it.
    struct BlobMaker : TestProcessor
    {
        static void make (const juce::XmlElement& xml, juce::MemoryBlock& blob)     { copyXmlToBinary (xml, blob); }
    };

    // -----------------------------------------------------------------------------------------------
    void testFallback()
    {
        const auto emptyFolder = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("headroom-no-models", "", false);
        emptyFolder.createDirectory();
        setEnvironment ("HEADROOM_MODELS_DIR", emptyFolder.getFullPathName());

        for (auto mode : { Mode::fast, Mode::best })
        {
            NoiseRemoval nr;
            setMode (nr, mode);
            nr.prepareToPlay (kRate, 512);

            expect (nr.getActiveMode() == Mode::light, "Light runs instead");
            expect (nr.getStatus() == NoiseRemoval::Status::unavailable || nr.getStatus() == NoiseRemoval::Status::loading);

            // The builder finds out that the files are not there and says so.
            expect (! nr.waitUntilReady (20000), "not ready, because there is nothing to get ready");
            expect (nr.getStatus() == NoiseRemoval::Status::unavailable, "unavailable");
            expect (nr.getActiveMode() == Mode::light);
            expect (nr.getProblem() == (dfn::isCompiledIn() ? NoiseRemoval::Problem::modelsMissing : NoiseRemoval::Problem::notCompiledIn));
            expect (nr.getStatusNote().contains ("Light"), "the panel can say so: " + nr.getStatusNote());
            expectEquals (nr.getLatencySamples(), NoiseRemoval::kNativeLatency);

            // And it still cleans: white noise at -30 dBFS comes out at least 10 dB lower.
            const int n = (int) kRate * 3;
            const auto x = makeNoise (n, 0.0316 * std::sqrt (3.0), 61);
            const auto y = runMono (nr, x, 256);
            const double reduction = toDb (rms (x.data() + n / 2, n / 2)) - toDb (rms (y.data() + n / 2, n / 2));
            expect (reduction >= 10.0, "still removes noise: " + juce::String (reduction, 1) + " dB");
            expect (allFinite (y.data(), n));
        }

        setEnvironment ("HEADROOM_MODELS_DIR", {});
        emptyFolder.deleteRecursively();
    }

    // -----------------------------------------------------------------------------------------------
    void testLatency()
    {
        struct Case { Mode mode; double rate; };
        const Case cases[] { { Mode::fast, 48000.0 }, { Mode::best, 48000.0 }, { Mode::light, 48000.0 },
                             { Mode::fast, 44100.0 }, { Mode::best, 96000.0 }, { Mode::light, 44100.0 } };

        for (const auto& c : cases)
        {
            // Strength 0 %: the input, delayed by the latency. An impulse comes out exactly `latency` samples later.
            for (int block : { 1, 37, 480, 1024 })
            {
                if (c.rate != kRate && block != 37)
                    continue;

                NoiseRemoval nr;
                setMode (nr, c.mode);
                setParam (nr.apvts, "strength", 0.0f);
                nr.prepareToPlay (c.rate, 1024);
                expect (nr.waitUntilReady (30000), "ready");
                expect (nr.getActiveMode() == c.mode);

                const int latency = nr.getLatencySamples();
                const auto label = juce::String (NoiseRemoval::getModeName (c.mode)) + " at " + juce::String (c.rate) + " Hz, block " + juce::String (block);

                if (c.rate == kRate)
                    expectEquals (latency, NoiseRemoval::getNominalLatencySamples (c.mode), label + ": latency");

                std::vector<float> x ((size_t) (latency + 6000), 0.0f);
                x[1000] = 0.5f;
                const auto y = runMono (nr, x, block);

                int at = -1;
                float peak = 0.0f;

                for (int i = 0; i < (int) y.size(); ++i)
                    if (std::abs (y[(size_t) i]) > peak) { peak = std::abs (y[(size_t) i]); at = i; }

                expectEquals (at, 1000 + latency, label + ": impulse delay is the reported latency (" + juce::String (latency) + ")");
                expectEquals (peak, 0.5f);
            }
        }

        // The numbers, for the report.
        for (auto mode : { Mode::fast, Mode::best, Mode::light })
        {
            NoiseRemoval nr;
            setMode (nr, mode);
            nr.prepareToPlay (kRate, 512);
            nr.waitUntilReady (30000);
            logMessage (juce::String (NoiseRemoval::getModeName (mode)) + ": latency " + juce::String (nr.getLatencySamples()) + " samples = "
                        + juce::String (1000.0 * nr.getLatencySamples() / kRate, 1) + " ms");
        }
    }

    // The wet path (not the blend): DeepFilterNet with its attenuation limit at ~0 dB hands back the input, so the
    // engine's own delay shows up.
    void testEngineAlignment()
    {
        const Lockstep lockstep;

        for (auto which : { dfn::Model::fast, dfn::Model::best })
        {
            juce::String error;
            auto model = dfn::Models::get (which, error);
            expect (model != nullptr, "model loads: " + error);

            if (model == nullptr)
                continue;

            auto engine = denoise::DfnEngine::create (model, error);
            expect (engine != nullptr, "engine starts: " + error);

            if (engine == nullptr)
                continue;

            expectEquals (engine->latency(), 2 * 480 + model->latency());
            engine->setTestAttenuationLimitDb (0.02f);

            const int n = (int) kRate * 2;
            const auto x = makeNoise (n, 0.1, 11);                  // white noise: correlates at exactly one lag

            std::vector<float> y (x.size());

            for (int offset = 0; offset < n; offset += 480)
            {
                const float* in[1] { x.data() + offset };
                float* out[1] { y.data() + offset };
                engine->process (in, out, 1, juce::jmin (480, n - offset));
            }

            const int latency = engine->latency();
            int bestLag = -1;
            double best = -2.0;

            for (int lag = latency - 10; lag <= latency + 10; ++lag)
            {
                const double c = correlationAtLag (x, y, lag, (int) kRate, n - 1);

                if (c > best) { best = c; bestLag = lag; }
            }

            logMessage (juce::String (which == dfn::Model::fast ? "Fast" : "Best") + " wet path: latency " + juce::String (latency)
                        + ", correlation peaks at " + juce::String (bestLag) + " (r = " + juce::String (best, 5) + ")");
            expectEquals (bestLag, latency, "the network's output is the input delayed by latency()");
            expect (best > 0.95, "and it is the same signal (r = " + juce::String (best, 3) + ")");
            expectEquals ((int) engine->misses(), 0);
        }
    }

    // -----------------------------------------------------------------------------------------------
    void testLightBasics()
    {
        // Strength 0 %: dry only, exactly delayed.
        {
            NoiseRemoval nr;
            setMode (nr, Mode::light);
            setParam (nr.apvts, "strength", 0.0f);
            nr.prepareToPlay (kRate, 1024);
            const auto x = makeNoise (20000, 0.3, 71);
            const auto y = runMono (nr, x, 333);
            bool same = true;

            for (int i = nr.getLatencySamples(); i < 20000; ++i)
                same = same && y[(size_t) i] == x[(size_t) (i - nr.getLatencySamples())];

            expect (same, "strength 0 % is the input, sample for sample, delayed");
        }

        // The old tests of the RNNoise path (DspTests) cover attenuation, gate, rates and the mono optimisation; this
        // one checks the new blend: strength 50 % on noise is about the 15 dB cap it stands for.
        {
            const int n = (int) kRate * 4;
            const auto x = makeNoise (n, 0.0316 * std::sqrt (3.0), 61);
            const double inputDb = toDb (rms (x.data() + n / 2, n / 2));

            for (float strength : { 50.0f, 100.0f })
            {
                NoiseRemoval nr;
                setMode (nr, Mode::light);
                setParam (nr.apvts, "strength", strength);
                nr.prepareToPlay (kRate, 512);
                const auto y = runMono (nr, x, 256);
                const double removed = inputDb - toDb (rms (y.data() + n / 2, n / 2));
                const double cap = NoiseRemoval::attenuationDbForStrength (strength);
                logMessage ("Light, strength " + juce::String (strength, 0) + " %: noise " + juce::String (removed, 1) + " dB down (limit " + juce::String (cap, 0) + " dB)");
                expect (removed > cap - 4.0 && removed < cap + 8.0, "strength " + juce::String (strength, 0) + " % removes about its limit (" + juce::String (removed, 1) + " dB)");
            }
        }
    }

    // -----------------------------------------------------------------------------------------------
    void testStrengthOnNoise()
    {
        const Lockstep lockstep;
        const int n = (int) kRate * 4;
        const auto x = makeNoise (n, 0.0316 * std::sqrt (3.0), 62);
        const double inputDb = toDb (rms (x.data() + n / 2, n / 2));

        for (auto mode : { Mode::fast, Mode::best, Mode::light })
        {
            double previous = -1.0;

            for (float strength : { 0.0f, 40.0f, 80.0f, 100.0f })
            {
                NoiseRemoval nr;
                setMode (nr, mode);
                setParam (nr.apvts, "strength", strength);
                nr.prepareToPlay (kRate, 512);
                expect (nr.waitUntilReady (30000));
                const auto y = runMono (nr, x, 256);
                const double removed = inputDb - toDb (rms (y.data() + n / 2, n / 2));
                const double cap = NoiseRemoval::attenuationDbForStrength (strength);
                logMessage (juce::String (NoiseRemoval::getModeName (mode)) + " strength " + juce::String (strength, 0) + " %: "
                            + juce::String (removed, 1) + " dB removed, limit " + juce::String (cap, 0) + " dB");
                expect (allFinite (y.data(), n));
                expect (removed > previous - 0.5, "more strength never removes less");
                expect (removed < cap + 3.0, "never more than the limit (+3 dB): " + juce::String (removed, 1));

                if (strength == 0.0f)
                    expect (std::abs (removed) < 0.1, "strength 0 % removes nothing");
                else if (strength >= 80.0f)
                    expect (removed > cap - 6.0, "strength " + juce::String (strength, 0) + " % reaches near its limit: " + juce::String (removed, 1));

                previous = removed;
            }
        }
    }

    // -----------------------------------------------------------------------------------------------
    void testModeSwitches()
    {
        const Lockstep lockstep;
        const std::pair<Mode, Mode> switches[] { { Mode::fast, Mode::light }, { Mode::light, Mode::best },
                                                 { Mode::best, Mode::fast }, { Mode::fast, Mode::best } };

        for (const auto& [from, to] : switches)
        {
            NoiseRemoval nr;
            setMode (nr, from);
            nr.prepareToPlay (kRate, 512);
            expect (nr.waitUntilReady (30000));
            const int before = nr.getLatencySamples();

            // A 300 Hz sine, 20 s of it at most: long enough for a model to load and the switch to complete.
            const auto sine = makeSine ((int) kRate * 20, kRate, 300.0, 0.4);
            int switchedAt = -1, doneAt = -1;

            auto y = runPaced (nr, sine, 256, true, [&] (int done)
            {
                if (switchedAt < 0 && done >= (int) kRate)
                {
                    setMode (nr, to);
                    switchedAt = done;
                }

                if (switchedAt >= 0 && doneAt < 0 && nr.getActiveMode() == to && ! nr.isSwitching())
                    doneAt = done;

                return doneAt >= 0 && done > doneAt + (int) (kRate * 0.5);
            });

            expect (doneAt > 0, juce::String (NoiseRemoval::getModeName (from)) + " -> " + NoiseRemoval::getModeName (to) + ": the switch completes");
            y.resize ((size_t) juce::jmax (1, doneAt + (int) (kRate * 0.5)));
            expect (allFinite (y.data(), (int) y.size()));

            const int settled = (int) (kRate * 0.6);
            const double whole = maxSecondDifference (y, settled, (int) y.size());
            const double steady = maxSecondDifference (y, settled, switchedAt);
            const double limit = std::pow (10.0, -40.0 / 20.0);

            logMessage (juce::String (NoiseRemoval::getModeName (from)) + " -> " + NoiseRemoval::getModeName (to) + ": switch took "
                        + juce::String ((doneAt - switchedAt) * 1000.0 / kRate, 0) + " ms of audio, latency " + juce::String (before) + " -> "
                        + juce::String (nr.getLatencySamples()) + ", largest second difference " + juce::String (toDb (whole), 1)
                        + " dBFS (before the switch " + juce::String (toDb (steady), 1) + " dBFS)");
            expect (whole < limit, "no discontinuity above -40 dBFS (" + juce::String (toDb (whole), 1) + ")");
            expect (nr.getLatencySamples() == NoiseRemoval::getNominalLatencySamples (to), "the new latency is reported");
        }
    }

    // -----------------------------------------------------------------------------------------------
    void testBlockSizes()
    {
        const Lockstep lockstep;

        for (auto mode : { Mode::fast, Mode::best, Mode::light })
        {
            for (double sr : { 48000.0, 44100.0 })
            {
                const int n = (int) sr * 2;
                auto x = makeSpeech (n, 21, sr);
                const auto noise = makeRoomNoise (n, 22, sr);

                for (int i = 0; i < n; ++i)
                    x[(size_t) i] = 0.05f * x[(size_t) i] + 0.03f * noise[(size_t) i];

                std::vector<float> reference;

                for (int block : { 480, 1, 7, 100, 481, 1023, 1024, 1025 })
                {
                    NoiseRemoval nr;
                    setMode (nr, mode);
                    setParam (nr.apvts, "strength", 70.0f);
                    nr.prepareToPlay (sr, 1024);
                    nr.waitUntilReady (30000);
                    const auto y = runMono (nr, x, block);

                    expect (allFinite (y.data(), n), "finite with blocks of " + juce::String (block));
                    expectEquals ((int) nr.getUnderruns(), 0);
                    expectEquals ((int) nr.getMissedFrames(), 0);

                    if (reference.empty())
                        reference = y;
                    else if (y != reference)
                        expect (false, juce::String (NoiseRemoval::getModeName (mode)) + " at " + juce::String (sr) + " Hz: output depends on the block size ("
                                           + juce::String (block) + ")");
                }
            }

            // Every size from 1 to 1024 once, back to back, then odd rates, on live input: finite.
            for (double sr : { 48000.0, 44100.0, 96000.0, 22050.0 })
            {
                NoiseRemoval nr;
                setMode (nr, mode);
                nr.prepareToPlay (sr, 1024);
                nr.waitUntilReady (30000);
                std::mt19937 rng (5);
                bool finite = true;

                for (int size = 1; size <= 1024; ++size)
                {
                    auto a = makeNoise (size, 0.3, (uint32_t) size), b = a;

                    if (size % 3 == 0)
                        std::fill (a.begin(), a.end(), 0.0f);

                    runProcessor (nr, a.data(), b.data(), size, 0);
                    finite = finite && allFinite (a.data(), size) && allFinite (b.data(), size);
                }

                expect (finite, juce::String (NoiseRemoval::getModeName (mode)) + " at " + juce::String (sr) + " Hz: sizes 1..1024 produce finite audio");
            }
        }
    }

    // -----------------------------------------------------------------------------------------------
    void testStereo()
    {
        const Lockstep lockstep;

        for (auto mode : { Mode::fast, Mode::light })
        {
            const int n = (int) kRate * 4;
            const auto voice = makeSpeech (n, 31);
            const auto noise = makeRoomNoise (n, 32);
            std::vector<float> left (n), right (n);

            // The first second is a mono microphone (both sides identical); afterwards they differ.
            for (int i = 0; i < n; ++i)
            {
                left[(size_t) i] = 0.05f * voice[(size_t) i] + 0.02f * noise[(size_t) i];
                const float other = 0.03f * voice[(size_t) i] + 0.02f * noise[(size_t) (n - 1 - i)];
                const float apart = juce::jlimit (0.0f, 1.0f, (float) (i - (int) kRate) / (float) (kRate * 0.1));     // no step in the input itself
                right[(size_t) i] = left[(size_t) i] + apart * (other - left[(size_t) i]);
            }

            NoiseRemoval nr;
            setMode (nr, mode);
            nr.prepareToPlay (kRate, 1024);
            expect (nr.waitUntilReady (30000));

            auto l = left, r = right;
            runProcessor (nr, l.data(), r.data(), n, 300);

            expect (allFinite (l.data(), n) && allFinite (r.data(), n));

            bool same = true;
            for (int i = 0; i < (int) kRate - 300; ++i)
                same = same && l[(size_t) i] == r[(size_t) i];

            expect (same, "identical channels stay identical");

            // After the start of the second lane nothing jumps: the largest step in the right channel around the moment
            // it starts (the channels begin to differ at 1 s) is no larger than elsewhere, and its level is sensible.
            const double around = maxSecondDifference (r, (int) kRate - 200, (int) kRate + 9600);
            const double ownRoughness = maxSecondDifference (right, (int) kRate - 200, (int) kRate + 9600);
            logMessage (juce::String (NoiseRemoval::getModeName (mode)) + ": right channel second difference at lane start " + juce::String (toDb (around), 1)
                        + " dBFS, its own input " + juce::String (toDb (ownRoughness), 1) + " dBFS");
            // Until its network has warmed up the right channel plays its own (uncleaned) input: nothing may jump above that.
            expect (around < 1.3 * ownRoughness, "the second lane starts without a click");
            expect (rms (r.data() + n / 2, n / 2) > 0.001, "the right channel has sound");
        }
    }

    // -----------------------------------------------------------------------------------------------
    static inline std::atomic<int> promotedTo { 0 };

    void testWorkerThread()
    {
        juce::String error;
        auto model = dfn::Models::get (dfn::Model::best, error);
        expect (model != nullptr, error);

        if (model == nullptr)
            return;

        // The backend can give the worker realtime priority (PipeWire's rtkit on Linux): the hook is called on the worker.
        promotedTo.store (0);
        denoise::setWorkerThreadPromoter ([] (int priority) noexcept { promotedTo.store (priority); return true; });

        {
            auto engine = denoise::DfnEngine::create (model, error);
            expect (engine != nullptr, error);

            for (int i = 0; i < 400 && promotedTo.load() == 0; ++i)
                juce::Thread::sleep (5);

            expect (promotedTo.load() > 0, "the worker asked for realtime priority");
            expect (engine->describeWorkerThread().contains ("realtime priority"), engine->describeWorkerThread());

            // Shutting down does not wait for anything: an idle engine is gone in well under a second.
            const auto t0 = juce::Time::getMillisecondCounter();
            engine.reset();
            expect (juce::Time::getMillisecondCounter() - t0 < 1000, "the worker stops promptly");
        }

        denoise::setWorkerThreadPromoter (nullptr);
    }

    // -----------------------------------------------------------------------------------------------
    void testMissedFrames()
    {
        juce::String error;
        auto model = dfn::Models::get (dfn::Model::fast, error);
        expect (model != nullptr, error);

        if (model == nullptr)
            return;

        auto engine = denoise::DfnEngine::create (model, error);
        expect (engine != nullptr, error);

        if (engine == nullptr)
            return;

        const int latency = engine->latency();
        const auto x = makeNoise ((int) kRate * 6, 0.05, 41);      // the network removes it: its output differs from the input
        std::vector<float> y (x.size());
        int done = 0;

        auto run = [&] (int seconds)
        {
            const int stop = juce::jmin ((int) x.size(), done + (int) (kRate * seconds));

            while (done < stop)
            {
                const int n = juce::jmin (480, stop - done);
                const float* in[1] { x.data() + done };
                float* out[1] { y.data() + done };
                engine->process (in, out, 1, n);
                done += n;
                juce::Thread::sleep (n * 1000 / (int) kRate);           // real time, so the worker's answers are due when they are due
            }
        };

        // A worker that needs 30 ms for a 10 ms frame is late for every frame: the input plays in place, exactly aligned.
        engine->setTestWorkerDelayMicroseconds (30000);
        run (2);
        const auto missesLate = engine->misses();
        logMessage ("worker 30 ms late per frame: " + juce::String ((int) missesLate) + " missed frames of about 200, "
                    + juce::String ((int) engine->overruns()) + " frames could not even be queued");
        expect (missesLate > 100, "late frames are counted");

        // Once the answers come back on time again the misses stop and the network's output plays.
        engine->setTestWorkerDelayMicroseconds (0);
        run (2);                                                        // catches up on the backlog
        const auto missesBefore = engine->misses();
        const int from = done;
        run (2);
        expectEquals ((int) engine->misses(), (int) missesBefore, "no more misses once the worker is on time");
        expect (allFinite (y.data(), done));

        // While every frame was missed the output is the input, delayed by latency(), sample for sample.
        int exact = 0, compared = 0;

        for (int i = latency + 4800; i < (int) (kRate * 1.5); ++i)
        {
            ++compared;
            exact += y[(size_t) i] == x[(size_t) (i - latency)] ? 1 : 0;
        }

        logMessage ("while missing frames: " + juce::String (exact) + " of " + juce::String (compared) + " samples are exactly the delayed input");
        expect (exact > compared * 9 / 10, "the delayed input plays in place of a missing answer");

        // And later it is the network: it differs from the input (it removes the noise-free silences to zero, for one).
        int differs = 0;

        for (int i = from + 2400; i < done; ++i)
            differs += y[(size_t) i] != x[(size_t) (i - latency)] ? 1 : 0;

        expect (differs > (done - from) / 4, "the network's output plays again");
    }

    // -----------------------------------------------------------------------------------------------
    void testHearRemovedAndTaps()
    {
        const int n = (int) kRate * 3;
        auto x = makeSpeech (n, 51);
        const auto noise = makeRoomNoise (n, 52);

        for (int i = 0; i < n; ++i)
            x[(size_t) i] = 0.05f * x[(size_t) i] + 0.02f * noise[(size_t) i];

        // Light is deterministic: two instances see the same thing. One plays the cleaned voice, one the removed part.
        NoiseRemoval normal, hearing;
        setMode (normal, Mode::light);
        setMode (hearing, Mode::light);
        normal.prepareToPlay (kRate, 512);
        hearing.prepareToPlay (kRate, 512);
        hearing.setHearRemoved (true);

        const auto cleaned = runMono (normal, x, 256);
        const auto removed = runMono (hearing, x, 256);
        const int latency = normal.getLatencySamples();

        double worst = 0.0;

        for (int i = latency + 100; i < n; ++i)
            worst = juce::jmax (worst, (double) std::abs (cleaned[(size_t) i] + removed[(size_t) i] - x[(size_t) (i - latency)]));

        logMessage ("cleaned + removed = input, aligned: largest difference " + juce::String (toDb (worst), 1) + " dBFS");
        expect (worst < 1.0e-5, "the removed part is exactly the input minus the output");
        expect (rms (removed.data() + n / 2, n / 2) > 1.0e-4, "there is something to hear");

        // Switching it on and off is a 20 ms fade, not a click.
        {
            NoiseRemoval nr;
            setMode (nr, Mode::light);
            nr.prepareToPlay (kRate, 512);
            int step = 0;
            auto y = runPaced (nr, x, 256, false, [&] (int done)
            {
                if (done >= (int) kRate && step == 0) { nr.setHearRemoved (true); step = 1; }
                if (done >= (int) kRate * 2 && step == 1) { nr.setHearRemoved (false); step = 2; }
                return false;
            });

            // The removed part is noise-like and rougher than the cleaned voice, so the yardstick is the roughness of each
            // of the two signals themselves: the fade between them must not be worse than the rougher one.
            const double cleanedRough = maxSecondDifference (y, (int) kRate / 2, (int) kRate - 300);
            const double removedRough = maxSecondDifference (y, (int) kRate + 4800, (int) kRate * 2 - 4800);
            const double yardstick = juce::jmax (cleanedRough, removedRough);
            const double atStart = maxSecondDifference (y, (int) kRate - 300, (int) kRate + 3000);
            const double atEnd = maxSecondDifference (y, (int) kRate * 2 - 300, (int) kRate * 2 + 3000);
            logMessage ("hear on/off: second difference " + juce::String (toDb (atStart), 1) + " / " + juce::String (toDb (atEnd)) + " dBFS at the two changes; cleaned "
                        + juce::String (toDb (cleanedRough), 1) + ", removed " + juce::String (toDb (removedRough), 1) + " dBFS");
            expect (atStart < 1.3 * yardstick, "no click when it starts");
            expect (atEnd < 1.3 * yardstick, "no click when it ends");
        }

        // Callbacks: requesting it asks the app first and then switches the effect.
        {
            NoiseRemoval nr;
            std::vector<bool> asked;
            nr.onHearRemovedRequest = [&] (bool on) { asked.push_back (on); };
            nr.requestHearRemoved (true);
            expect (nr.isHearingRemoved() && asked.size() == 1 && asked[0]);
            nr.requestHearRemoved (true);
            expectEquals ((int) asked.size(), 1, "asking twice asks once");
            nr.requestHearRemoved (false);
            expect (! nr.isHearingRemoved() && asked.size() == 2 && ! asked[1]);

            nr.requestHearRemoved (true);
            nr.onHearRemovedRequest = [&] (bool on) { asked.push_back (on); };
        }

        // The spectrum tap writes nothing while nobody looks.
        {
            NoiseRemoval nr;
            setMode (nr, Mode::light);
            nr.prepareToPlay (kRate, 512);
            auto& tap = nr.getSpectrumTap();
            expect (! tap.active.load());
            runMono (nr, x, 256);
            expectEquals ((int) tap.written(), 0, "display tap off: nothing is written");

            tap.active.store (true);
            runMono (nr, x, 256);
            expectEquals ((int) tap.written(), n, "display tap on: every sample");

            std::vector<float> in (4096), out (4096);
            expect (tap.read (in.data(), out.data(), 4096));

            // The tap's input is the aligned input: it equals the input delayed by the latency, the window ends kSlack before the end.
            const int end = n - (int) denoise::SpectrumTap::kSlack;
            bool aligned = true;

            for (int i = 0; i < 4096; ++i)
                aligned = aligned && in[(size_t) i] == x[(size_t) (end - 4096 + i - latency)];

            expect (aligned, "the tap holds the input aligned with the output");

            tap.active.store (false);
            const auto written = tap.written();
            runMono (nr, x, 256);
            expectEquals ((int) tap.written(), (int) written, "and stops again");
        }

        // Meters: after speech in a noisy room the panel has numbers, in every mode (Best is the default).
        for (auto meterMode : { Mode::light, Mode::fast, Mode::best })
        {
            NoiseRemoval nr;
            setMode (nr, meterMode);
            nr.prepareToPlay (kRate, 512);
            expect (nr.waitUntilReady (30000), "ready");
            const int m = (int) kRate * 8;
            auto voice = makeSpeech (m, 53);
            const auto room = makeRoomNoise (m, 54);

            for (int i = 0; i < m; ++i)
                voice[(size_t) i] = 0.1f * voice[(size_t) i] + 0.003f * room[(size_t) i];

            bool sawVoice = false, sawSilence = false;
            runPaced (nr, voice, 480, meterMode != Mode::light, [&] (int)          // DeepFilterNet needs audio at (about) real time
            {
                sawVoice = sawVoice || nr.isVoiceActive();
                sawSilence = sawSilence || ! nr.isVoiceActive();
                return false;
            });

            logMessage (juce::String ("meters (") + NoiseRemoval::getModeName (meterMode) + "): voice " + juce::String (nr.getVoiceLevelDb(), 1) + " dBFS, room " + juce::String (nr.getNoiseFloorDb(), 1)
                        + " dBFS, removed " + juce::String (nr.getReductionDb(), 1) + " dB");
            expect (sawVoice && sawSilence, "speaking switches on and off");
            expect (nr.getVoiceLevelDb() > nr.getNoiseFloorDb() + 15.0f, "voice is well above the room");
            expect (nr.getNoiseFloorDb() > -90.0f && nr.getNoiseFloorDb() < -30.0f, "room noise is measured");
            expect (nr.getReductionDb() > 6.0f, "noise removed is measured");
        }
    }

    // -----------------------------------------------------------------------------------------------
    void testControllerHearRemoved()
    {
        AppController::Options options;
        options.demo = true;
        options.persist = false;
        options.startBackend = false;

        AppController controller (options);
        controller.start();
        controller.applyMicPreset ("off");             // no effects: the mic's routing is all that is tested here

        auto& engine = controller.getEngine();
        constexpr int frames = 2048;

        // The mic carries DC 0.1; what comes out of each of the three outputs after the smoothers settle:
        auto levels = [&]
        {
            std::vector<float> mic (frames, 0.1f), monL (frames), monR (frames), strL (frames), strR (frames), outL (frames), outR (frames);
            EngineIO io;
            io.numFrames = frames;
            io.sampleRate = kRate;
            io.micIn[0] = mic.data();
            io.monitorOut[0] = monL.data();  io.monitorOut[1] = monR.data();
            io.streamOut[0] = strL.data();   io.streamOut[1] = strR.data();
            io.micOut[0] = outL.data();      io.micOut[1] = outR.data();

            for (int i = 0; i < 4; ++i)
                engine.process (io);

            return std::array<float, 3> { monL.back(), strL.back(), outL.back() };
        };

        auto before = levels();
        expectWithinAbsoluteError (before[0], 0.0f, 1.0e-6f);          // Headphones: -inf by default
        expectWithinAbsoluteError (before[1], 0.1f, 1.0e-3f);          // Stream: 0 dB
        expectWithinAbsoluteError (before[2], 0.1f, 1.0e-3f);          // "Headroom Mic" for Discord

        controller.setHearingRemoved (true);
        auto during = levels();
        expect (controller.isHearingRemoved());
        expectWithinAbsoluteError (during[0], 0.1f, 1.0e-3f);          // 0 dB, unmuted, in the headphones
        expectWithinAbsoluteError (during[1], 0.0f, 1.0e-6f);          // not in the stream
        expectWithinAbsoluteError (during[2], 0.0f, 1.0e-6f);          // not to Discord

        // Other changes to the mic while listening do not undo it ...
        controller.setChannelLevel ("mic", Mix::Monitor, -20.0f);
        auto meanwhile = levels();
        expectWithinAbsoluteError (meanwhile[0], 0.1f, 1.0e-3f);
        expectWithinAbsoluteError (meanwhile[2], 0.0f, 1.0e-6f);

        // ... and letting go puts every setting back, the one changed meanwhile included.
        controller.setHearingRemoved (false);
        auto after = levels();
        expect (! controller.isHearingRemoved());
        expectWithinAbsoluteError (after[0], 0.01f, 1.0e-3f);          // -20 dB
        expectWithinAbsoluteError (after[1], 0.1f, 1.0e-3f);
        expectWithinAbsoluteError (after[2], 0.1f, 1.0e-3f);

        // The saved model was never touched.
        const auto mic = AppState::describe (controller.getState().findChannel ("mic"));
        expectWithinAbsoluteError (mic.monitorDb, -20.0f, 1.0e-4f);
        expect (! mic.monitorMuted && ! mic.streamMuted);
    }

    // -----------------------------------------------------------------------------------------------
    void testNoAllocation()
    {
        const Lockstep lockstep;
        const int n = (int) kRate * 6;
        auto x = makeSpeech (n, 61);
        const auto noise = makeRoomNoise (n, 62);

        for (int i = 0; i < n; ++i)
            x[(size_t) i] = 0.05f * x[(size_t) i] + 0.02f * noise[(size_t) i];

        for (auto mode : { Mode::fast, Mode::light })
        {
            NoiseRemoval nr;
            setMode (nr, mode);
            nr.prepareToPlay (kRate, 512);
            nr.waitUntilReady (30000);

            // Warm up (first blocks touch thread-locals and the like), then count on this thread only.
            runMono (nr, std::vector<float> (x.begin(), x.begin() + (int) kRate), 256);

            const auto target = mode == Mode::fast ? Mode::light : Mode::fast;
            std::vector<float> l (x.begin() + (int) kRate, x.end()), r = l;
            juce::MidiBuffer midi;
            int newCount = -1, mallocCount = -1;
            std::atomic<bool> go { false };
            std::thread messageThread ([&] { while (! go.load()) std::this_thread::yield(); setMode (nr, target); });
            int blocks = 0;

            {
                ScopedNoAllocation newCalls;
                MallocCounter::ScopedThread mallocCalls;

                const auto started = juce::Time::getMillisecondCounter();
                const int usable = ((int) l.size() / 256) * 256;

                // Up to 20 s of wall-clock time: the loader thread needs as long as it needs to get a model in.
                for (int offset = 0; juce::Time::getMillisecondCounter() - started < 20000; offset = (offset + 256) % usable, ++blocks)
                {
                    float* ch[2] { l.data() + offset, r.data() + offset };
                    juce::AudioBuffer<float> buffer (ch, 2, 256);
                    nr.processBlock (buffer, midi);

                    // Ask for the other engine from another thread, as the message thread does; then keep processing until
                    // it has been built, adopted, cross-faded in and the old one retired: all of that on this thread.
                    if (blocks == 100)
                        go.store (true);

                    if (blocks > 100 && nr.getActiveMode() == target && ! nr.isSwitching())
                        break;

                    juce::Thread::sleep (2);
                }

                newCount = newCalls.allocations();
                mallocCount = mallocCalls.allocations();
            }

            expectEquals (newCount, 0, juce::String (NoiseRemoval::getModeName (mode)) + ": operator new on the audio thread");

            if (MallocCounter::isSupported())
                expectEquals (mallocCount, 0, juce::String (NoiseRemoval::getModeName (mode)) + ": malloc on the audio thread");

            if (messageThread.joinable())
                messageThread.join();

            expect (nr.getActiveMode() == target, juce::String (NoiseRemoval::getModeName (mode)) + ": the switch was carried out inside the measured stretch");
            logMessage (juce::String (NoiseRemoval::getModeName (mode)) + " -> " + NoiseRemoval::getModeName (target) + ": " + juce::String (blocks)
                        + " blocks on the audio thread, no allocation");
        }

        // For the record: what one DeepFilterNet frame costs in heap allocations (on the worker thread, where it belongs).
        if (MallocCounter::isSupported())
        {
            juce::String error;
            auto model = dfn::Models::get (dfn::Model::fast, error);

            if (model != nullptr)
            {
                auto state = model->createState();
                std::vector<float> in (480), out (480);
                auto noise2 = makeNoise (480 * 400, 0.05, 63);

                for (int f = 0; f < 200; ++f)
                    state->process (noise2.data() + f * 480, out.data());

                MallocCounter::startGlobal();

                for (int f = 200; f < 400; ++f)
                    state->process (noise2.data() + f * 480, out.data());

                const auto calls = MallocCounter::stopGlobal();
                logMessage ("DeepFilterNet (Fast): " + juce::String ((double) calls / 200.0, 1) + " heap allocations per frame in steady state, all on the worker thread");
                expect (calls > 0, "(this is why the network runs on a worker thread and not inside processBlock)");
            }
        }
    }

    // -----------------------------------------------------------------------------------------------
    void testQuality()
    {
        const Lockstep lockstep;
        const int n = (int) kRate * 10;
        const auto speech = makeSpeech (n, 71);
        const auto room = makeRoomNoise (n, 72);

        const float speechLevel = 0.05f;                                    // -26 dBFS
        const float noiseLevel = speechLevel * (float) std::pow (10.0, -5.0 / 20.0);
        std::vector<float> mix ((size_t) n), reference ((size_t) n);

        for (int i = 0; i < n; ++i)
        {
            mix[(size_t) i] = speechLevel * speech[(size_t) i] + noiseLevel * room[(size_t) i];
            reference[(size_t) i] = speechLevel * speech[(size_t) i];
        }

        const int from = (int) kRate, to = n - 6000;
        const double inputSnr = snrDb (mix, reference, 0, from, to);
        logMessage ("input: " + juce::String (inputSnr, 1) + " dB signal-to-noise");

        double result[3] {};

        for (auto mode : { Mode::fast, Mode::best, Mode::light })
        {
            NoiseRemoval nr;
            setMode (nr, mode);
            setParam (nr.apvts, "strength", 100.0f);
            nr.prepareToPlay (kRate, 512);
            expect (nr.waitUntilReady (30000));

            const auto y = runMono (nr, mix, 256);
            const double out = snrDb (y, reference, nr.getLatencySamples(), from, to);
            result[(int) mode] = out;

            {
                // Where does the noise that is left sit: in the gaps between phrases, or under the speech?
                const int lag = nr.getLatencySamples();
                double gapIn = 0.0, gapOut = 0.0, speechIn = 0.0, speechOut = 0.0, gain = 0.0, sumSS = 0.0;
                int gaps = 0, voiced = 0;
                std::vector<char> active ((size_t) n, 0);

                for (int i = 0; i < n; ++i)
                    if (std::abs (speech[(size_t) i]) > 0.05f)
                        for (int k = juce::jmax (0, i - 2400); k < juce::jmin (n, i + 2400); ++k)
                            active[(size_t) k] = 1;

                double ys = 0.0;
                for (int i = from; i < to; ++i) { ys += (double) y[(size_t) (i + lag)] * reference[(size_t) i]; sumSS += (double) reference[(size_t) i] * reference[(size_t) i]; }
                gain = ys / sumSS;

                for (int i = from; i < to; ++i)
                {
                    const double noiseIn = (double) mix[(size_t) i] - (double) reference[(size_t) i];
                    const double residual = (double) y[(size_t) (i + lag)] - gain * (double) reference[(size_t) i];

                    if (active[(size_t) i]) { speechIn += noiseIn * noiseIn; speechOut += residual * residual; ++voiced; }
                    else                    { gapIn += noiseIn * noiseIn; gapOut += residual * residual; ++gaps; }
                }

                logMessage (juce::String (NoiseRemoval::getModeName (mode)) + ": speech gain " + juce::String (toDb (gain), 1) + " dB; noise in gaps "
                            + juce::String (10.0 * std::log10 (gapOut / juce::jmax (1.0e-30, gapIn)), 1) + " dB, under speech "
                            + juce::String (10.0 * std::log10 (speechOut / juce::jmax (1.0e-30, speechIn)), 1) + " dB (gaps are "
                            + juce::String (100.0 * gaps / juce::jmax (1, gaps + voiced), 0) + " % of the time)");
            }

            logMessage (juce::String (NoiseRemoval::getModeName (mode)) + ": " + juce::String (out, 1) + " dB signal-to-noise, "
                        + juce::String (out - inputSnr, 1) + " dB better");
            // RNNoise removes noise between phrases and words but hardly any under the voice; the two DeepFilterNet modes do
            // both, which is what "at least 10 dB" asks for. (This signal is a stand-in for speech, on purpose.)
            const double required = mode == Mode::light ? 5.0 : 10.0;
            expect (out - inputSnr >= required, juce::String (NoiseRemoval::getModeName (mode)) + " improves the ratio by at least " + juce::String (required, 0)
                                                   + " dB (" + juce::String (out - inputSnr, 1) + ")");
        }

        expect (result[(int) Mode::fast] > result[(int) Mode::light], "Fast beats Light");
        expect (result[(int) Mode::best] > result[(int) Mode::light], "Best beats Light");
    }

    // -----------------------------------------------------------------------------------------------
    void testCpu()
    {
        // Real time, mono mic in 256-frame blocks: the audio thread's share, the worker's share and its worst frame.
        for (auto mode : { Mode::fast, Mode::best, Mode::light })
        {
            NoiseRemoval nr;
            setMode (nr, mode);
            nr.prepareToPlay (kRate, 512);
            expect (nr.waitUntilReady (30000));

            const int n = (int) kRate * 6;
            auto x = makeSpeech (n, 81);
            const auto room = makeRoomNoise (n, 82);

            for (int i = 0; i < n; ++i)
                x[(size_t) i] = 0.05f * x[(size_t) i] + 0.02f * room[(size_t) i];

            float audioShare = 0.0f;
            runPaced (nr, x, 256, true, [&] (int done)
            {
                if (done > n / 2)
                    audioShare = juce::jmax (audioShare, nr.getCpuLoad());

                return false;
            });

            logMessage (juce::String (NoiseRemoval::getModeName (mode)) + ": " + juce::String (100.0 * nr.getCpuLoad(), 1) + " % of one core (audio thread + network thread), worst frame "
                        + juce::String (nr.getWorstFrameMs(), 2) + " ms, " + juce::String ((int) nr.getMissedFrames()) + " missed frames of "
                        + juce::String (n / 480));
            expect (nr.getCpuLoad() < 0.5f, "well below one core");
            expectEquals ((int) nr.getUnderruns(), 0);
        }
    }
};

static NoiseRemovalTests noiseRemovalTests;
}
