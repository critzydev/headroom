// Unit tests for the built-in effect registry: ids, parameter defaults, state round trip and slot use.
#include "Juce.h"

#include "engine/FxChain.h"
#include "engine/builtin/BuiltinFx.h"

#include <cmath>
#include <map>

namespace hr
{
class BuiltinFxTests final : public juce::UnitTest
{
public:
    BuiltinFxTests() : juce::UnitTest ("BuiltinFx", "headroom") {}

    void runTest() override
    {
        beginTest ("registry lists the six effects in menu order");
        {
            const auto& list = BuiltinFx::list();
            const char* expected[] { "noise", "eq", "comp", "gate", "deess", "limit" };

            expectEquals ((int) list.size(), 6);

            for (size_t i = 0; i < list.size() && i < 6; ++i)
            {
                expectEquals (list[i].id, juce::String (expected[i]));
                expect (list[i].displayName.isNotEmpty() && list[i].description.isNotEmpty());
                expect (BuiltinFx::create (list[i].id) != nullptr);
            }

            expect (BuiltinFx::create ("nope") == nullptr);
            expect (BuiltinFx::find ("nope") == nullptr);
            expectEquals (BuiltinFx::displayNameFor ("noise"), juce::String ("Noise Removal"));
        }

        beginTest ("parameter defaults match the first-run mic chain");
        {
            checkDefaults ("noise", { { "mode", 1.0f }, { "strength", 80.0f }, { "threshold", 60.0f }, { "hold", 150.0f }, { "postFilter", 0.0f } });   // Best, 24 dB
            // Band 1 is the 80 Hz / 12 dB per octave low cut and on; bands 2-8 are off, waiting at 100, 250, 1k, 3k, 6k, 10k and 16 kHz.
            checkDefaults ("eq",    { { "b1On", 1.0f }, { "b1Type", 3.0f }, { "b1Freq", 80.0f }, { "b1Slope", 1.0f },
                                      { "b2On", 0.0f }, { "b3On", 0.0f }, { "b4On", 0.0f }, { "b5On", 0.0f }, { "b6On", 0.0f }, { "b7On", 0.0f }, { "b8On", 0.0f },
                                      { "b2Freq", 100.0f }, { "b3Freq", 250.0f }, { "b4Freq", 1000.0f }, { "b5Freq", 3000.0f },
                                      { "b6Freq", 6000.0f }, { "b7Freq", 10000.0f }, { "b8Freq", 16000.0f },
                                      { "b1Gain", 0.0f }, { "b4Gain", 0.0f }, { "b8Gain", 0.0f } });
            checkDefaults ("comp",  { { "threshold", -20.0f }, { "ratio", 3.0f }, { "attack", 10.0f }, { "release", 160.0f },
                                      { "knee", 6.0f }, { "makeup", 4.0f } });
            checkDefaults ("gate",  { { "threshold", -50.0f }, { "range", 40.0f } });
            checkDefaults ("deess", { { "frequency", 6500.0f }, { "amount", 50.0f } });
            checkDefaults ("limit", { { "ceiling", -1.0f }, { "release", 50.0f } });
        }

        beginTest ("state survives a round trip");
        {
            auto a = BuiltinFx::create ("noise");
            auto* strength = a->apvts.getParameter ("strength");
            expect (strength != nullptr);
            strength->setValueNotifyingHost (strength->convertTo0to1 (30.0f));

            juce::MemoryBlock state;
            a->getStateInformation (state);
            expect (state.getSize() > 0);

            auto b = BuiltinFx::create ("noise");
            b->setStateInformation (state.getData(), (int) state.getSize());
            expectWithinAbsoluteError (b->apvts.getRawParameterValue ("strength")->load(), 30.0f, 0.2f);
            expectWithinAbsoluteError (b->apvts.getRawParameterValue ("hold")->load(), 150.0f, 0.01f);

            // Garbage is ignored rather than crashing.
            const char junk[] = "not a state";
            b->setStateInformation (junk, (int) sizeof (junk));
            expectWithinAbsoluteError (b->apvts.getRawParameterValue ("strength")->load(), 30.0f, 0.2f);
        }

        beginTest ("built-ins run as FX slots (stereo, dry during the warm-up, then processed)");
        {
            std::shared_ptr<juce::AudioProcessor> p = BuiltinFx::create ("eq");
            FxSlot slot (p, "eqxx", 1u);
            slot.configure (48000.0, kMaxBlock);
            expectEquals (slot.getNumInputChannels(), 2);
            expectEquals (slot.getNumOutputChannels(), 2);

            // A new slot lets the dry signal through while it warms up (40 ms), so adding an effect never clicks.
            std::vector<float> l (256, 0.25f), r (256, -0.25f);
            slot.process (l.data(), r.data(), 256);
            expectEquals (l[10], 0.25f);
            expectEquals (r[10], -0.25f);

            // Then the default high-pass is in the path: DC is removed.
            for (int i = 0; i < 200; ++i)
            {
                std::fill (l.begin(), l.end(), 0.25f);
                std::fill (r.begin(), r.end(), -0.25f);
                slot.process (l.data(), r.data(), 256);
            }

            expect (std::abs (l[255]) < 1.0e-3f && std::abs (r[255]) < 1.0e-3f, "the 80 Hz high-pass removes DC");
        }
    }

private:
    void checkDefaults (const juce::String& id, const std::map<juce::String, float>& expected)
    {
        auto fx = BuiltinFx::create (id);

        if (fx == nullptr)
        {
            expect (false, "cannot create " + id);
            return;
        }

        expectEquals (fx->getBuiltinId(), id);

        for (const auto& [paramId, value] : expected)
        {
            auto* raw = fx->apvts.getRawParameterValue (paramId);

            if (raw == nullptr)
                expect (false, id + " is missing parameter " + paramId);
            else
                expectWithinAbsoluteError (raw->load(), value, 1.0e-3f, id + "." + paramId);
        }
    }
};

static BuiltinFxTests builtinFxTests;
}
