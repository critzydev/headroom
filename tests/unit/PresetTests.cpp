// Unit tests for mic presets and notices on AppController.
#include "Juce.h"

#include "app/AppController.h"
#include "engine/builtin/BuiltinFx.h"
#include "engine/builtin/BuiltinProcessor.h"
#include "engine/builtin/NoiseRemoval.h"
#include "engine/builtin/Presets.h"

namespace hr
{
class PresetTests final : public juce::UnitTest
{
public:
    PresetTests() : juce::UnitTest ("Presets and notices", "headroom") {}

    void runTest() override
    {
        AppController::Options options;
        options.demo = true;
        options.persist = false;
        options.startBackend = false;

        AppController controller (options);
        controller.start();
        auto& state = controller.getState();

        beginTest ("every preset references real built-ins and parameters");
        {
            for (auto& preset : micPresets())
                for (auto& slot : preset.slots)
                {
                    auto processor = BuiltinFx::create (slot.builtinId);
                    expect (processor != nullptr, preset.id + " uses unknown effect " + slot.builtinId);

                    if (processor != nullptr)
                        for (auto& [paramId, value] : slot.params)
                            expect (processor->apvts.getParameter (paramId) != nullptr,
                                    preset.id + ": " + slot.builtinId + " has no parameter " + paramId);
                }
        }

        beginTest ("the first-run mic chain starts Noise Removal on Best");
        {
            NoiseRemoval* noise = nullptr;
            auto fx = AppState::getFx (state.getMicChannel());

            for (int i = 0; i < fx.getNumChildren() && noise == nullptr; ++i)
                noise = dynamic_cast<NoiseRemoval*> (controller.getFxProcessor (fx.getChild (i)[ids::uid].toString()));

            expect (noise != nullptr, "the default mic chain has Noise Removal (is HEADROOM_TEST_NO_FX set?)");

            if (noise != nullptr)
                expectEquals ((int) noise->getRequestedMode(), (int) NoiseRemoval::Mode::best);
        }

        beginTest ("every preset with noise removal sets its mode to Best on purpose");
        {
            int withNoise = 0;

            for (auto& preset : micPresets())
                for (auto& slot : preset.slots)
                    if (slot.builtinId == "noise")
                    {
                        ++withNoise;
                        bool listed = false;

                        for (auto& [paramId, value] : slot.params)
                            if (paramId == "mode")
                            {
                                listed = true;
                                expectEquals ((int) value, (int) NoiseRemoval::Mode::best, preset.id + ": mode");
                            }

                        expect (listed, preset.id + " does not list Noise Removal's mode");
                    }

            expectEquals (withNoise, 3);            // Clean voice, Broadcast, Noisy room

            for (const char* id : { "clean", "broadcast", "noisy" })
            {
                controller.applyMicPreset (id);
                auto chain = AppState::getFx (state.getMicChannel());
                bool seen = false;

                for (int i = 0; i < chain.getNumChildren(); ++i)
                    if (auto* noise = dynamic_cast<NoiseRemoval*> (controller.getFxProcessor (chain.getChild (i)[ids::uid].toString())))
                    {
                        seen = true;
                        expectEquals ((int) noise->getRequestedMode(), (int) NoiseRemoval::Mode::best, juce::String (id) + " applied");
                    }

                expect (seen, juce::String (id) + " puts Noise Removal in the chain");
            }
        }

        beginTest ("applying a preset replaces the mic chain and sets values");
        {
            controller.applyMicPreset ("broadcast");
            auto fx = AppState::getFx (state.getMicChannel());
            expectEquals (fx.getNumChildren(), (int) findMicPreset ("broadcast")->slots.size());

            bool sawEq = false;

            for (int i = 0; i < fx.getNumChildren(); ++i)
            {
                const auto uid = fx.getChild (i)[ids::uid].toString();

                if (auto* builtin = dynamic_cast<BuiltinProcessor*> (controller.getFxProcessor (uid)))
                    if (builtin->getBuiltinId() == "eq")
                    {
                        sawEq = true;
                        expectWithinAbsoluteError (builtin->apvts.getRawParameterValue ("b3Gain")->load(), -2.5f, 0.05f);
                    }
            }

            expect (sawEq, "broadcast preset should create a live EQ");

            controller.applyMicPreset ("off");
            expectEquals (AppState::getFx (state.getMicChannel()).getNumChildren(), 0);
        }

        beginTest ("notices replace by id and run their action once");
        {
            int changes = 0, actions = 0;
            controller.onNoticesChanged = [&] { ++changes; };

            controller.postNotice ({ "crash:x", Notice::Severity::error, "Plugin crashed", "Turn back on", [&] { ++actions; } });
            controller.postNotice ({ "crash:x", Notice::Severity::error, "Plugin crashed again", "Turn back on", [&] { ++actions; } });
            expectEquals ((int) controller.getNotices().size(), 1);
            expectEquals (controller.getNotices().front().text, juce::String ("Plugin crashed again"));

            controller.runNoticeAction ("crash:x");
            expectEquals (actions, 1);
            expect (controller.getNotices().empty());
            expectEquals (changes, 3);

            controller.onNoticesChanged = nullptr;
        }
    }
};

static PresetTests presetTests;
}
