// Unit tests for what the compressor panel stands on: the display tap, the shared gain computer, the Amount macro and its
// Custom flag, their state round trip, and the panel's data flow and paint cost.
#include "Juce.h"

#include "AllocationGuard.h"
#include "TestHelpers.h"
#include "engine/RtUtils.h"
#include "engine/builtin/BuiltinFx.h"
#include "engine/builtin/Compressor.h"
#include "ui/fx/CompPanel.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

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

    float raw (Compressor& c, const char* id)   { return c.apvts.getRawParameterValue (id)->load(); }

    // AudioProcessor's XML <-> binary helpers are protected statics; this gives the tests a way in.
    struct StateAccess : juce::AudioProcessor
    {
        static std::unique_ptr<juce::XmlElement> unpack (const void* data, int size)    { return getXmlFromBinary (data, size); }
        static void pack (const juce::XmlElement& xml, juce::MemoryBlock& block)        { copyXmlToBinary (xml, block); }
    };

    // 5 s of a 200 Hz tone at `levelDb`, in blocks of 256, through `comp`; the tap is drained after every block.
    void hammer (Compressor& comp, float levelDb, double seconds, std::vector<Compressor::DisplayRecord>& out)
    {
        const int n = (int) (kRate * seconds);
        auto x = makeSine (n, kRate, 200.0, dbToLinear (levelDb));
        auto y = x;
        juce::MidiBuffer midi;
        Compressor::DisplayRecord buffer[64];

        for (int offset = 0; offset < n; offset += 256)
        {
            const int count = juce::jmin (256, n - offset);
            float* channels[2] { x.data() + offset, y.data() + offset };
            juce::AudioBuffer<float> block (channels, 2, count);
            comp.processBlock (block, midi);

            for (int got; (got = comp.drainDisplay (buffer, 64)) > 0;)
                out.insert (out.end(), buffer, buffer + got);
        }
    }
}

class CompressorDisplayTests final : public juce::UnitTest
{
public:
    CompressorDisplayTests() : juce::UnitTest ("Compressor display", "headroom") {}

    void runTest() override
    {
        beginTest ("tap off: nothing is written, nothing is measured");
        testTapOff();

        beginTest ("tap on: one record per 5 ms with input, output and gain reduction");
        testTapOn();

        beginTest ("tap: a full ring drops records and never blocks; a reader on another thread sees them in order");
        testRing();

        beginTest ("tap: no allocation on the audio thread, on or off");
        testNoAllocation();

        beginTest ("static gain computer: known points, and it is what the DSP does");
        testGainComputer();

        beginTest ("Amount: endpoints, the default, and a smooth voice curve");
        testAmountMapping();

        beginTest ("Amount: moving it writes the details; editing a detail says Custom");
        testCustomFlag();

        beginTest ("Amount and Custom survive a state round trip; old states migrate");
        testState();

        beginTest ("panel: follows the tap only while it is on screen, and shows what the compressor does");
        testPanelDataFlow();

        beginTest ("panel: frame cost (tick + repaint of the parts that move)");
        testPanelCost();
    }

private:
    // -----------------------------------------------------------------------------------------------
    void testTapOff()
    {
        Compressor comp;
        comp.prepareToPlay (kRate, 512);
        expect (! comp.displayActive.load());

        auto x = makeSine ((int) kRate, kRate, 440.0, dbToLinear (-10.0f));
        runMono (comp, x, 256);

        expectEquals ((int) comp.getDisplayRecordsWritten(), 0);
        expectEquals ((int) comp.getDisplayRecordsDropped(), 0);
        Compressor::DisplayRecord record;
        expectEquals (comp.drainDisplay (&record, 1), 0);
        expect (comp.getDetectorLevelDb() <= -99.0f, "the detector level is only kept while the tap is on");

        // The same block through a tap that is on gives the identical audio: the tap only watches.
        Compressor a, b;
        a.prepareToPlay (kRate, 512);
        b.prepareToPlay (kRate, 512);
        b.displayActive = true;
        const auto ya = runMono (a, x, 256), yb = runMono (b, x, 256);
        expect (ya == yb, "the audio is bit-identical with the tap on or off");
    }

    void testTapOn()
    {
        Compressor comp;
        comp.prepareToPlay (kRate, 512);
        setParam (comp.apvts, "threshold", -20.0f);
        setParam (comp.apvts, "ratio", 4.0f);
        setParam (comp.apvts, "knee", 0.0f);
        setParam (comp.apvts, "makeup", 3.0f);
        comp.displayActive = true;

        std::vector<Compressor::DisplayRecord> records;
        hammer (comp, -10.0f, 2.0, records);

        expect (records.size() >= 395 && records.size() <= 405, "about 200 records per second (" + juce::String ((int) records.size()) + " in 2 s)");
        expectEquals ((int) comp.getDisplayRecordsDropped(), 0);

        const auto& last = records.back();
        logMessage ("tap record after 2 s: in " + juce::String (last.inputPeakDb, 2) + " dB, out " + juce::String (last.outputPeakDb, 2)
                    + " dB, reduction " + juce::String (last.gainReductionDb, 2) + " dB; detector " + juce::String (comp.getDetectorLevelDb(), 2));
        expect (std::abs (last.inputPeakDb + 10.0f) < 0.3f, "input peak is the tone's level");
        expect (std::abs (last.gainReductionDb + 7.5f) < 0.4f, "reduction is 7.5 dB (10 dB over, 4:1)");
        expect (std::abs (last.outputPeakDb - (last.inputPeakDb + last.gainReductionDb + 3.0f)) < 0.3f, "output = input - reduction + makeup");
        expect (std::abs (comp.getDetectorLevelDb() + 10.0f) < 0.3f, "the detector level follows the input");

        // Another rate and odd block sizes: the cadence is still 5 ms.
        Compressor other;
        other.prepareToPlay (44100.0, 512);
        other.displayActive = true;
        auto x = makeSine (44100, 44100.0, 300.0, 0.1);
        juce::MidiBuffer midi;

        for (int offset = 0; offset < 44100; offset += 137)
        {
            const int n = juce::jmin (137, 44100 - offset);
            std::vector<float> l (x.begin() + offset, x.begin() + offset + n), r = l;
            float* channels[2] { l.data(), r.data() };
            juce::AudioBuffer<float> block (channels, 2, n);
            other.processBlock (block, midi);
        }

        const int perSecond = (int) other.getDisplayRecordsWritten();
        expect (perSecond >= 198 && perSecond <= 202, "44.1 kHz in 137-frame blocks: " + juce::String (perSecond) + " records in 1 s");

        // Turning it off stops the records at once, and turning it on again starts a fresh window.
        const auto before = comp.getDisplayRecordsWritten();
        comp.displayActive = false;
        std::vector<Compressor::DisplayRecord> none;
        hammer (comp, -10.0f, 0.5, none);
        expectEquals ((int) comp.getDisplayRecordsWritten(), (int) before);
    }

    void testRing()
    {
        // Nobody reads: the ring fills, the rest is dropped, and processing simply carries on.
        {
            Compressor comp;
            comp.prepareToPlay (kRate, 512);
            comp.displayActive = true;
            auto x = makeSine ((int) kRate * 10, kRate, 200.0, 0.3);
            const auto start = juce::Time::getMillisecondCounterHiRes();
            runMono (comp, x, 256);
            const auto elapsed = juce::Time::getMillisecondCounterHiRes() - start;

            expectEquals ((int) comp.getDisplayRecordsWritten(), Compressor::kDisplayRingSize);
            expect (comp.getDisplayRecordsDropped() > 900, "the surplus was dropped (" + juce::String ((int) comp.getDisplayRecordsDropped()) + ")");
            expect (elapsed < 2000.0, "10 s of audio with a full ring took " + juce::String (elapsed, 0) + " ms");

            std::vector<Compressor::DisplayRecord> all (Compressor::kDisplayRingSize + 8);
            const int got = comp.drainDisplay (all.data(), (int) all.size());
            expectEquals (got, Compressor::kDisplayRingSize);
            expectEquals (comp.drainDisplay (all.data(), 1), 0);

            // Space again: writing resumes.
            runMono (comp, makeSine (4800, kRate, 200.0, 0.3), 256);
            expect (comp.drainDisplay (all.data(), 8) > 0, "records flow again once the reader has caught up");

            comp.clearDisplay();
            expectEquals (comp.drainDisplay (all.data(), 1), 0);
        }

        // A real reader on another thread, a real writer here: whatever arrives is in order and intact. The level ramps up
        // 60 dB over the run, so the input peak of the records must never go backwards.
        {
            Compressor comp;
            comp.prepareToPlay (kRate, 512);
            comp.displayActive = true;

            const int n = (int) kRate * 20;
            std::vector<float> x ((size_t) n);

            for (int i = 0; i < n; ++i)
                x[(size_t) i] = (float) (std::pow (10.0, (-60.0 + 60.0 * (double) i / (double) n) / 20.0) * std::sin (2.0 * kPi * 1000.0 * (double) i / kRate));

            std::atomic<bool> done { false };
            std::vector<float> seen;
            std::thread reader ([&]
            {
                Compressor::DisplayRecord buffer[32];

                while (! done.load())
                {
                    const int got = comp.drainDisplay (buffer, 32);

                    for (int i = 0; i < got; ++i)
                        seen.push_back (buffer[i].inputPeakDb);

                    std::this_thread::sleep_for (std::chrono::microseconds (200));
                }

                for (int got; (got = comp.drainDisplay (buffer, 32)) > 0;)
                    for (int i = 0; i < got; ++i)
                        seen.push_back (buffer[i].inputPeakDb);
            });

            runMono (comp, x, 128);
            done = true;
            reader.join();

            bool ordered = ! seen.empty(), finite = true;

            for (size_t i = 1; i < seen.size(); ++i)
                ordered = ordered && seen[i] >= seen[i - 1] - 0.5f;

            for (float v : seen)
                finite = finite && std::isfinite (v) && v <= 0.5f;

            expect (ordered && finite, "records arrive in order and untorn (" + juce::String ((int) seen.size()) + " of "
                                           + juce::String ((int) comp.getDisplayRecordsWritten()) + " written)");
            expect (comp.getDisplayRecordsWritten() + comp.getDisplayRecordsDropped() >= 3900, "about 4000 windows in 20 s");
        }
    }

    void testNoAllocation()
    {
        for (const bool tapOn : { false, true })
        {
            Compressor comp;
            comp.prepareToPlay (kRate, 512);
            comp.displayActive = tapOn;

            std::vector<float> l (512, 0.2f), r (512, 0.1f);
            float* channels[2] { l.data(), r.data() };
            juce::AudioBuffer<float> block (channels, 2, 512);
            juce::MidiBuffer midi;
            comp.processBlock (block, midi);

            ScopedNoAllocation guard;

            for (int i = 0; i < 200; ++i)
            {
                std::fill (l.begin(), l.end(), 0.2f);
                std::fill (r.begin(), r.end(), 0.1f);
                comp.processBlock (block, midi);
            }

            const int allocations = guard.allocations();        // read before the message below is built
            expectEquals (allocations, 0, juce::String ("no allocations with the tap ") + (tapOn ? "on" : "off"));
        }
    }

    // -----------------------------------------------------------------------------------------------
    void testGainComputer()
    {
        // Known points.
        expectWithinAbsoluteError (Compressor::reductionDb (-10.0f, -20.0f, 4.0f, 0.0f), 7.5f, 1.0e-4f);
        expectWithinAbsoluteError (Compressor::outputDb (-10.0f, -20.0f, 4.0f, 0.0f, 0.0f), -17.5f, 1.0e-4f);
        expectWithinAbsoluteError (Compressor::outputDb (-10.0f, -20.0f, 4.0f, 0.0f, 4.0f), -13.5f, 1.0e-4f);
        expectWithinAbsoluteError (Compressor::reductionDb (-20.0f, -20.0f, 4.0f, 6.0f), 0.5625f, 1.0e-4f);   // (1 - 1/R) W / 8
        expectEquals (Compressor::reductionDb (-30.0f, -20.0f, 4.0f, 6.0f), 0.0f);
        expectEquals (Compressor::reductionDb (-25.0f, -20.0f, 4.0f, 0.0f), 0.0f);
        expectEquals (Compressor::reductionDb (-5.0f, -20.0f, 1.0f, 6.0f), 0.0f);
        expectEquals (Compressor::reductionDb (-5.0f, -20.0f, 0.2f, 0.0f), 0.0f);                              // ratio < 1 counts as 1

        // What the DSP does in steady state, tone by tone.
        struct Setting { float threshold, ratio, knee, makeup; };
        const Setting settings[] { { -20.0f, 4.0f, 0.0f, 0.0f }, { -24.0f, 6.0f, 6.0f, 2.0f }, { -18.0f, 2.0f, 12.0f, 3.0f }, { -30.0f, 20.0f, 3.0f, 0.0f } };
        double worst = 0.0;

        for (const auto& s : settings)
        {
            for (const float level : { -50.0f, -35.0f, -28.0f, -22.0f, -20.0f, -16.0f, -10.0f, -4.0f })
            {
                Compressor comp;
                comp.prepareToPlay (kRate, 512);
                setParam (comp.apvts, "threshold", s.threshold);
                setParam (comp.apvts, "ratio", s.ratio);
                setParam (comp.apvts, "knee", s.knee);
                setParam (comp.apvts, "makeup", s.makeup);
                setParam (comp.apvts, "attack", 1.0f);
                setParam (comp.apvts, "release", 100.0f);

                const int n = (int) kRate * 2;
                const auto y = runMono (comp, makeSine (n, kRate, 1000.0, dbToLinear (level)), 256);
                const double measured = toneDb (y.data() + n / 2, n / 2, kRate, 1000.0);
                const double predicted = Compressor::outputDb (level, s.threshold, s.ratio, s.knee, s.makeup);
                worst = juce::jmax (worst, std::abs (measured - predicted));

                expect (std::abs (measured - predicted) < 0.3, "thr " + juce::String (s.threshold) + ", " + juce::String (s.ratio) + ":1, knee "
                                                                  + juce::String (s.knee) + ", in " + juce::String (level) + " dB: DSP "
                                                                  + juce::String (measured, 2) + ", curve " + juce::String (predicted, 2));
            }
        }

        logMessage ("static curve vs DSP: worst difference " + juce::String (worst, 3) + " dB over 32 steady tones");

        // A typical setting: -20 dB threshold, 4:1, hard knee, a -10 dB tone.
        Compressor comp;
        comp.prepareToPlay (kRate, 512);
        setParam (comp.apvts, "threshold", -20.0f);
        setParam (comp.apvts, "ratio", 4.0f);
        setParam (comp.apvts, "knee", 0.0f);
        setParam (comp.apvts, "makeup", 0.0f);
        const int n = (int) kRate;
        const auto y = runMono (comp, makeSine (n, kRate, 1000.0, dbToLinear (-10.0f)), 256);
        expectWithinAbsoluteError ((float) toneDb (y.data() + n / 2, n / 2, kRate, 1000.0), -17.5f, 0.3f);
    }

    // -----------------------------------------------------------------------------------------------
    void testAmountMapping()
    {
        auto expectSettings = [this] (float amount, float threshold, float ratio, float knee, float makeup)
        {
            const auto s = Compressor::settingsForAmount (amount);
            expectWithinAbsoluteError (s.thresholdDb, threshold, 1.0e-4f, "threshold at " + juce::String (amount));
            expectWithinAbsoluteError (s.ratio, ratio, 1.0e-4f, "ratio at " + juce::String (amount));
            expectWithinAbsoluteError (s.kneeDb, knee, 1.0e-4f, "knee at " + juce::String (amount));
            expectWithinAbsoluteError (s.makeupDb, makeup, 1.0e-4f, "makeup at " + juce::String (amount));
        };

        expectSettings (0.0f, 0.0f, 1.0f, 6.0f, 0.0f);               // transparent
        expectSettings (50.0f, -20.0f, 3.0f, 6.0f, 4.0f);            // today's default
        expectSettings (100.0f, -30.0f, 6.0f, 6.0f, 9.0f);
        expectSettings (-10.0f, 0.0f, 1.0f, 6.0f, 0.0f);             // clamped
        expectSettings (140.0f, -30.0f, 6.0f, 6.0f, 9.0f);

        // 0 % changes nothing, whatever the level.
        expectEquals (Compressor::reductionDb (-6.0f, 0.0f, 1.0f, 6.0f), 0.0f);

        // Monotonic: more Amount = lower threshold, higher ratio, more makeup, more reduction on a typical loud peak.
        float lastThreshold = 1.0f, lastRatio = 0.0f, lastMakeup = -1.0f, lastReduction = -1.0f;
        bool monotonic = true;

        for (int a = 0; a <= 100; ++a)
        {
            const auto s = Compressor::settingsForAmount ((float) a);
            const float reduction = Compressor::reductionDb (-12.0f, s.thresholdDb, s.ratio, s.kneeDb);
            monotonic = monotonic && s.thresholdDb <= lastThreshold + 1.0e-5f && s.ratio >= lastRatio - 1.0e-5f
                        && s.makeupDb >= lastMakeup - 1.0e-5f && reduction >= lastReduction - 1.0e-5f;
            lastThreshold = s.thresholdDb;
            lastRatio = s.ratio;
            lastMakeup = s.makeupDb;
            lastReduction = reduction;
        }

        expect (monotonic, "every step of Amount compresses at least as much as the one before");

        // The processor's own defaults are the 50 % point, and the flag starts clear.
        Compressor comp;
        expectWithinAbsoluteError (comp.getAmount(), 50.0f, 1.0e-4f);
        expect (! comp.isAmountCustom());
        expectWithinAbsoluteError (raw (comp, "threshold"), -20.0f, 1.0e-3f);
        expectWithinAbsoluteError (raw (comp, "ratio"), 3.0f, 1.0e-3f);
        expectWithinAbsoluteError (raw (comp, "makeup"), 4.0f, 1.0e-3f);
        expectWithinAbsoluteError (raw (comp, "knee"), 6.0f, 1.0e-3f);

        // The detailed parameters keep their ids and ranges (presets depend on them).
        const char* ids[] { "threshold", "ratio", "attack", "release", "knee", "makeup" };
        const float lows[] { -60.0f, 1.0f, 0.1f, 10.0f, 0.0f, -12.0f }, highs[] { 0.0f, 20.0f, 100.0f, 1000.0f, 24.0f, 24.0f };

        for (int i = 0; i < 6; ++i)
        {
            auto* p = comp.apvts.getParameter (ids[i]);
            expect (p != nullptr, juce::String (ids[i]) + " exists");

            if (p != nullptr)
            {
                expectWithinAbsoluteError (p->convertFrom0to1 (0.0f), lows[i], 1.0e-4f);
                expectWithinAbsoluteError (p->convertFrom0to1 (1.0f), highs[i], 1.0e-4f);
            }
        }
    }

    void testCustomFlag()
    {
        // Amount through its parameter (what the knob does) writes the four detailed parameters.
        {
            Compressor comp;
            setParam (comp.apvts, "amount", 100.0f);
            expectWithinAbsoluteError (raw (comp, "threshold"), -30.0f, 0.02f);
            expectWithinAbsoluteError (raw (comp, "ratio"), 6.0f, 0.02f);
            expectWithinAbsoluteError (raw (comp, "makeup"), 9.0f, 0.02f);
            expectWithinAbsoluteError (raw (comp, "knee"), 6.0f, 0.02f);
            expectWithinAbsoluteError (raw (comp, "attack"), 10.0f, 0.02f);         // not part of the macro
            expectWithinAbsoluteError (raw (comp, "release"), 160.0f, 0.02f);
            expect (! comp.isAmountCustom(), "moving Amount is not an edit of the details");

            setParam (comp.apvts, "amount", 0.0f);
            expectWithinAbsoluteError (raw (comp, "threshold"), 0.0f, 0.02f);
            expectWithinAbsoluteError (raw (comp, "ratio"), 1.0f, 0.02f);
            expectWithinAbsoluteError (raw (comp, "makeup"), 0.0f, 0.02f);
        }

        // Editing any of the six says Custom, and the Amount value stays where the user left it.
        for (const char* id : { "threshold", "ratio", "attack", "release", "knee", "makeup" })
        {
            Compressor comp;
            setParam (comp.apvts, "amount", 70.0f);
            expect (! comp.isAmountCustom());

            auto* p = comp.apvts.getParameter (id);
            const float value = p->convertFrom0to1 (p->getValue());
            const float other = p->convertFrom0to1 (value > 0.5f * (p->convertFrom0to1 (0.0f) + p->convertFrom0to1 (1.0f)) ? 0.25f : 0.75f);
            setParam (comp.apvts, id, other);

            expect (comp.isAmountCustom(), juce::String (id) + " edited: Custom");
            expectWithinAbsoluteError (comp.getAmount(), 70.0f, 0.01f);

            // ... and moving Amount again takes it back.
            setParam (comp.apvts, "amount", 71.0f);
            expect (! comp.isAmountCustom(), juce::String (id) + ": Amount clears Custom");
        }

        // A parameter "set" to the value it already has (a preset re-applying defaults) is not an edit.
        {
            Compressor comp;

            for (const auto& [id, value] : std::vector<std::pair<const char*, float>> { { "threshold", -20.0f }, { "ratio", 3.0f }, { "attack", 10.0f },
                                                                                        { "release", 160.0f }, { "knee", 6.0f }, { "makeup", 4.0f } })
                setParam (comp.apvts, id, value);

            expect (! comp.isAmountCustom(), "the clean-voice preset leaves Amount at 50%");

            setParam (comp.apvts, "ratio", 4.0f);
            expect (comp.isAmountCustom(), "the broadcast preset does not fit the curve");
        }

        // applyAmount: the explicit "back to the curve", also when the value itself did not change.
        {
            Compressor comp;
            setParam (comp.apvts, "threshold", -33.0f);
            expect (comp.isAmountCustom());
            comp.applyAmount (50.0f);
            expect (! comp.isAmountCustom());
            expectWithinAbsoluteError (raw (comp, "threshold"), -20.0f, 0.02f);
            comp.applyAmount (85.0f);
            expectWithinAbsoluteError (comp.getAmount(), 85.0f, 0.01f);
            expectWithinAbsoluteError (raw (comp, "makeup"), 7.5f, 0.02f);          // 6.5 + 10/25 of the way to 9
        }

        // The compressor made through the registry behaves the same.
        {
            auto fx = BuiltinFx::create ("comp");
            auto* comp = dynamic_cast<Compressor*> (fx.get());
            expect (comp != nullptr);

            if (comp != nullptr)
            {
                setParam (comp->apvts, "amount", 30.0f);
                expect (! comp->isAmountCustom() && raw (*comp, "threshold") < -10.0f);
            }
        }
    }

    void testState()
    {
        // Amount, no edits: the details come back as the curve made them.
        {
            Compressor a;
            a.applyAmount (80.0f);
            juce::MemoryBlock block;
            a.getStateInformation (block);

            Compressor b;
            b.setStateInformation (block.getData(), (int) block.getSize());
            expectWithinAbsoluteError (b.getAmount(), 80.0f, 0.01f);
            expect (! b.isAmountCustom());
            expectWithinAbsoluteError (raw (b, "threshold"), raw (a, "threshold"), 0.01f);
            expectWithinAbsoluteError (raw (b, "ratio"), raw (a, "ratio"), 0.01f);
            expectWithinAbsoluteError (raw (b, "makeup"), raw (a, "makeup"), 0.01f);
        }

        // Amount and an edit: Custom is kept, Amount is kept, and the edit is not overwritten by the curve on load.
        {
            Compressor a;
            a.applyAmount (80.0f);
            setParam (a.apvts, "threshold", -12.0f);
            expect (a.isAmountCustom());
            juce::MemoryBlock block;
            a.getStateInformation (block);

            Compressor b;
            b.setStateInformation (block.getData(), (int) block.getSize());
            expect (b.isAmountCustom(), "Custom is stored");
            expectWithinAbsoluteError (b.getAmount(), 80.0f, 0.01f);
            expectWithinAbsoluteError (raw (b, "threshold"), -12.0f, 0.02f);
            expectWithinAbsoluteError (raw (b, "ratio"), raw (a, "ratio"), 0.02f);

            // Loading it over a compressor that had other ideas replaces all of it, including the flag.
            Compressor c;
            c.applyAmount (20.0f);
            c.setStateInformation (block.getData(), (int) block.getSize());
            expect (c.isAmountCustom());
            expectWithinAbsoluteError (c.getAmount(), 80.0f, 0.01f);

            // And the other way round: a clean state clears it.
            Compressor clean;
            juce::MemoryBlock cleanBlock;
            clean.getStateInformation (cleanBlock);
            c.setStateInformation (cleanBlock.getData(), (int) cleanBlock.getSize());
            expect (! c.isAmountCustom());
            expectWithinAbsoluteError (c.getAmount(), 50.0f, 0.01f);
            expectWithinAbsoluteError (raw (c, "threshold"), -20.0f, 0.02f);
        }

        // Junk is ignored.
        {
            Compressor a;
            a.applyAmount (65.0f);
            const char junk[] = "not a state";
            a.setStateInformation (junk, (int) sizeof (junk));
            expectWithinAbsoluteError (a.getAmount(), 65.0f, 0.01f);
            expect (! a.isAmountCustom());
        }

        // A state saved before Amount existed: its six values are all there is.
        auto legacyState = [] (float threshold)
        {
            Compressor source;
            setParam (source.apvts, "threshold", threshold);
            juce::MemoryBlock block;
            source.getStateInformation (block);

            auto xml = StateAccess::unpack (block.getData(), (int) block.getSize());

            if (xml != nullptr)
                for (const char* id : { "amount", "amountCustom" })
                    if (auto* child = xml->getChildByAttribute ("id", id))
                        xml->removeChildElement (child, true);

            juce::MemoryBlock legacy;

            if (xml != nullptr)
                StateAccess::pack (*xml, legacy);

            return legacy;
        };

        {
            const auto factory = legacyState (-20.0f);
            Compressor c;
            c.applyAmount (90.0f);
            c.setStateInformation (factory.getData(), (int) factory.getSize());
            expectWithinAbsoluteError (c.getAmount(), 50.0f, 0.01f);
            expect (! c.isAmountCustom(), "an old state at the factory values reads as 50%");

            const auto tuned = legacyState (-26.0f);
            Compressor d;
            d.setStateInformation (tuned.getData(), (int) tuned.getSize());
            expect (d.isAmountCustom(), "an old state with another threshold reads as Custom");
            expectWithinAbsoluteError (raw (d, "threshold"), -26.0f, 0.02f);
            expectWithinAbsoluteError (d.getAmount(), 50.0f, 0.01f);
        }
    }

    // -----------------------------------------------------------------------------------------------
    void testPanelDataFlow()
    {
        Compressor comp;
        comp.prepareToPlay (kRate, 512);
        setParam (comp.apvts, "threshold", -20.0f);
        setParam (comp.apvts, "ratio", 4.0f);
        setParam (comp.apvts, "knee", 0.0f);
        setParam (comp.apvts, "makeup", 0.0f);

        {
            CompPanel panel (comp);
            expect (! comp.displayActive.load(), "not on screen yet: no tap");

            panel.setVisible (true);
            panel.tickMeter (1.0 / 30.0);
            expect (panel.isTapActive() && comp.displayActive.load(), "on screen: the tap is on");
            expect (panel.getHeight() == panel.getPreferredHeight() && ! panel.isDetailsOpen());

            // 6 s of a -10 dBFS tone, one 30 fps frame at a time.
            std::vector<float> l ((size_t) (kRate / 30.0)), r;
            juce::MidiBuffer midi;
            double phase = 0.0;

            for (int frame = 0; frame < 180; ++frame)
            {
                for (auto& s : l)
                {
                    s = (float) (dbToLinear (-10.0f) * std::sin (phase));
                    phase += 2.0 * kPi * 200.0 / kRate;
                }

                r = l;
                float* channels[2] { l.data(), r.data() };
                juce::AudioBuffer<float> block (channels, 2, (int) l.size());
                comp.processBlock (block, midi);
                panel.tickMeter (1.0 / 30.0);
            }

            const auto& newest = panel.getHistory().getNewestColumn();
            logMessage ("panel after 6 s: newest column in " + juce::String (newest.inDb, 1) + ", out " + juce::String (newest.outDb, 1)
                        + ", reduction " + juce::String (newest.grDb, 1) + "; meter " + juce::String (panel.getGrMeter().getHeldDb(), 1) + " dB");
            expect (std::abs (newest.inDb + 10.0f) < 0.5f && std::abs (newest.outDb + 17.5f) < 0.6f && std::abs (newest.grDb + 7.5f) < 0.6f,
                    "the newest history column shows what the compressor does");
            expect (std::abs (panel.getGrMeter().getHeldDb() - 7.5f) < 0.6f, "the meter reads the reduction");
            expect (panel.getCurve().isDotVisible() && panel.getCurve().isDotBright(), "the dot is on the curve and bright while compressing");

            // Off screen: the tap goes off and stays off while audio runs.
            panel.setVisible (false);
            panel.tickMeter (1.0 / 30.0);
            expect (! panel.isTapActive() && ! comp.displayActive.load(), "hidden: the tap is off");
            const auto written = comp.getDisplayRecordsWritten();
            std::vector<Compressor::DisplayRecord> ignored;
            hammer (comp, -10.0f, 0.5, ignored);
            expectEquals ((int) comp.getDisplayRecordsWritten(), (int) written);

            // Back on screen: a fresh start.
            panel.setVisible (true);
            panel.tickMeter (1.0 / 30.0);
            expect (comp.displayActive.load() && panel.getHistory().getNewestColumn().inDb <= -99.0f, "shown again: the history starts empty");
        }

        expect (! comp.displayActive.load(), "the panel going away switches the tap off");

        // "On screen" means every parent shows it: a hidden parent, or scrolled out of a parent's visible area, is not.
        {
            juce::Component root, middle;
            root.setBounds (0, 0, 400, 600);
            root.setVisible (true);
            middle.setBounds (0, 0, 400, 300);
            root.addAndMakeVisible (middle);

            CompPanel panel (comp);
            middle.addAndMakeVisible (panel);
            panel.setTopLeftPosition (16, 10);
            panel.tickMeter (0.033);
            expect (panel.isTapActive() && comp.displayActive.load(), "inside a visible parent: on");

            middle.setVisible (false);
            panel.tickMeter (0.033);
            expect (! panel.isTapActive() && ! comp.displayActive.load(), "parent hidden: off");

            middle.setVisible (true);
            panel.setTopLeftPosition (16, 400);
            panel.tickMeter (0.033);
            expect (! panel.isTapActive(), "scrolled out of the parent's area: off");

            panel.setTopLeftPosition (16, 200);
            panel.tickMeter (0.033);
            expect (panel.isTapActive(), "partly visible: on");
        }

        expect (! comp.displayActive.load());

        // The Details toggle.
        {
            CompPanel panel (comp);
            const int collapsed = panel.getPreferredHeight();
            panel.setDetailsOpen (true);
            expect (panel.isDetailsOpen() && panel.getHeight() == panel.getPreferredHeight() && panel.getPreferredHeight() > collapsed + 150);
            panel.setDetailsOpen (false);
            expectEquals (panel.getPreferredHeight(), collapsed);
        }
    }

    void testPanelCost()
    {
        for (const int scale : { 1, 2 })
        {
            Compressor comp;
            comp.prepareToPlay (kRate, 512);
            CompPanel panel (comp);
            panel.setVisible (true);
            panel.tickMeter (1.0 / 30.0);

            const int width = panel.getWidth(), height = panel.getHeight();
            juce::Image image (juce::Image::ARGB, width * scale, height * scale, true, juce::SoftwareImageType());

            auto paintPart = [&] (juce::Component& part, juce::Rectangle<int> clip)
            {
                juce::Graphics g (image);
                g.addTransform (juce::AffineTransform::scale ((float) scale));
                g.setOrigin (part.getPosition());
                g.reduceClipRegion (clip.isEmpty() ? part.getLocalBounds() : clip);
                part.paintEntireComponent (g, false);
            };

            // A speech-like signal: 4 Hz syllables of a 150 Hz tone, loud enough to compress.
            std::vector<float> l ((size_t) (kRate / 30.0)), r;
            juce::MidiBuffer midi;
            double phase = 0.0, t = 0.0;
            double total = 0.0, worst = 0.0, tickOnly = 0.0;
            const int warm = 30, frames = 300;

            for (int frame = 0; frame < warm + frames; ++frame)
            {
                for (auto& s : l)
                {
                    const double envelope = 0.5 + 0.5 * std::sin (2.0 * kPi * 4.0 * t);
                    s = (float) (0.35 * envelope * std::sin (phase));
                    phase += 2.0 * kPi * 150.0 / kRate;
                    t += 1.0 / kRate;
                }

                r = l;
                float* channels[2] { l.data(), r.data() };
                juce::AudioBuffer<float> block (channels, 2, (int) l.size());
                comp.processBlock (block, midi);

                const auto start = juce::Time::getHighResolutionTicks();
                panel.tickMeter (1.0 / 30.0);
                const auto afterTick = juce::Time::getHighResolutionTicks();

                // What the frame repaints: the history (it scrolled), the meter, and the little square around the dot.
                paintPart (panel.getHistory(), {});
                paintPart (panel.getGrMeter(), {});
                paintPart (panel.getCurve(), juce::Rectangle<int> (60, 60, 24, 24));
                const auto end = juce::Time::getHighResolutionTicks();

                if (frame >= warm)
                {
                    const double ms = 1000.0 * juce::Time::highResolutionTicksToSeconds (end - start);
                    total += ms;
                    worst = juce::jmax (worst, ms);
                    tickOnly += 1000.0 * juce::Time::highResolutionTicksToSeconds (afterTick - start);
                }
            }

            const double average = total / frames;
            logMessage ("compressor panel at " + juce::String (scale) + "x: " + juce::String (average, 3) + " ms per 30 fps frame on average (tick "
                        + juce::String (tickOnly / frames, 3) + " ms, worst " + juce::String (worst, 3) + " ms)");
            expect (average < 3.0, "a frame costs " + juce::String (average, 2) + " ms at " + juce::String (scale) + "x");
        }
    }
};

static CompressorDisplayTests compressorDisplayTests;
}
