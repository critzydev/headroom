// Hardening tests for the engine: FX snapshot swapping under a running RT thread, rate changes, bad plugins, odd
// buffers, the master limiter, the mic watchdog and FxSlot's bypass / crash handling.
#include "Juce.h"

#include "AllocationGuard.h"
#include "TestHelpers.h"
#include "engine/AudioEngine.h"
#include "engine/FxChain.h"
#include "engine/builtin/BuiltinFx.h"
#include "engine/builtin/NoiseRemoval.h"
#include "plugins/CrashGuard.h"

#include <atomic>
#include <functional>
#include <stdexcept>
#include <thread>

namespace hr
{
using namespace test;

namespace
{
    bool waitUntil (const std::function<bool()>& condition, int timeoutMs)
    {
        const auto deadline = juce::Time::getMillisecondCounter() + (uint32_t) timeoutMs;

        while (! condition())
        {
            if (juce::Time::getMillisecondCounter() > deadline)
                return false;

            juce::Thread::sleep (1);
        }

        return true;
    }

    // Blocks inside processBlock until told to leave: an RT thread stuck in a plugin.
    class BlockingProcessor final : public TestProcessor
    {
    public:
        void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override
        {
            entered.store (true);

            while (! release.load())
                std::this_thread::yield();
        }

        std::atomic<bool> entered { false }, release { false };
    };

    // Halves the signal and notes when it is destroyed anywhere but on the message thread (which would mean the RT
    // thread dropped the last reference to a retired snapshot).
    class DestructionProbeProcessor final : public TestProcessor
    {
    public:
        ~DestructionProbeProcessor() override
        {
            alive.store (0xDEAD);
            ++destroyed;

            if (! juce::MessageManager::getInstance()->isThisTheMessageThread())
                ++destroyedElsewhere;
        }

        void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
        {
            if (alive.load() != 0xC0FFEE)
                ++usedAfterFree;

            b.applyGain (0.5f);
        }

        std::atomic<int> alive { 0xC0FFEE };
        static inline std::atomic<int> destroyed { 0 }, destroyedElsewhere { 0 }, usedAfterFree { 0 };
    };

    // Remembers what CrashGuard::currentRtSlot said while it was running.
    class SlotProbeProcessor final : public TestProcessor
    {
    public:
        void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override
        {
            seen.store (CrashGuard::currentRtSlot.load());
            ++calls;
        }

        std::atomic<uint32_t> seen { 0 };
        std::atomic<int> calls { 0 };
    };

    class ThrowingProcessor final : public TestProcessor
    {
    public:
        void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override
        {
            if (armed.load())
                throw std::runtime_error ("plugin exploded");
        }

        std::atomic<bool> armed { true };
    };

    // A plugin that only supports mono.
    class MonoHalver final : public juce::AudioProcessor
    {
    public:
        MonoHalver()
            : juce::AudioProcessor (BusesProperties().withInput ("in", juce::AudioChannelSet::mono(), true)
                                                     .withOutput ("out", juce::AudioChannelSet::mono(), true)) {}

        bool isBusesLayoutSupported (const BusesLayout& layouts) const override
        {
            return layouts.getMainInputChannelSet() == juce::AudioChannelSet::mono()
                   && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::mono();
        }

        const juce::String getName() const override                    { return "MonoHalver"; }
        void prepareToPlay (double, int) override                       {}
        void releaseResources() override                                {}
        void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override    { jassert (b.getNumChannels() == 1); b.applyGain (0.5f); }
        double getTailLengthSeconds() const override                    { return 0.0; }
        bool acceptsMidi() const override                               { return false; }
        bool producesMidi() const override                              { return false; }
        juce::AudioProcessorEditor* createEditor() override             { return nullptr; }
        bool hasEditor() const override                                 { return false; }
        int getNumPrograms() override                                   { return 1; }
        int getCurrentProgram() override                                { return 0; }
        void setCurrentProgram (int) override                           {}
        const juce::String getProgramName (int) override                { return {}; }
        void changeProgramName (int, const juce::String&) override      {}
        void getStateInformation (juce::MemoryBlock&) override          {}
        void setStateInformation (const void*, int) override            {}
    };

    // A stand-in for the backend's RT thread: feeds noise into a few app slots and the mic, random block sizes, and
    // checks every output block.
    class RtRunner
    {
    public:
        explicit RtRunner (AudioEngine& e, int fixedBlock = 0) : engine (e), fixedBlockSize (fixedBlock)
        {
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k < 4; ++k)
                    appBuffers[(size_t) k][(size_t) c] = makeNoise (kCapacity, 0.1, (uint32_t) (10 * k + c + 1));

            mic = makeNoise (kCapacity, 0.05, 99);

            for (auto& o : outputs)
                o.assign (kCapacity, 0.0f);
        }

        ~RtRunner() { stop(); }

        void start()
        {
            stopFlag.store (false);
            worker = std::thread ([this] { run(); });
        }

        void stop()
        {
            stopFlag.store (true);

            if (worker.joinable())
                worker.join();
        }

        std::atomic<double> rate { 48000.0 };
        std::atomic<uint64_t> blocks { 0 };
        std::atomic<uint32_t> badBlocks { 0 };          // non-finite output, or a mix above -0.3 dBFS
        std::atomic<float> lastPeak { 0.0f };
        std::atomic<int> biggestBlock { 0 };

    private:
        static constexpr int kCapacity = 8192;

        void run()
        {
            std::mt19937 rng (1234);
            std::uniform_int_distribution<int> anySize (1, kCapacity);
            const int typical[] { 1, 37, 64, 128, 256, 480, 1023, 1024, 1025, 2048, 4096, 8192 };

            while (! stopFlag.load())
            {
                const int n = fixedBlockSize > 0 ? fixedBlockSize
                                                 : (rng() % 8 == 0 ? typical[rng() % 12] : (rng() % 8 == 0 ? anySize (rng) : 1 + (int) (rng() % 300)));

                EngineIO io;
                io.numFrames = n;
                io.sampleRate = rate.load();

                for (int k = 0; k < 4; ++k)
                    for (int c = 0; c < 2; ++c)
                        io.appIn[k][c] = appBuffers[(size_t) k][(size_t) c].data();

                io.micIn[0] = mic.data();
                io.monitorOut[0] = outputs[0].data();  io.monitorOut[1] = outputs[1].data();
                io.streamOut[0] = outputs[2].data();   io.streamOut[1] = outputs[3].data();
                io.micOut[0] = outputs[4].data();      io.micOut[1] = outputs[5].data();

                engine.process (io);

                float peak = 0.0f;
                bool ok = true;

                for (size_t i = 0; i < outputs.size(); ++i)
                {
                    ok = ok && allFinite (outputs[i].data(), n);
                    const float p = peakAbs (outputs[i].data(), n);
                    peak = juce::jmax (peak, p);

                    if (i < 4 && p > 0.96606f)
                        ok = false;
                }

                if (! ok)
                    badBlocks.fetch_add (1);

                lastPeak.store (peak);
                biggestBlock.store (juce::jmax (biggestBlock.load(), n));
                blocks.fetch_add (1);
            }
        }

        AudioEngine& engine;
        const int fixedBlockSize;
        std::array<std::array<std::vector<float>, 2>, 4> appBuffers;
        std::vector<float> mic;
        std::array<std::vector<float>, 6> outputs;
        std::atomic<bool> stopFlag { false };
        std::thread worker;
    };

    FxSlotPtr makeSlot (std::shared_ptr<juce::AudioProcessor> processor, const juce::String& uid, uint32_t crashId, double rate = 48000.0)
    {
        auto slot = std::make_shared<FxSlot> (std::move (processor), uid, crashId);
        slot->configure (rate, kMaxBlock);
        return slot;
    }
}

class EngineHardeningTests final : public juce::UnitTest
{
public:
    EngineHardeningTests() : juce::UnitTest ("Engine hardening", "headroom") {}

    void runTest() override
    {
        beginTest ("chain swap stress: 2000 snapshots under a running RT thread");
        testSwapStress();

        beginTest ("a retired snapshot survives while the RT thread is inside it");
        testRetiredWhileBusy();

        beginTest ("rate change 48k -> 44.1k -> 48k with live chains");
        testRateChangeLive();

        beginTest ("prepare gives up cleanly when the RT thread is stuck");
        testPrepareTimeout();

        beginTest ("nothing on the RT thread allocates or frees");
        testNoAllocation();

        beginTest ("NaN, Inf and huge samples never reach the mix");
        testSanitising();

        beginTest ("CrashGuard slot id is published around processBlock");
        testCrashGuard();

        beginTest ("odd block sizes, null inputs and outputs");
        testBuffers();

        beginTest ("master limiter: brickwall, transparent, constant delay");
        testMasterLimiter();

        beginTest ("mic watchdog counters");
        testMicWatch();

        beginTest ("FxSlot: bypass crossfade and warm-up");
        testBypass();

        beginTest ("FxSlot: mono plugins, throwing plugins, re-prepare");
        testSlotMisc();
    }

private:
    static constexpr double kRate = test::kRate;

    // ------------------------------------------------------------------------------------------------
    void testSwapStress()
    {
        std::vector<std::weak_ptr<juce::AudioProcessor>> everything;
        DestructionProbeProcessor::destroyed = 0;
        DestructionProbeProcessor::destroyedElsewhere = 0;
        DestructionProbeProcessor::usedAfterFree = 0;

        {
            AudioEngine engine;
            engine.prepare (kRate);

            for (int k = 0; k < 4; ++k)
                engine.setChannelActive (k, true);

            RtRunner rt (engine);
            rt.start();

            std::mt19937 rng (42);
            const char* ids[] { "eq", "comp", "gate", "deess", "limit", "eq", "comp", "limit" };
            std::array<std::vector<FxSlotPtr>, kNumChannels> pool;
            const int channels[] { 0, 1, 2, 3, kMicChannel };
            int created = 0;

            for (int iteration = 0; iteration < 2000; ++iteration)
            {
                const int channel = channels[rng() % 5];
                std::vector<FxSlotPtr> slots;

                // Keep some of the channel's current slots (reorder / neighbour added), like AppController does.
                for (auto& existing : pool[(size_t) channel])
                    if (rng() % 3 != 0)
                        slots.push_back (existing);

                const int add = (int) (rng() % 3);

                for (int i = 0; i < add && slots.size() < 5; ++i)
                {
                    std::shared_ptr<juce::AudioProcessor> processor;
                    const auto pick = rng() % 12;

                    if (pick == 0)              processor = BuiltinFx::create ("noise");
                    else if (pick == 1)         processor = std::make_shared<HalvingProcessor>();
                    else if (pick == 2)         processor = std::make_shared<MisbehavingProcessor>();
                    else if (pick == 3)         processor = std::make_shared<DestructionProbeProcessor>();
                    else                        processor = BuiltinFx::create (ids[rng() % 8]);

                    everything.push_back (processor);
                    ++created;
                    slots.push_back (makeSlot (processor, "s" + juce::String (created), (uint32_t) (100 + created)));
                }

                std::shuffle (slots.begin(), slots.end(), rng);

                for (auto& s : slots)
                    s->bypassed.store (rng() % 5 == 0);

                pool[(size_t) channel] = slots;
                engine.setChain (channel, slots.empty() ? nullptr : FxChain::create (slots));

                // Poke parameters of the live built-ins while audio is running.
                if (iteration % 7 == 0)
                    for (auto& s : slots)
                        if (auto* builtin = dynamic_cast<BuiltinProcessor*> (&s->getProcessor()))
                            if (auto* first = builtin->getParameters().getFirst())
                                first->setValueNotifyingHost ((float) (rng() % 1000) / 999.0f);

                // Let the RT thread complete at least one cycle between swaps, so snapshots are replaced at all
                // sorts of moments relative to a running cycle (before, inside, after).
                if (iteration % 2 == 0)
                {
                    const auto before = rt.blocks.load();
                    const auto deadline = juce::Time::getMillisecondCounter() + 30u;

                    while (rt.blocks.load() == before && juce::Time::getMillisecondCounter() < deadline)
                        std::this_thread::yield();
                }

                engine.collectGarbage();
            }

            const auto blocksAtEnd = rt.blocks.load();
            rt.stop();

            logMessage ("swap stress: " + juce::String (created) + " processors created, RT thread ran " + juce::String ((juce::int64) blocksAtEnd)
                        + " blocks (largest " + juce::String (rt.biggestBlock.load()) + " frames)");
            expect (blocksAtEnd > 50, "the RT thread made progress (" + juce::String ((juce::int64) blocksAtEnd) + " blocks)");
            expectEquals ((int) rt.badBlocks.load(), 0, "every output block was finite and at most -0.3 dBFS");

            // Everything left is released together with the engine.
            for (auto& v : pool)
                v.clear();
        }

        int leaked = 0;

        for (auto& w : everything)
            leaked += w.expired() ? 0 : 1;

        expectEquals (leaked, 0, "no processor outlives the engine (no leak)");
        expect (! everything.empty());
        expect (DestructionProbeProcessor::destroyed.load() > 20, "the destruction probe was exercised");
        expectEquals (DestructionProbeProcessor::destroyedElsewhere.load(), 0, "snapshots were only ever released on the message thread");
        expectEquals (DestructionProbeProcessor::usedAfterFree.load(), 0, "and never while the RT thread could still be running them");
    }

    void testRetiredWhileBusy()
    {
        AudioEngine engine;
        engine.prepare (kRate);
        engine.setChannelActive (0, true);

        auto blocking = std::make_shared<BlockingProcessor>();
        std::weak_ptr<juce::AudioProcessor> weak = blocking;
        auto* raw = blocking.get();
        auto slot = makeSlot (blocking, "blk", 7);
        blocking.reset();

        engine.setChain (0, FxChain::create ({ slot }));
        slot.reset();

        EngineHarness feed;
        feed.setAppInput (0, 0.1f);
        auto io = feed.makeIO (256);

        std::thread rt ([&] { engine.process (io); });
        expect (waitUntil ([raw] { return raw->entered.load(); }, 2000), "the RT thread is inside the plugin");

        // Replace the chain while the RT thread is inside it: the old snapshot must stay alive.
        engine.setChain (0, nullptr);
        engine.collectGarbage();
        expect (! weak.expired(), "retired snapshot is kept while the RT thread is busy");

        juce::Thread::sleep (20);
        engine.collectGarbage();
        expect (! weak.expired(), "and still kept a bit later");

        raw->release.store (true);
        rt.join();

        engine.collectGarbage();
        expect (weak.expired(), "released once the RT thread has left");
    }

    void testRateChangeLive()
    {
        AudioEngine engine;
        engine.prepare (kRate);

        for (int k = 0; k < 4; ++k)
            engine.setChannelActive (k, true);

        // A hosted-style plugin plus built-ins on an app channel; the default-like mic chain on the mic.
        auto halver = std::make_shared<HalvingProcessor>();
        auto halverSlot = makeSlot (halver, "h", 1);
        auto eqSlot = makeSlot (BuiltinFx::create ("eq"), "e", 2);
        auto limitSlot = makeSlot (BuiltinFx::create ("limit"), "l", 3);
        engine.setChain (0, FxChain::create ({ eqSlot, halverSlot, limitSlot }));

        auto noiseSlot = makeSlot (BuiltinFx::create ("noise"), "n", 4);
        auto compSlot = makeSlot (BuiltinFx::create ("comp"), "c", 5);
        engine.setChain (kMicChannel, FxChain::create ({ noiseSlot, compSlot }));

        auto* noise = dynamic_cast<NoiseRemoval*> (&noiseSlot->getProcessor());
        auto* limiter = &limitSlot->getProcessor();
        expect (noise != nullptr);
        const auto* halverBefore = &halverSlot->getProcessor();
        const int noiseLatency48 = noise->getLatencySamples();
        const int limiterLatency48 = limiter->getLatencySamples();

        RtRunner rt (engine);
        rt.start();
        expect (waitUntil ([&] { return rt.blocks.load() > 20; }, 2000));

        for (double target : { 44100.0, 48000.0, 96000.0, 48000.0 })
        {
            rt.rate.store (target);
            expect (waitUntil ([&] { return engine.requestedRate.load() == target; }, 2000), "RT thread asks for " + juce::String (target));
            engine.requestedRate.store (0.0);

            expect (engine.prepare (target), "prepare succeeds with the RT thread running");
            expectEquals (engine.getPreparedRate(), target);
            expectEquals (halver->lastPreparedRate, target);
            expect (&halverSlot->getProcessor() == halverBefore, "the same processor instance is re-prepared, not re-created");

            // Rate dependent state moved with it: latency of the noise removal and of the limiter.
            if (target == 48000.0)
            {
                expectEquals (noise->getLatencySamples(), noiseLatency48);
                expectEquals (limiter->getLatencySamples(), limiterLatency48);
            }
            else
            {
                expect (noise->getLatencySamples() != noiseLatency48, "noise removal adapted to " + juce::String (target));
                expectEquals (limiter->getLatencySamples(), (int) std::lround (0.0015 * target));
            }

            // Audio resumes: wait for several audible blocks.
            const auto before = rt.blocks.load();
            expect (waitUntil ([&] { return rt.blocks.load() > before + 20 && rt.lastPeak.load() > 0.0f; }, 3000), "audio resumes at " + juce::String (target));
            expect (engine.getPreparedRate() == target);
        }

        rt.stop();
        expectEquals ((int) rt.badBlocks.load(), 0);
        expectEquals (halver->prepareCount, 5);         // initial + 4 changes

        // The engine is silent, not broken, when the backend runs at a rate it has not been prepared for.
        EngineHarness h;
        h.engine->prepare (kRate);
        h.setAppInput (0, 0.1f);
        h.engine->setChannelActive (0, true);
        h.poisonOutputs();
        h.run (300, 96000.0);
        expectEquals (peakAbs (h.monitorL().data(), 300), 0.0f);
    }

    void testPrepareTimeout()
    {
        AudioEngine engine;
        engine.prepare (kRate);
        engine.setChannelActive (0, true);
        engine.prepareTimeoutMs = 60;

        auto blocking = std::make_shared<BlockingProcessor>();
        auto* raw = blocking.get();
        engine.setChain (0, FxChain::create ({ makeSlot (blocking, "blk", 9) }));

        EngineHarness feed;
        feed.setAppInput (0, 0.1f);
        auto io = feed.makeIO (256);

        std::thread rt ([&] { engine.process (io); });
        expect (waitUntil ([raw] { return raw->entered.load(); }, 2000));

        expect (! engine.prepare (44100.0), "prepare() refuses to touch processors the RT thread is inside");
        expectEquals (engine.getPreparedRate(), kRate);

        raw->release.store (true);
        rt.join();

        expect (engine.prepare (44100.0), "and works as soon as the RT thread has left");
        expectEquals (engine.getPreparedRate(), 44100.0);

        // A running RT thread never blocks prepare().
        engine.setChain (0, nullptr);
        engine.prepareTimeoutMs = 2000;
        RtRunner idleRunner (engine);
        idleRunner.rate.store (44100.0);
        idleRunner.start();
        expect (waitUntil ([&] { return idleRunner.blocks.load() > 10; }, 2000));

        for (int i = 0; i < 20; ++i)
            expect (engine.prepare (44100.0));

        idleRunner.stop();
        expectEquals ((int) idleRunner.badBlocks.load(), 0);
    }

    void testNoAllocation()
    {
        // Positive control: the counting operators really are in place.
        {
            ScopedNoAllocation guard;
            std::vector<int> grown;

            for (int i = 0; i < 100; ++i)
                grown.push_back (i);

            const int last = grown.back();
            grown = {};
            grown.shrink_to_fit();
            expect (last == 99 && guard.allocations() >= 1 && guard.frees() >= 1, "the allocation guard counts new and delete");
        }

        const char* ids[] { "noise", "eq", "comp", "gate", "deess", "limit" };

        for (double rate : { 48000.0, 44100.0, 96000.0 })
        {
            EngineHarness h (16384);
            h.engine->prepare (rate);
            h.engine->setChannelActive (0, true);
            h.engine->setChannelActive (1, true);
            h.setAppSignal (0, makeNoise (16384, 0.3, 1), makeNoise (16384, 0.3, 2));
            h.setAppSignal (1, makeNoise (16384, 0.3, 3), makeNoise (16384, 0.3, 4));
            h.setMicSignal (makeNoise (16384, 0.1, 5));

            std::vector<FxSlotPtr> appSlots, micSlots;
            int uid = 0;

            for (auto* id : ids)
            {
                appSlots.push_back (makeSlot (BuiltinFx::create (id), "a" + juce::String (uid), (uint32_t) uid, rate));
                ++uid;
                micSlots.push_back (makeSlot (BuiltinFx::create (id), "m" + juce::String (uid), (uint32_t) uid, rate));
                ++uid;
            }

            appSlots.push_back (makeSlot (std::make_shared<HalvingProcessor>(), "half", 90, rate));
            appSlots.push_back (makeSlot (std::make_shared<MisbehavingProcessor>(), "bad", 91, rate));      // sanitiser path
            h.engine->setChain (0, FxChain::create (appSlots));
            h.engine->setChain (kMicChannel, FxChain::create (micSlots));

            for (int i = 0; i < 6; ++i)
                h.run (1024, rate);                 // warm up: past every slot's start-up phase

            std::mt19937 rng (7);
            const int sizes[] { 1, 37, 256, 480, 1023, 1024, 1025, 4096, 8192 };
            int allocations = 0, frees = 0, cycles = 0;

            for (int round = 0; round < 120; ++round)
            {
                // Things the message thread does between cycles: parameter moves and bypass switches (fades run in RT).
                for (auto& slot : (round % 2 == 0 ? appSlots : micSlots))
                    if (auto* builtin = dynamic_cast<BuiltinProcessor*> (&slot->getProcessor()))
                        if (rng() % 3 == 0)
                            if (auto* first = builtin->getParameters().getFirst())
                                first->setValueNotifyingHost ((float) (rng() % 100) / 99.0f);

                if (round % 10 == 5)
                    appSlots[rng() % appSlots.size()]->bypassed.store (rng() % 2 == 0);

                if (round % 17 == 3)
                    h.engine->setChannelGainDb (0, Mix::Monitor, -6.0f * (float) (rng() % 4));

                ScopedNoAllocation guard;
                h.run (sizes[rng() % 9], rate);
                allocations += guard.allocations();
                frees += guard.frees();
                ++cycles;
            }

            expectEquals (allocations, 0, "no allocation in " + juce::String (cycles) + " cycles at " + juce::String (rate) + " Hz");
            expectEquals (frees, 0, "no free in " + juce::String (cycles) + " cycles at " + juce::String (rate) + " Hz");
            expect (allFinite (h.monitorL().data(), 512));
        }
    }

    void testSanitising()
    {
        const float inf = std::numeric_limits<float>::infinity();
        const float nan = std::numeric_limits<float>::quiet_NaN();

        for (float bad : { nan, inf, -inf, 1.0e30f, -50.0f })
        {
            EngineHarness h;
            h.engine->prepare (kRate);
            h.engine->setChannelActive (0, true);
            h.engine->setChannelActive (kMicChannel, true);
            h.setAppInput (0, 0.1f);
            h.setMic (0.1f);

            auto misbehaving = std::make_shared<MisbehavingProcessor>();
            misbehaving->badValue = bad;
            auto slot = makeSlot (misbehaving, "bad", 5);
            h.engine->setChain (0, FxChain::create ({ slot }));
            h.engine->setChain (kMicChannel, FxChain::create ({ makeSlot (std::make_shared<MisbehavingProcessor>(), "bad2", 6) }));

            bool finite = true;
            float peak = 0.0f;

            for (int i = 0; i < 30; ++i)
            {
                h.run (1024);

                for (const auto& v : h.outputs)
                {
                    finite = finite && allFinite (v.data(), 1024);
                    peak = juce::jmax (peak, peakAbs (v.data(), 1024));
                }
            }

            expect (finite, "output stays finite with a plugin producing " + juce::String (bad));
            expect (peak <= 8.0f, "and bounded (" + juce::String (peak) + ")");
            expect (slot->getSanitisedBlocks() > 0, "the slot counts the blocks it had to clean up");
        }

        // Non-finite input from a backend buffer: the mix recovers as soon as it goes away (no poisoned state).
        {
            EngineHarness h;
            h.engine->prepare (kRate);
            h.engine->setChannelActive (0, true);
            h.setAppInput (0, 0.1f);
            h.setMic (0.1f);
            h.run (1024);

            h.appBuffers[0][0][500] = nan;
            h.appBuffers[0][1][501] = inf;
            h.micBuffers[0][10] = -inf;
            h.run (1024);
            expect (allFinite (h.monitorL().data(), 1024) && allFinite (h.monitorR().data(), 1024) && allFinite (h.streamL().data(), 1024));
            expect (allFinite (h.micOutL().data(), 1024));

            h.setAppInput (0, 0.1f);
            h.setMic (0.1f);
            h.settle();
            expect (std::abs (h.monitorL().back() - 0.2f) < 1.0e-4f, "and the next clean blocks are exact again (" + juce::String (h.monitorL().back()) + ")");
        }
    }

    void testCrashGuard()
    {
        EngineHarness h;
        h.engine->prepare (kRate);
        h.engine->setChannelActive (0, true);
        h.setAppInput (0, 0.1f);

        auto probeA = std::make_shared<SlotProbeProcessor>();
        auto probeB = std::make_shared<SlotProbeProcessor>();
        h.engine->setChain (0, FxChain::create ({ makeSlot (probeA, "a", 1234u), makeSlot (probeB, "b", 5678u) }));

        CrashGuard::currentRtSlot.store (0);
        h.run (256);

        expectEquals ((int) probeA->seen.load(), 1234);
        expectEquals ((int) probeB->seen.load(), 5678);
        expectEquals ((int) CrashGuard::currentRtSlot.load(), 0, "cleared after the cycle");
        expect (probeA->calls.load() > 0);

        // A bypassed slot is not called at all (and so never blamed).
        h.settle();
        auto chain = h.engine->getChain (0);
        chain->getSlots()[0]->bypassed.store (true);
        h.settle();
        const int callsA = probeA->calls.load();
        h.run (256);
        expectEquals (probeA->calls.load(), callsA, "a fully bypassed slot costs nothing");
    }

    void testBuffers()
    {
        EngineHarness h (16384);
        h.engine->prepare (kRate);
        h.engine->setChannelActive (0, true);
        h.engine->setChannelActive (1, true);
        h.setAppInput (0, 0.2f);
        h.setAppInput (1, 0.1f);
        h.setMic (0.05f);

        // FX on the mic and on a channel so that chunking goes through real processors.
        h.engine->setChain (kMicChannel, FxChain::create ({ makeSlot (BuiltinFx::create ("eq"), "e", 1), makeSlot (BuiltinFx::create ("comp"), "c", 2),
                                                           makeSlot (BuiltinFx::create ("limit"), "l", 3), makeSlot (BuiltinFx::create ("noise"), "n", 4) }));
        h.engine->setChain (0, FxChain::create ({ makeSlot (BuiltinFx::create ("gate"), "g", 5), makeSlot (BuiltinFx::create ("deess"), "d", 6) }));
        h.settle();

        for (int size : { 1, 37, 256, 1023, 1024, 1025, 8192 })
        {
            h.poisonOutputs (7.0f);
            h.run (size);

            for (size_t i = 0; i < h.outputs.size(); ++i)
            {
                bool written = true;

                for (int f = 0; f < size; ++f)
                    written = written && h.outputs[i][(size_t) f] != 7.0f;

                expect (written, "output " + juce::String ((int) i) + " fully written for " + juce::String (size) + " frames");
                expectEquals (h.outputs[i][(size_t) size], 7.0f, "nothing written past " + juce::String (size));
                expect (allFinite (h.outputs[i].data(), size));
            }
        }

        // Frames = 0 and negative: nothing happens.
        h.poisonOutputs (7.0f);
        h.run (0);
        h.run (-5);
        expectEquals (h.monitorL()[0], 7.0f);

        // Only some outputs connected, mic or apps not linked, everything null.
        {
            EngineIO io = h.makeIO (300);
            io.monitorOut[1] = nullptr;
            io.streamOut[0] = io.streamOut[1] = nullptr;
            io.micOut[0] = nullptr;
            io.micIn[0] = nullptr;
            io.micIn[1] = nullptr;
            h.poisonOutputs (7.0f);
            h.engine->process (io);
            expect (allFinite (h.monitorL().data(), 300) && h.monitorL()[0] != 7.0f);
            expectEquals (h.monitorR()[0], 7.0f);
            expectEquals (h.streamL()[0], 7.0f);
            expect (h.micOutR()[0] != 7.0f, "micOut R alone is still written");

            EngineIO nothing;
            nothing.numFrames = 512;
            nothing.sampleRate = kRate;
            h.engine->process (nothing);        // no inputs, no outputs: just must not crash

            EngineIO onlyMic = h.makeIO (128);
            for (auto& pair : onlyMic.appIn)
                pair[0] = pair[1] = nullptr;
            h.engine->process (onlyMic);
            expect (allFinite (h.monitorL().data(), 128));
        }

        // Mono mic and a half-connected app pair.
        {
            EngineIO io = h.makeIO (256);
            io.micIn[1] = nullptr;
            io.appIn[0][1] = nullptr;
            h.poisonOutputs (7.0f);
            h.engine->process (io);
            expect (allFinite (h.monitorL().data(), 256) && allFinite (h.monitorR().data(), 256));
        }
    }

    void testMasterLimiter()
    {
        const float ceiling = dbToLinear (-0.3f);

        // Hot random peaks (two channels summing to +-4.0) never exceed -0.3 dBFS, in either mix, for any block size.
        {
            EngineHarness h (16384);
            h.engine->prepare (kRate);
            h.engine->setChannelActive (0, true);
            h.engine->setChannelActive (1, true);
            h.engine->setChannelActive (kMicChannel, false);

            h.setAppSignal (0, makeNoise (16384, 2.0, 1), makeNoise (16384, 2.0, 2));
            h.setAppSignal (1, makeNoise (16384, 2.0, 3), makeNoise (16384, 2.0, 4));

            float peak = 0.0f;
            std::mt19937 rng (5);

            for (int i = 0; i < 60; ++i)
            {
                h.run (1 + (int) (rng() % 4000));
                peak = juce::jmax (peak, juce::jmax (peakAbs (h.monitorL().data(), 4000), peakAbs (h.monitorR().data(), 4000)),
                                   juce::jmax (peakAbs (h.streamL().data(), 4000), peakAbs (h.streamR().data(), 4000)));
            }

            // (the buffers hold the last block only; use the master meter for the whole run)
            const auto meterM = h.engine->readMasterLevels (Mix::Monitor);
            const auto meterS = h.engine->readMasterLevels (Mix::Stream);
            expect (juce::jmax (meterM.l, meterM.r, meterS.l, meterS.r) <= ceiling + 1.0e-6f,
                    "master never exceeds -0.3 dBFS (" + juce::String (juce::jmax (meterM.l, meterM.r, meterS.l, meterS.r)) + ")");
            expect (peak <= ceiling + 1.0e-6f);
            expect (peak > 0.9f, "and does not squash more than needed");
        }

        // Silence stays exactly silence (no denormals, no residue).
        {
            EngineHarness h;
            h.engine->prepare (kRate);
            h.engine->setChannelActive (0, true);
            h.setAppInput (0, 0.0f);
            h.poisonOutputs();
            for (int i = 0; i < 10; ++i)
                h.run (777);
            expectEquals (peakAbs (h.monitorL().data(), 777), 0.0f);
            expectEquals (peakAbs (h.streamR().data(), 777), 0.0f);
        }

        // Transparent below the ceiling, delayed by the reported latency; the delay does not change with the toggle.
        {
            EngineHarness h;
            h.engine->prepare (kRate);
            h.engine->setChannelActive (0, true);
            h.engine->setChannelActive (kMicChannel, false);
            const int latency = h.engine->getMasterLatencySamples();
            expect (latency > 0 && latency < 200);

            const auto music = makeNoise (4096, 0.6, 8);
            h.setAppSignal (0, music, music);
            h.run (4096);

            bool exact = true;
            for (int i = latency; i < 4096; ++i)
                exact = exact && std::abs (h.monitorL()[(size_t) i] - music[(size_t) (i - latency)]) < 1.0e-7f;

            expect (exact, "the mix is the input delayed by getMasterLatencySamples()");

            for (bool enabled : { false, true, false })
            {
                h.engine->setLimiterEnabled (Mix::Monitor, enabled);
                h.flush (0);
                h.flush (0);
                std::vector<float> impulse (4096, 0.0f);
                impulse[300] = 0.5f;
                h.setAppSignal (0, impulse, impulse);
                h.run (2048);
                int at = -1;

                for (int i = 0; i < 2048; ++i)
                    if (h.monitorL()[(size_t) i] > 0.25f && at < 0)
                        at = i;

                expectEquals (at, 300 + latency, juce::String ("impulse timing with the limiter ") + (enabled ? "on" : "off"));
            }
        }

        // The master limiter does not touch the processed mic.
        {
            EngineHarness h;
            h.engine->prepare (kRate);
            h.setMic (0.5f);
            h.run (512);
            expectEquals (h.micOutL()[0], 0.5f, "micOut is not delayed");
        }
    }

    void testMicWatch()
    {
        EngineHarness h (16384);
        h.engine->prepare (kRate);
        h.engine->setChannelActive (kMicChannel, false);

        // Exact accounting: 100 cycles of 256 frames at 48 kHz = 25600 frames = 533.3 ms.
        h.setMicSilent();
        for (int i = 0; i < 100; ++i)
            h.run (256);

        expectEquals ((int) h.engine->micSilentMs.load(), 533);
        expectEquals ((int) h.engine->micLiveCycles.load(), 100);
        expectEquals ((int) h.engine->micSeenSignal.load(), 0);

        // One sample of signal anywhere in a cycle counts as signal and restarts the count.
        h.micBuffers[0][255] = 1.0e-3f;
        h.run (256);
        expectEquals ((int) h.engine->micSeenSignal.load(), 1);
        expectEquals ((int) h.engine->micSilentMs.load(), 0);
        expectEquals ((int) h.engine->micLiveCycles.load(), 101);

        // Below 1e-9 still counts as exactly zero; NaN is not silence.
        h.micBuffers[0].assign ((size_t) h.capacity, 1.0e-12f);
        h.run (256);
        expect (h.engine->micSilentMs.load() > 0);

        h.micBuffers[0][3] = std::numeric_limits<float>::quiet_NaN();
        h.run (256);
        expectEquals ((int) h.engine->micSilentMs.load(), 0);

        // An unlinked mic is not silent: counters reset, and "seen signal" survives until resetMicWatch().
        h.setMicSilent();
        h.run (256);
        h.unlinkMic();
        h.run (256);
        expectEquals ((int) h.engine->micLiveCycles.load(), 0);
        expectEquals ((int) h.engine->micSilentMs.load(), 0);
        expectEquals ((int) h.engine->micSeenSignal.load(), 1);

        // The right side of a stereo mic counts too, and a rate other than 48 kHz is respected.
        h.engine->resetMicWatch();
        float rightValue = 0.2f;
        h.setMic (0.0f, &rightValue);
        h.run (256);
        expectEquals ((int) h.engine->micSeenSignal.load(), 1);
        expectEquals ((int) h.engine->micSilentMs.load(), 0);

        // Different block sizes in one run accumulate by frames, not by cycles.
        h.engine->resetMicWatch();
        h.setMicSilent();
        h.run (1);
        h.run (1000);
        h.run (8192);
        expectEquals ((int) h.engine->micSilentMs.load(), (int) (9193.0 * 1000.0 / 48000.0));
        expectEquals ((int) h.engine->micLiveCycles.load(), 3);
    }

    // ------------------------------------------------------------------------------------------------
    void testBypass()
    {
        // Crossfade: toggling never steps by more than the ramp allows (0.2 over 10 ms = 4.2e-4 per sample).
        {
            auto halver = std::make_shared<HalvingProcessor>();
            auto slot = makeSlot (halver, "h", 1);
            const int n = 48000;
            std::vector<float> l ((size_t) n, 0.4f), r ((size_t) n, 0.4f);

            auto run = [&] (int from, int to)
            {
                for (int i = from; i < to; i += 64)
                    slot->process (l.data() + i, r.data() + i, juce::jmin (64, to - i));
            };

            // Warm-up first: dry for 40 ms, then the fade to 0.2.
            run (0, 3840);                       // 80 ms
            expectEquals (l[0], 0.4f);
            expectEquals (l[1919], 0.4f);
            expect (l[1920 + 64] < 0.4f, "warm-up over: fading in");
            expect (std::abs (l[3839] - 0.2f) < 1.0e-6f, "fully processed after the fade");

            std::fill (l.begin(), l.end(), 0.4f);
            r = l;
            slot->bypassed.store (true);
            run (0, 4800);
            expectEquals (l[4799], 0.4f);
            expect (l[0] > 0.19f && l[0] < 0.21f, "starts to fade out at once");

            double worstStep = 0.0;

            for (int i = 1; i < 4800; ++i)
                worstStep = juce::jmax (worstStep, (double) std::abs (l[(size_t) i] - l[(size_t) i - 1]));

            expect (worstStep < 5.0e-4, "bypass on is a smooth 10 ms fade (largest step " + juce::String (worstStep) + ")");

            // Back on: no processing effect until warm; the processor is being fed meanwhile.
            std::fill (l.begin(), l.end(), 0.4f);
            r = l;
            slot->bypassed.store (false);
            run (0, 9600);
            expectEquals (l[1919], 0.4f);
            expect (std::abs (l[9599] - 0.2f) < 1.0e-6f);

            // A very short bypass pulse never leaves the slot half-done: it settles back to processed.
            slot->bypassed.store (true);
            std::fill (l.begin(), l.end(), 0.4f);
            r = l;
            run (0, 128);
            slot->bypassed.store (false);
            run (128, 9600);
            expect (std::abs (l[9599] - 0.2f) < 1.0e-6f, "settles processed after a short bypass pulse");
            expect (allFinite (l.data(), n));
        }

        // A bypassed slot never calls the processor; a switched-on one calls it during warm-up.
        {
            auto probe = std::make_shared<SlotProbeProcessor>();
            auto slot = makeSlot (probe, "p", 2);
            std::vector<float> l (256, 0.1f), r (256, 0.1f);
            slot->bypassed.store (true);

            for (int i = 0; i < 20; ++i)
                slot->process (l.data(), r.data(), 256);

            expectEquals (probe->calls.load(), 0);

            slot->bypassed.store (false);
            slot->process (l.data(), r.data(), 256);
            expectEquals (probe->calls.load(), 1);
        }
    }

    void testSlotMisc()
    {
        // Mono-only plugin: fed with L, its output goes to both sides.
        {
            auto slot = makeSlot (std::make_shared<MonoHalver>(), "m", 3);
            expectEquals (slot->getNumInputChannels(), 1);
            expectEquals (slot->getNumOutputChannels(), 1);

            std::vector<float> l (9600, 0.4f), r (9600, 0.1f);

            for (int i = 0; i < 9600; i += 128)
                slot->process (l.data() + i, r.data() + i, 128);

            expect (std::abs (l[9599] - 0.2f) < 1.0e-6f && std::abs (r[9599] - 0.2f) < 1.0e-6f, "mono plugin output is duplicated to R");
        }

        // A plugin that throws is taken out of the path; the audio carries on dry, nothing crashes.
        {
            auto thrower = std::make_shared<ThrowingProcessor>();
            thrower->armed.store (false);
            auto slot = makeSlot (thrower, "t", 4);
            std::vector<float> l (256, 0.3f), r (256, 0.3f);

            slot->process (l.data(), r.data(), 256);
            expect (! slot->hasFaulted());

            thrower->armed.store (true);
            CrashGuard::currentRtSlot.store (0);

            for (int i = 0; i < 5; ++i)
                slot->process (l.data(), r.data(), 256);

            expect (slot->hasFaulted(), "the slot notes that its plugin threw");
            expectEquals ((int) CrashGuard::currentRtSlot.load(), 0);
            expectEquals (l[255], 0.3f);
            expect (allFinite (l.data(), 256));

            // Re-preparing gives it another chance.
            thrower->armed.store (false);
            slot->configure (kRate, kMaxBlock);
            expect (! slot->hasFaulted());
        }

        // Re-prepare restarts the warm-up (silence from the engine while it happens anyway).
        {
            auto halver = std::make_shared<HalvingProcessor>();
            auto slot = makeSlot (halver, "r", 5);
            std::vector<float> l (9600, 0.4f), r = l;

            for (int i = 0; i < 9600; i += 64)
                slot->process (l.data() + i, r.data() + i, 64);

            expect (std::abs (l[9599] - 0.2f) < 1.0e-6f);

            slot->configure (44100.0, kMaxBlock);
            expectEquals (halver->lastPreparedRate, 44100.0);
            std::fill (l.begin(), l.end(), 0.4f);
            r = l;

            for (int i = 0; i < 1024; i += 64)
                slot->process (l.data() + i, r.data() + i, 64);

            expectEquals (l[1000], 0.4f, "dry again while warming up");
        }

        // Blocks larger than the work buffer are split, not overrun.
        {
            auto halver = std::make_shared<HalvingProcessor>();
            auto slot = std::make_shared<FxSlot> (halver, "big", 6u);
            slot->configure (kRate, 256);                       // work buffer of 256 frames
            std::vector<float> l, r;

            for (int i = 0; i < 4; ++i)
            {
                l.assign (20000, 0.4f);
                r = l;
                slot->process (l.data(), r.data(), 20000);
            }

            expect (std::abs (l[19999] - 0.2f) < 1.0e-6f && allFinite (l.data(), 20000));
        }
    }
};

static EngineHardeningTests engineHardeningTests;
}
