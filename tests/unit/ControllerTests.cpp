// Unit tests for AppController: model changes reach the engine, FX chains rebuild, routing rules reach the backend.
#include "Juce.h"

#include "app/AppController.h"
#include "backend/NullBackend.h"
#include "backend/RuleMatcher.h"

#include <array>
#include <vector>

namespace hr
{
namespace
{
    constexpr int kFrames = 2048;

    // Feeds DC into one app slot and returns the monitor mix sample after the smoothers have settled.
    struct Probe
    {
        explicit Probe (AudioEngine& e) : engine (e) {}

        std::array<float, 2> measure (int slot, float dc)
        {
            std::vector<float> in (kFrames, dc), outMonL (kFrames), outMonR (kFrames), outStrL (kFrames), outStrR (kFrames);

            EngineIO io;
            io.numFrames = kFrames;
            io.sampleRate = 48000.0;
            io.appIn[slot][0] = in.data();
            io.appIn[slot][1] = in.data();
            io.monitorOut[0] = outMonL.data();  io.monitorOut[1] = outMonR.data();
            io.streamOut[0] = outStrL.data();   io.streamOut[1] = outStrR.data();

            for (int i = 0; i < 4; ++i)
                engine.process (io);

            return { outMonL.back(), outStrL.back() };
        }

        AudioEngine& engine;
    };
}

class ControllerTests final : public juce::UnitTest
{
public:
    ControllerTests() : juce::UnitTest ("AppController", "headroom") {}

    void runTest() override
    {
        AppController::Options options;
        options.demo = true;            // NullBackend, but never started: no threads in unit tests
        options.persist = false;
        options.startBackend = false;

        AppController controller (options);
        controller.start();

        auto& state = controller.getState();
        auto& engine = controller.getEngine();
        Probe probe (engine);

        beginTest ("start() applies the model to the engine");
        {
            expectEquals (controller.engineIndexFor ("mic"), kMicChannel);
            expectEquals (controller.engineIndexFor ("music"), 2);
            expectEquals (controller.engineIndexFor ("nope"), -1);

            const auto levels = probe.measure (2, 0.1f);
            expectWithinAbsoluteError (levels[0], 0.1f, 1.0e-4f);     // music at 0 dB into headphones
            expectWithinAbsoluteError (levels[1], 0.1f, 1.0e-4f);

            // The default mic chain was instantiated: noise, eq, comp, limit.
            auto micChain = engine.getChain (kMicChannel);
            expect (micChain != nullptr);
            expectEquals (micChain != nullptr ? (int) micChain->getSlots().size() : -1, 4);
        }

        beginTest ("faders, mutes and links reach the engine");
        {
            controller.setChannelLevel ("music", Mix::Monitor, -6.0206f);
            auto levels = probe.measure (2, 0.4f);
            expectWithinAbsoluteError (levels[0], 0.2f, 1.0e-3f);
            expectWithinAbsoluteError (levels[1], 0.4f, 1.0e-3f);      // stream mix is separate

            controller.toggleMute ("music", Mix::Stream);
            levels = probe.measure (2, 0.4f);
            expectWithinAbsoluteError (levels[1], 0.0f, 1.0e-5f);
            expect (AppState::describe (state.findChannel ("music")).streamMuted);

            controller.setChannelLinked ("music", true);
            controller.setChannelLevel ("music", Mix::Monitor, -12.0412f);
            expectWithinAbsoluteError (AppState::describe (state.findChannel ("music")).streamDb, -12.0412f, 1.0e-3f);

            controller.setChannelLevel ("music", Mix::Monitor, -100.0f);
            levels = probe.measure (2, 0.4f);
            expectWithinAbsoluteError (levels[0], 0.0f, 1.0e-6f);

            controller.setMasterMuted (Mix::Monitor, true);
            controller.setChannelLevel ("chat", Mix::Monitor, 0.0f);
            levels = probe.measure (3, 0.1f);
            expectWithinAbsoluteError (levels[0], 0.0f, 1.0e-6f);
            controller.setMasterMuted (Mix::Monitor, false);
        }

        beginTest ("adding and removing channels");
        {
            const auto id = controller.addChannel();
            expect (id.isNotEmpty());
            const auto slot = controller.engineIndexFor (id);
            expectEquals (slot, 4);

            auto levels = probe.measure (slot, 0.3f);
            expectWithinAbsoluteError (levels[0], 0.3f, 1.0e-4f);

            bool sawEndpoint = false;

            for (const auto& e : state.getChannelEndpoints())
                sawEndpoint = sawEndpoint || (e.channelId == id && e.slot == slot);

            expect (sawEndpoint);

            controller.renameChannel (id, "  Stream Deck ");
            expectEquals (AppState::describe (state.findChannel (id)).name, juce::String ("Stream Deck"));

            controller.removeChannel (id);
            levels = probe.measure (slot, 0.3f);
            expectWithinAbsoluteError (levels[0], 0.0f, 1.0e-6f);     // slot is inactive again
            expect (! state.findChannel (id).isValid());
        }

        beginTest ("effects: add, bypass, move, remove rebuild the chain");
        {
            const auto uid = controller.addBuiltinFx ("chat", "gate");
            expect (uid.isNotEmpty());
            expect (controller.getFxProcessor (uid) != nullptr);
            expectEquals (engine.getChain (3) != nullptr ? (int) engine.getChain (3)->getSlots().size() : -1, 1);

            const auto second = controller.addBuiltinFx ("chat", "deess", 0);
            expectEquals (engine.getChain (3)->getSlots()[0]->getUid(), second);

            controller.moveFx ("chat", second, 1);
            expectEquals (engine.getChain (3)->getSlots()[1]->getUid(), second);
            expect (engine.getChain (3)->getSlots()[0]->getProcessor().getName() == "Noise Gate");

            // Live processors are reused, not re-created, when the chain is rebuilt.
            auto* before = controller.getFxProcessor (uid);
            controller.setFxBypassed ("chat", uid, true);
            expect (engine.getChain (3)->getSlots()[0]->bypassed.load());
            controller.setFxBypassed ("chat", uid, false);
            controller.addBuiltinFx ("chat", "limit");
            expect (controller.getFxProcessor (uid) == before);

            controller.removeFx ("chat", second);
            expect (controller.getFxProcessor (second) == nullptr);
            expectEquals ((int) engine.getChain (3)->getSlots().size(), 2);

            // Disabled (crashed) slots leave the chain until re-enabled.
            state.findSlot (uid).setProperty (ids::disabled, 1, nullptr);
            expectEquals ((int) engine.getChain (3)->getSlots().size(), 1);
            controller.reenableFx (uid);
            expectEquals ((int) engine.getChain (3)->getSlots().size(), 2);

            // Unknown built-ins are refused.
            expect (controller.addBuiltinFx ("chat", "nope").isEmpty());

            // Plugin state is captured into the model.
            controller.captureFxStates (false);
            expect (state.findSlot (uid)[ids::state].toString().isNotEmpty());

            // Removing the channel tears its chain down.
            controller.removeChannel ("chat");
            expect (controller.getFxProcessor (uid) == nullptr);
            expect (engine.getChain (3) == nullptr);
        }

        beginTest ("app routing: routeApp stores a rule and the backend follows the rules");
        {
            auto streams = controller.getBackend().getAppStreams();
            expectEquals ((int) streams.size(), 4);

            auto channelOf = [&] (const juce::String& binary)
            {
                for (const auto& s : controller.getBackend().getAppStreams())
                    if (s.binary == binary)
                        return s.channelId;

                return juce::String ("?");
            };

            // Seeded rules
            expectEquals (channelOf ("spotify"), juce::String ("music"));
            expectEquals (channelOf ("cyberpunk2077.exe"), juce::String ("game"));
            expectEquals (channelOf ("firefox"), juce::String ("system"));     // no rule: fallback channel

            // Move Firefox to Music: remembered as a rule for its binary.
            uint32_t firefoxId = 0;

            for (const auto& s : streams)
                if (s.binary == "firefox")
                    firefoxId = s.nodeId;

            controller.routeApp (firefoxId, "music");
            expectEquals (channelOf ("firefox"), juce::String ("music"));
            expectEquals (matchRoutingRule (state.getRoutingRules(), "firefox", "Firefox"), juce::String ("music"));

            // Removing the rule sends it back to the fallback.
            controller.removeRule ("firefox");
            expectEquals (channelOf ("firefox"), juce::String ("system"));

            // The fallback channel is configurable.
            controller.setFallbackChannel ("game");
            expectEquals (channelOf ("firefox"), juce::String ("game"));
        }

        beginTest ("settings reach the backend");
        {
            controller.setLatency (128);
            expectEquals (controller.getBackendStatus().quantum, 128);

            controller.setOutputDevice ("alsa_output.pci-0000_01_00.1.hdmi-stereo");
            expectEquals (controller.getBackendStatus().outputLabel, juce::String ("HDMI Audio"));
            expectEquals (state.getMix (Mix::Monitor)[ids::outputDevice].toString(), juce::String ("alsa_output.pci-0000_01_00.1.hdmi-stereo"));

            controller.setOutputDevice ("auto");
            expectEquals (controller.getBackendStatus().outputLabel, juce::String ("Elgato Wave:3 Headphones"));
        }

        beginTest ("keepDefault reaches the backend: at start, and when it changes");
        {
            // The demo backend only remembers what it was told.
            auto* backend = dynamic_cast<NullBackend*> (&controller.getBackend());
            expect (backend != nullptr);

            if (backend == nullptr)
                return;

            expect (backend->getMakeDefault());             // start() pushed the defaults: both on
            expect (backend->getKeepDefault());

            controller.setKeepDefault (false);
            expect (! backend->getKeepDefault());
            expect (! state.getKeepDefault());
            expect (backend->getMakeDefault());             // one does not touch the other

            controller.setKeepDefault (true);
            expect (backend->getKeepDefault());

            controller.setMakeDefault (false);
            expect (! backend->getMakeDefault());
            expect (backend->getKeepDefault());             // the lock keeps its own value while it does not apply
            expect (state.getKeepDefault());

            controller.setMakeDefault (true);
            expect (backend->getMakeDefault());

            // Changing the model directly (the way the settings view and a loaded state.xml do) is picked up too.
            state.setSetting (ids::keepDefault, 0);
            expect (! backend->getKeepDefault());
            state.setSetting (ids::keepDefault, 1);
            expect (backend->getKeepDefault());
        }

        beginTest ("a model that starts with keepDefault off starts the backend that way");
        {
            AppController other (options);
            other.getState().setSetting (ids::keepDefault, 0);
            other.getState().setSetting (ids::makeDefault, 1);
            other.start();

            auto* backend = dynamic_cast<NullBackend*> (&other.getBackend());
            expect (backend != nullptr);

            if (backend != nullptr)
            {
                expect (! backend->getKeepDefault());
                expect (backend->getMakeDefault());
            }
        }
    }
};

static ControllerTests controllerTests;
}
