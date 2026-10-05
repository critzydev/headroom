// Unit tests for AudioEngine mixing, gains and mutes, mic path, rate handling, chunking, meters and FX snapshots.
#include "Juce.h"

#include "TestHelpers.h"
#include "engine/AudioEngine.h"
#include "engine/FxChain.h"

#include <array>
#include <cmath>
#include <memory>
#include <vector>

namespace hr
{
using test::EngineHarness;
using test::HalvingProcessor;

class EngineTests final : public juce::UnitTest
{
public:
    EngineTests() : juce::UnitTest ("AudioEngine", "headroom") {}

    void runTest() override
    {
        beginTest ("two channels at 0 dB sum into the monitor mix");
        testSum();

        beginTest ("mute and mix independence");
        testMuteAndIndependentMixes();

        beginTest ("fader changes are smoothed");
        testSmoothing();

        beginTest ("mic reaches micOut (mono, stereo sum, mute)");
        testMic();

        beginTest ("sample-rate mismatch gives silence and requests the rate");
        testRateMismatch();

        beginTest ("blocks larger than kMaxBlock are chunked correctly");
        testChunking();

        beginTest ("meters are max-since-last-read");
        testMeters();

        beginTest ("safety limiter and master controls");
        testMaster();

        beginTest ("mic silence watchdog counters");
        testMicWatch();

        beginTest ("FX chain snapshots: process, bypass, release");
        testFxChain();

        beginTest ("inactive and unlinked channels are skipped");
        testInactive();
    }

private:
    static constexpr int kFrames = 4096;
    static constexpr double kRate = test::kRate;

    static void prepared (EngineHarness& h)
    {
        h.engine->prepare (kRate);
        h.engine->setChannelActive (0, true);
        h.engine->setChannelActive (1, true);
    }

    void expectAll (const std::vector<float>& v, int from, int to, float expected, float tolerance, const juce::String& what)
    {
        float worst = 0.0f;

        for (int i = from; i < to; ++i)
            worst = juce::jmax (worst, std::abs (v[(size_t) i] - expected));

        expect (worst <= tolerance, what + ": worst error " + juce::String (worst) + " vs expected " + juce::String (expected));
    }

    void testSum()
    {
        EngineHarness h;
        prepared (h);
        h.setAppInput (0, 0.1f, 0.1f);
        h.setAppInput (1, 0.2f, 0.25f);
        h.engine->setChannelActive (kMicChannel, false);     // keep the mic out of this test

        const int latency = h.engine->getMasterLatencySamples();
        expect (latency > 0, "the master limiter reports its lookahead");

        h.run (512);

        // The mixes are delayed by the master limiter's lookahead; before that it is silence.
        expectAll (h.monitorL(), 0, latency, 0.0f, 0.0f, "monitor L before the lookahead has filled");
        expectAll (h.monitorL(), latency, 512, 0.3f, 1.0e-5f, "monitor L");
        expectAll (h.monitorR(), latency, 512, 0.35f, 1.0e-5f, "monitor R");
        expectAll (h.streamL(), latency, 512, 0.3f, 1.0e-5f, "stream L");

        // -6.0206 dB is a factor 0.5
        h.engine->setChannelGainDb (0, Mix::Monitor, -6.0206f);
        h.settle();
        expectAll (h.monitorL(), 0, kFrames, 0.05f + 0.2f, 1.0e-4f, "monitor L after -6 dB on slot 0");
        expectAll (h.streamL(), 0, kFrames, 0.3f, 1.0e-5f, "stream L unaffected by the monitor fader");
    }

    void testMuteAndIndependentMixes()
    {
        EngineHarness h;
        prepared (h);
        h.setAppInput (0, 0.1f);
        h.setAppInput (1, 0.2f);
        h.engine->setChannelActive (kMicChannel, false);

        h.engine->setChannelMuted (0, Mix::Monitor, true);
        h.settle();
        expectAll (h.monitorL(), 0, kFrames, 0.2f, 1.0e-5f, "monitor with slot 0 muted");
        expectAll (h.streamL(), 0, kFrames, 0.3f, 1.0e-5f, "stream keeps slot 0");

        h.engine->setChannelGainDb (1, Mix::Stream, -6.0206f);
        h.settle();
        expectAll (h.streamL(), 0, kFrames, 0.1f + 0.1f, 1.0e-4f, "stream with slot 1 at -6 dB");
        expectAll (h.monitorL(), 0, kFrames, 0.2f, 1.0e-5f, "monitor unaffected by the stream fader");

        h.engine->setChannelMuted (0, Mix::Monitor, false);
        h.engine->setChannelGainDb (1, Mix::Monitor, kMinusInfDb);      // -inf
        h.settle();
        expectAll (h.monitorL(), 0, kFrames, 0.1f, 1.0e-5f, "monitor with slot 1 at -inf");

        // Master mute silences one mix only.
        h.engine->setMasterMuted (Mix::Monitor, true);
        h.settle();
        expectAll (h.monitorL(), 0, kFrames, 0.0f, 1.0e-6f, "muted monitor master");
        expectAll (h.streamL(), 0, kFrames, 0.2f, 1.0e-4f, "stream master unaffected");
    }

    void testSmoothing()
    {
        EngineHarness h;
        prepared (h);
        h.setAppInput (0, 1.0f);
        h.engine->setChannelActive (kMicChannel, false);
        h.engine->setMasterGainDb (Mix::Monitor, -12.0f);        // keep the limiter out of the picture
        h.engine->setChannelActive (1, false);
        h.run (2048);
        h.settle();

        const float steady = h.monitorL().back();
        h.engine->setChannelMuted (0, Mix::Monitor, true);
        h.run (2048);

        expect (std::abs (h.monitorL()[0] - steady) < 0.05f, "no jump on the first sample after a mute");
        expect (h.monitorL()[100] < h.monitorL()[0] && h.monitorL()[100] > 0.0f, "gain ramps down rather than stepping");
        expectAll (h.monitorL(), 1200, 2048, 0.0f, 1.0e-6f, "fully muted after the 20 ms ramp");

        // Monotonic ramp: no zipper.
        bool monotonic = true;

        for (int i = 1; i < 960; ++i)
            monotonic = monotonic && h.monitorL()[(size_t) i] <= h.monitorL()[(size_t) i - 1] + 1.0e-7f;

        expect (monotonic, "ramp is monotonic");
    }

    void testMic()
    {
        EngineHarness h;
        prepared (h);
        h.engine->setChannelActive (0, false);
        h.engine->setChannelActive (1, false);
        const int latency = h.engine->getMasterLatencySamples();

        // Mono mic: L only, duplicated to both micOut sides. micOut is not delayed by the master limiter.
        h.setMic (0.25f);
        h.run (512);
        expectAll (h.micOutL(), 0, 512, 0.25f, 1.0e-6f, "micOut L (mono)");
        expectAll (h.micOutR(), 0, 512, 0.25f, 1.0e-6f, "micOut R (mono)");

        // Both sides linked: (L+R)/2.
        float right = 0.5f;
        h.setMic (0.1f, &right);
        h.run (512);
        expectAll (h.micOutL(), 0, 512, 0.3f, 1.0e-6f, "micOut L (stereo sum)");
        expectAll (h.micOutR(), 0, 512, 0.3f, 1.0e-6f, "micOut R (stereo sum)");

        // The mic also feeds both mixes at its fader value (engine default: unity).
        expectAll (h.monitorL(), latency, 512, 0.3f, 1.0e-5f, "mic in the monitor mix");
        expectAll (h.streamL(), latency, 512, 0.3f, 1.0e-5f, "mic in the stream mix");

        h.engine->setChannelMuted (kMicChannel, Mix::Monitor, true);
        h.engine->setChannelGainDb (kMicChannel, Mix::Stream, kMinusInfDb);
        h.settle();
        expectAll (h.monitorL(), 0, kFrames, 0.0f, 1.0e-6f, "mic muted in monitor");
        expectAll (h.streamL(), 0, kFrames, 0.0f, 1.0e-6f, "mic at -inf in stream");
        expectAll (h.micOutL(), 0, kFrames, 0.3f, 1.0e-5f, "micOut is independent of the mix faders");

        h.engine->setMicOutMuted (true);
        h.settle();
        expectAll (h.micOutL(), 0, kFrames, 0.0f, 1.0e-6f, "micOut muted");

        // Not linked: micOut is still fully written (silence).
        h.engine->setMicOutMuted (false);
        h.unlinkMic();
        h.poisonOutputs();
        h.run (512);
        expectAll (h.micOutL(), 0, 512, 0.0f, 1.0e-6f, "micOut without a mic");
    }

    void testRateMismatch()
    {
        EngineHarness h;
        prepared (h);
        h.setAppInput (0, 0.1f);
        h.setMic (0.1f);

        h.poisonOutputs();
        h.run (512, 44100.0);

        for (size_t i = 0; i < h.outputs.size(); ++i)
            expectAll (h.outputs[i], 0, 512, 0.0f, 0.0f, "output " + juce::String ((int) i) + " silent on rate mismatch");

        expectEquals (h.engine->requestedRate.load(), 44100.0);
        expectEquals (h.engine->getPreparedRate(), kRate);

        // The message thread re-prepares and audio flows again at the new rate.
        h.engine->requestedRate.store (0.0);
        expect (h.engine->prepare (44100.0));
        h.engine->setChannelActive (kMicChannel, false);
        h.run (512, 44100.0);
        expectAll (h.monitorL(), h.engine->getMasterLatencySamples(), 512, 0.1f, 1.0e-5f, "audio after re-prepare");
        expectEquals (h.engine->requestedRate.load(), 0.0);

        // Before the first prepare(): silence too, and the rate is requested.
        EngineHarness fresh;
        fresh.setAppInput (0, 0.1f);
        fresh.poisonOutputs();
        fresh.run (256);
        expectAll (fresh.monitorL(), 0, 256, 0.0f, 0.0f, "silence before prepare()");
        expectEquals (fresh.engine->requestedRate.load(), kRate);
    }

    void testChunking()
    {
        EngineHarness h;
        prepared (h);
        h.engine->setChannelActive (kMicChannel, false);
        h.engine->setChannelActive (1, false);
        const int latency = h.engine->getMasterLatencySamples();

        // A ramp makes any chunk that restarts at the wrong offset obvious.
        h.setAppRamp (0, 1.0e-4f);
        h.poisonOutputs();

        constexpr int frames = 3000;                            // 3 chunks: 1024 + 1024 + 952
        static_assert (frames > kMaxBlock);
        h.run (frames);

        // Output = ramp delayed by the master lookahead (silence before that).
        float worst = 0.0f;

        for (int i = 0; i < frames; ++i)
        {
            const float expected = i < latency ? 0.0f : 1.0e-4f * (float) (i - latency);
            worst = juce::jmax (worst, std::abs (h.monitorL()[(size_t) i] - expected));
        }

        expect (worst < 1.0e-5f, "every chunk lands at its own offset, worst error " + juce::String (worst));
        expectEquals (h.monitorL()[(size_t) frames], 7.0f);      // nothing written past numFrames
        expectEquals (h.streamL()[(size_t) frames], 7.0f);

        // Odd sizes and 1-frame cycles: a continuous stream stays continuous however it is cut up.
        for (int size : { 1, 37, 256, 1023, 1024, 1025, 2049 })
        {
            h.flush (0);
            h.setAppRamp (0, 1.0e-4f);
            h.poisonOutputs();
            h.run (size);

            float worstSize = 0.0f;

            for (int i = 0; i < size; ++i)
            {
                const float expected = i < latency ? 0.0f : 1.0e-4f * (float) (i - latency);
                worstSize = juce::jmax (worstSize, std::abs (h.monitorL()[(size_t) i] - expected));
            }

            expect (worstSize < 1.0e-5f, "block of " + juce::String (size) + " frames, worst error " + juce::String (worstSize));
            expectEquals (h.monitorL()[(size_t) size], 7.0f);
        }
    }

    void testMeters()
    {
        EngineHarness h;
        prepared (h);
        h.engine->setChannelActive (kMicChannel, false);
        h.setAppInput (0, 0.5f, 0.25f);
        h.engine->setChannelGainDb (0, Mix::Stream, -6.0206f);
        h.settle();

        const auto levels = h.engine->readChannelLevels (0);
        expectWithinAbsoluteError (levels.input.l, 0.5f, 1.0e-5f);
        expectWithinAbsoluteError (levels.input.r, 0.25f, 1.0e-5f);
        expectWithinAbsoluteError (levels.monitor.l, 0.5f, 1.0e-4f);
        expectWithinAbsoluteError (levels.stream.l, 0.25f, 1.0e-4f);         // post-fader
        expectWithinAbsoluteError (levels.stream.r, 0.125f, 1.0e-4f);

        const auto master = h.engine->readMasterLevels (Mix::Monitor);
        expectWithinAbsoluteError (master.l, 0.5f, 1.0e-4f);

        // Reading resets: with no new audio the next read is zero.
        h.engine->readChannelLevels (0);
        const auto again = h.engine->readChannelLevels (0);
        expectEquals (again.input.l, 0.0f);
        expectEquals (h.engine->readMasterLevels (Mix::Monitor).l, 0.0f);

        // Max since last read, not last block: a loud block followed by a quiet one still reports the loud one.
        h.setAppInput (0, 0.8f);
        h.run (256);
        h.setAppInput (0, 0.1f);
        h.run (256);
        const auto loud = h.engine->readChannelLevels (0);
        expectWithinAbsoluteError (loud.input.l, 0.8f, 1.0e-5f);
        expectWithinAbsoluteError (loud.monitor.l, 0.8f, 1.0e-5f);

        // Pre-fader input meter ignores the fader; the post-fader meters follow it.
        h.engine->setChannelGainDb (0, Mix::Monitor, -20.0f);
        h.settle();
        h.engine->readChannelLevels (0);
        h.run (256);
        const auto quiet = h.engine->readChannelLevels (0);
        expectWithinAbsoluteError (quiet.input.l, 0.1f, 1.0e-5f);
        expectWithinAbsoluteError (quiet.monitor.l, 0.01f, 1.0e-4f);
        expectWithinAbsoluteError (quiet.stream.l, 0.1f * 0.5f, 1.0e-4f);
    }

    void testMaster()
    {
        EngineHarness h;
        prepared (h);
        h.engine->setChannelActive (kMicChannel, false);
        h.setAppInput (0, 0.9f);
        h.setAppInput (1, 0.9f);        // 1.8 into a 1.0 full scale mix

        h.run (2048);
        float peak = 0.0f;

        for (int i = 0; i < 2048; ++i)
            peak = juce::jmax (peak, std::abs (h.monitorL()[(size_t) i]), std::abs (h.streamL()[(size_t) i]));

        expect (peak <= 0.96605f + 1.0e-6f, "limiter ceiling holds (peak " + juce::String (peak) + ")");
        expect (peak > 0.9f, "limiter is not squashing more than needed");

        h.engine->setLimiterEnabled (Mix::Monitor, false);
        h.settle();
        expectAll (h.monitorL(), 0, kFrames, 1.8f, 1.0e-4f, "monitor unlimited");
        expect (h.streamL().back() <= 0.96605f + 1.0e-6f, "stream limiter still on");

        h.engine->setMasterGainDb (Mix::Monitor, -20.0f);
        h.settle();
        expectAll (h.monitorL(), 0, kFrames, 0.18f, 1.0e-4f, "master at -20 dB");
    }

    void testMicWatch()
    {
        EngineHarness h;
        prepared (h);
        h.engine->setChannelActive (kMicChannel, false);

        h.unlinkMic();
        h.run (256);
        expectEquals ((int) h.engine->micLiveCycles.load(), 0);
        expectEquals ((int) h.engine->micSilentMs.load(), 0);

        h.setMicSilent();
        for (int i = 0; i < 400; ++i)       // 400 x 256 frames = 2.13 s
            h.run (256);

        expect (h.engine->micSilentMs.load() >= 2000, "silent for >= 2 s, got " + juce::String ((int) h.engine->micSilentMs.load()));
        expectEquals ((int) h.engine->micSeenSignal.load(), 0);
        expectEquals ((int) h.engine->micLiveCycles.load(), 400);

        h.setMic (0.1f);
        h.run (256);
        expectEquals ((int) h.engine->micSeenSignal.load(), 1);
        expectEquals ((int) h.engine->micSilentMs.load(), 0);

        // Mic goes quiet after having had signal: the counter runs but "seen signal" stays set.
        h.setMicSilent();
        for (int i = 0; i < 200; ++i)
            h.run (256);

        expect (h.engine->micSilentMs.load() >= 1000);
        expectEquals ((int) h.engine->micSeenSignal.load(), 1);

        h.engine->resetMicWatch();
        h.run (256);
        expectEquals ((int) h.engine->micSeenSignal.load(), 0);
        expect (h.engine->micSilentMs.load() < 10);
    }

    void testFxChain()
    {
        EngineHarness h;
        prepared (h);
        h.engine->setChannelActive (kMicChannel, false);
        h.engine->setChannelActive (1, false);
        h.setAppInput (0, 0.4f);

        auto processor = std::make_shared<HalvingProcessor>();
        auto* halver = processor.get();
        std::weak_ptr<juce::AudioProcessor> weak = processor;

        auto slot = std::make_shared<FxSlot> (processor, "abcd", 42u);
        processor.reset();
        slot->configure (kRate, kMaxBlock);

        expectEquals (slot->getNumInputChannels(), 2);
        expectEquals (slot->getNumOutputChannels(), 2);

        auto chain = FxChain::create ({ slot });
        h.engine->setChain (0, chain);
        h.settle();
        expectAll (h.monitorL(), 0, kFrames, 0.2f, 1.0e-5f, "processed through the chain");
        expectEquals ((int) h.engine->getChain (0)->getSlots().size(), 1);

        // Bypass is a short crossfade, then the audio is untouched.
        slot->bypassed.store (true);
        h.settle();
        expectAll (h.monitorL(), 0, kFrames, 0.4f, 1.0e-5f, "bypassed slot passes audio untouched");
        slot->bypassed.store (false);
        h.settle();
        expectAll (h.monitorL(), 0, kFrames, 0.2f, 1.0e-5f, "and processes again once switched back on");

        // A sample-rate change re-prepares the processors that are live in the chain.
        expectEquals (halver->lastPreparedRate, kRate);
        expect (h.engine->prepare (44100.0));
        expectEquals (halver->lastPreparedRate, 44100.0);
        h.settle (44100.0);
        expectAll (h.monitorL(), 0, kFrames, 0.2f, 1.0e-5f, "chain still processes after re-prepare");
        expect (h.engine->prepare (kRate));
        h.settle();

        // Drop every reference we hold except the engine's. Replacing the chain retires the old snapshot; nothing
        // is inside process() here, so it goes at once (the busy case is covered by the stress tests).
        chain = nullptr;
        slot.reset();
        h.engine->setChain (0, nullptr);
        expect (weak.expired(), "retired snapshot released while the RT thread is idle");

        h.settle();
        expectAll (h.monitorL(), 0, kFrames, 0.4f, 1.0e-5f, "no chain: audio untouched");
    }

    void testInactive()
    {
        EngineHarness h;
        h.engine->prepare (kRate);
        h.engine->setChannelActive (kMicChannel, false);
        h.setAppInput (5, 0.5f);           // slot 5 was never activated

        h.poisonOutputs();
        h.run (256);
        expectAll (h.monitorL(), 0, 256, 0.0f, 0.0f, "inactive channel contributes nothing (and outputs are still written)");

        h.engine->setChannelActive (5, true);
        h.settle();
        expectAll (h.monitorL(), 0, kFrames, 0.5f, 1.0e-5f, "channel audible once active");

        h.appLinked[5] = false;             // port unlinked
        h.settle();
        expectAll (h.monitorL(), 0, kFrames, 0.0f, 0.0f, "unlinked port is silence");
    }
};

static EngineTests engineTests;
}
