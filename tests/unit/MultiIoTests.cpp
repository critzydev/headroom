// Unit tests for the routing of Wave Link 3: input channels (a second mic, a capture card, a line-in) and a mix that plays to
// several outputs at once. The rules behind the PipeWire graph are plain functions and are tested here with
// made-up devices; the model, the engine and the controller are tested with their real classes.
#include "Juce.h"

#include "TestHelpers.h"
#include "app/AppController.h"
#include "app/AppState.h"
#include "backend/DeviceRules.h"
#include "backend/ElgatoModels.h"
#include "backend/NullBackend.h"
#include "ui/setup/SetupChoices.h"

#include <string>
#include <vector>

namespace hr
{
namespace
{
    using namespace devices;
    using test::EngineHarness;
    using test::HalvingProcessor;

    constexpr int kFrames = 4096;

    std::vector<PortFacts> portsOf (std::initializer_list<const char*> channels, uint32_t firstId = 100)
    {
        std::vector<PortFacts> result;
        uint32_t id = firstId;

        for (const auto* channel : channels)
            result.push_back ({ id++, channel });

        return result;
    }

    Candidate candidate (const char* name, int priority, bool real = true, bool wave = false)
    {
        return { name, real, priority, wave };
    }

    const char* const kWave = "alsa_output.usb-Elgato_Systems_Elgato_Wave_3_BS49-00.analog-stereo";
    const char* const kSpeakers = "alsa_output.usb-Generic_USB_Audio-00.analog-stereo";
    const char* const kHdmi = "alsa_output.pci-0000_01_00.1.hdmi-stereo";

    std::vector<Candidate> threeSinks()
    {
        return { candidate (kSpeakers, 1500), candidate (kHdmi, 1000), candidate (kWave, 1200, true, true) };
    }

    std::vector<std::string> idList (std::initializer_list<const char*> list)
    {
        return { list.begin(), list.end() };
    }
}

// =====================================================================================================
// The rules behind the graph
// =====================================================================================================

class MultiIoRulesTests final : public juce::UnitTest
{
public:
    MultiIoRulesTests() : juce::UnitTest ("Several outputs and input slots: rules", "headroom") {}

    void runTest() override
    {
        outputTests();
        inputPortTests();
        planTests();
        gateTests();
        nameTests();
    }

private:
    void outputTests()
    {
        beginTest ("one output, \"auto\": what it always was, an Elgato Wave first");
        {
            const auto result = resolveOutputs (threeSinks(), idList ({ "auto" }), true);
            expectEquals ((int) result.size(), 1);

            if (! result.empty())
            {
                expect (result[0].automatic);
                expectEquals (result[0].index, 2);      // the Wave, though the speakers have the better priority
            }
        }

        beginTest ("one explicit output is honoured; one that is gone falls back to auto, as with a single device before");
        {
            const auto present = resolveOutputs (threeSinks(), idList ({ kHdmi }), true);
            expectEquals ((int) present.size(), 1);
            expectEquals (present.empty() ? -2 : present[0].index, 1);

            const auto gone = resolveOutputs (threeSinks(), idList ({ "alsa_output.gone" }), true);
            expectEquals ((int) gone.size(), 2);                    // the missing one stays in the list (the UI says so) ...
            expectEquals (gone.empty() ? -2 : gone[0].index, -1);
            expectEquals (gone.size() < 2 ? -2 : gone[1].index, 2); // ... and the automatic choice plays in its place
            expect (gone.size() == 2 && gone[1].automatic);
        }

        beginTest ("several outputs play to each device that is there, in order");
        {
            const auto result = resolveOutputs (threeSinks(), idList ({ kWave, kSpeakers }), true);
            expectEquals ((int) result.size(), 2);
            expectEquals (result.empty() ? -2 : result[0].index, 2);
            expectEquals (result.size() < 2 ? -2 : result[1].index, 0);
        }

        beginTest ("a device that is there plays even when another is not, and no automatic one is added");
        {
            const auto result = resolveOutputs (threeSinks(), idList ({ "alsa_output.gone", kSpeakers }), true);
            expectEquals ((int) result.size(), 2);
            expectEquals (result.empty() ? -2 : result[0].index, -1);
            expectEquals (result.size() < 2 ? -2 : result[1].index, 0);
            expect (! result[1].automatic);
        }

        beginTest ("the same device twice (an explicit entry and what \"auto\" means) is one entry");
        {
            const auto result = resolveOutputs (threeSinks(), idList ({ kWave, "auto", kWave }), true);
            expectEquals ((int) result.size(), 1);
            expectEquals (result.empty() ? -2 : result[0].index, 2);

            const auto twiceGone = resolveOutputs (threeSinks(), idList ({ "alsa_output.gone", "alsa_output.gone", kHdmi }), false);
            expectEquals ((int) twiceGone.size(), 2);
        }

        beginTest ("the Stream mix's extra outputs do not fall back: nothing plays there until the device is back");
        {
            const auto gone = resolveOutputs (threeSinks(), idList ({ "alsa_output.gone" }), false);
            expectEquals ((int) gone.size(), 1);
            expectEquals (gone.empty() ? -2 : gone[0].index, -1);

            expect (resolveOutputs (threeSinks(), {}, false).empty());
        }

        beginTest ("no sink at all: \"auto\" has nothing to point at");
        {
            const auto result = resolveOutputs ({}, idList ({ "auto" }), true);
            expectEquals ((int) result.size(), 1);
            expectEquals (result.empty() ? -2 : result[0].index, -1);
        }
    }

    void inputPortTests()
    {
        beginTest ("a mono mic feeds both sides of its slot, whether the channel is mono or stereo");
        {
            for (const bool stereo : { false, true })
            {
                const auto source = pickInputPorts (portsOf ({ "MONO" }), stereo);
                expectEquals ((int) source.left, 100);
                expectEquals ((int) source.right, 100);
            }
        }

        beginTest ("a stereo device as a stereo channel: left and right; as a mono channel: the first channel on both sides");
        {
            const auto both = pickInputPorts (portsOf ({ "FL", "FR" }), true);
            expectEquals ((int) both.left, 100);
            expectEquals ((int) both.right, 101);

            const auto first = pickInputPorts (portsOf ({ "FL", "FR" }), false);
            expectEquals ((int) first.left, 100);
            expectEquals ((int) first.right, 100);
        }

        beginTest ("ports in another order are still found by their channel");
        {
            const auto both = pickInputPorts (portsOf ({ "FR", "FL" }), true);
            expectEquals ((int) both.left, 101);        // FL is the second port
            expectEquals ((int) both.right, 100);

            const auto first = pickInputPorts (portsOf ({ "FR", "FL" }), false);
            expectEquals ((int) first.left, 101);
        }

        beginTest ("an interface with many channels: its first channel, or its first pair");
        {
            const auto ports = portsOf ({ "FL", "FR", "FC", "LFE", "RL", "RR", "SL", "SR" });
            const auto mono = pickInputPorts (ports, false);
            expectEquals ((int) mono.left, 100);
            expectEquals ((int) mono.right, 100);

            const auto stereo = pickInputPorts (ports, true);
            expectEquals ((int) stereo.left, 100);
            expectEquals ((int) stereo.right, 101);

            const auto aux = pickInputPorts (portsOf ({ "AUX0", "AUX1", "AUX2" }), false);
            expectEquals ((int) aux.left, 100);         // no names to go by: the first
        }

        beginTest ("a device without ports gives nothing to link");
        {
            for (const bool stereo : { false, true })
            {
                const auto source = pickInputPorts ({}, stereo);
                expectEquals ((int) source.left, (int) kNoPort);
                expectEquals ((int) source.right, (int) kNoPort);
            }
        }
    }

    void planTests()
    {
        beginTest ("an input slot gets one link per side, a mono source twice from the same port");
        {
            const auto links = planInputSlot ({ 100, 100 }, 500, 501);
            expectEquals ((int) links.size(), 2);

            if (links.size() == 2)
            {
                expectEquals ((int) links[0].out, 100);  expectEquals ((int) links[0].in, 500);
                expectEquals ((int) links[1].out, 100);  expectEquals ((int) links[1].in, 501);
            }

            const auto stereo = planInputSlot ({ 100, 101 }, 500, 501);
            expectEquals ((int) stereo.size(), 2);
            expectEquals (stereo.size() < 2 ? -1 : (int) stereo[1].out, 101);
        }

        beginTest ("a slot or a source that is not there makes no link");
        {
            expect (planInputSlot ({ kNoPort, kNoPort }, 500, 501).empty());
            expectEquals ((int) planInputSlot ({ 100, 100 }, kNoPort, 501).size(), 1);
            expectEquals ((int) planInputSlot ({ 100, kNoPort }, 500, 501).size(), 1);
        }

        beginTest ("fan-out: the engine's left and right to every device, left to left and right to right");
        {
            const auto links = planFanOut (10, 11, { { 20, 21 }, { 30, 31 }, { 40, 41 } });
            expectEquals ((int) links.size(), 6);

            for (size_t device = 0; device < 3 && links.size() == 6; ++device)
            {
                expectEquals ((int) links[device * 2].out, 10);
                expectEquals ((int) links[device * 2].in, (int) (20 + device * 10));
                expectEquals ((int) links[device * 2 + 1].out, 11);
                expectEquals ((int) links[device * 2 + 1].in, (int) (21 + device * 10));
            }
        }

        beginTest ("fan-out: one device is exactly what a single output always got; a device with one port gets the left side");
        {
            const auto one = planFanOut (10, 11, { { 20, 21 } });
            expectEquals ((int) one.size(), 2);

            const auto mono = planFanOut (10, 11, { { 20, kNoPort }, { 30, 31 } });
            expectEquals ((int) mono.size(), 3);

            expect (planFanOut (10, 11, {}).empty());
            expect (planFanOut (kNoPort, kNoPort, { { 20, 21 } }).empty());
            expectEquals ((int) planFanOut (10, kNoPort, { { 20, 21 } }).size(), 1);
        }
    }

    void gateTests()
    {
        const auto wave = elgato::identify (0x0fd9, 0x0070, "alsa_input.usb-Elgato_Systems_Elgato_Wave_3_BS49-00.mono-fallback", "Wave:3");
        const auto wave1 = elgato::identify (0x0fd9, 0x006e, "", "Wave:1");
        const auto yeti = elgato::identify (0xb58e, 0x9e84, "alsa_input.usb-Blue_Microphones_Yeti-00.analog-stereo", "Yeti");
        const auto captureCard = elgato::identify (0x0fd9, 0x7f01, "alsa_input.usb-Elgato_Game_Capture_HD60_X-00.analog-stereo", "Elgato Game Capture HD60 X");

        beginTest ("an input channel gets capture-first only on an Elgato Wave: not on another mic, a capture card or nothing");
        {
            expect (inputNeedsCaptureFirst (wave));
            expect (inputNeedsCaptureFirst (wave1));
            expect (! inputNeedsCaptureFirst (yeti));
            expect (! inputNeedsCaptureFirst (captureCard));
            expect (! inputNeedsCaptureFirst (elgato::identify (-1, -1, "alsa_input.pci-0000_00_1f.3.analog-stereo", "Built-in Audio")));
        }

        beginTest ("the gate is open when there is nothing to wait for");
        {
            CaptureGate gate;
            expect (gate.isOpen());
            expect (gate.update (false, true, false, 0, 0));        // not needed
            expect (gate.update (true, false, false, 0, 10));       // the device is not there: headphone audio must work without it
        }

        beginTest ("playback opens once the capture links exist and buffers flow");
        {
            CaptureGate gate;
            expect (! gate.update (true, true, false, 0, 0));       // not linked yet
            expect (! gate.update (true, true, false, 0, 100));
            expect (! gate.update (true, true, true, 0, 200));      // linked, no buffers yet
            expect (! gate.update (true, true, true, 2, 215));
            expect (gate.update (true, true, true, 3, 230));        // three cycles with a buffer
            expect (gate.update (true, true, true, 3, 240));        // and it stays open
        }

        beginTest ("... or after 500 ms when no buffer is seen");
        {
            CaptureGate gate;
            gate.update (true, true, true, 0, 1000);
            expect (! gate.update (true, true, true, 0, 1000 + CaptureGate::kConfirmMs - 1));
            expect (gate.update (true, true, true, 0, 1000 + CaptureGate::kConfirmMs));
        }

        beginTest ("a capture that cannot be linked does not hold the headphones for ever");
        {
            CaptureGate gate;
            expect (! gate.update (true, true, false, 0, 0));
            expect (! gate.update (true, true, false, 0, CaptureGate::kLinkGiveUpMs - 1));
            expect (gate.update (true, true, false, 0, CaptureGate::kLinkGiveUpMs));
        }

        beginTest ("losing the capture links while confirming starts again; the device going away opens it");
        {
            CaptureGate gate;
            gate.update (true, true, true, 0, 0);
            expect (! gate.update (true, true, false, 0, 100));     // back to linking
            expect (! gate.update (true, true, true, 1, 200));      // confirming again, from now
            expect (gate.update (true, true, true, 3, 210));

            CaptureGate other;
            expect (! other.update (true, true, false, 0, 0));
            expect (other.update (true, false, false, 0, 50));      // unplugged: nothing to wait for
            expect (! other.update (true, true, false, 0, 60));     // plugged in again: a fresh start
        }
    }

    void nameTests()
    {
        beginTest ("two nodes of one card are told apart from nodes of another by their names");
        {
            expectEquals (cardOfNodeName ("alsa_input.usb-Elgato_Systems_Elgato_Wave_3_BS49-00.mono-fallback"),
                          cardOfNodeName ("alsa_output.usb-Elgato_Systems_Elgato_Wave_3_BS49-00.analog-stereo"));
            expect (cardOfNodeName ("alsa_input.usb-Elgato_Systems_Elgato_Wave_3_BS49-00.mono-fallback")
                    != cardOfNodeName ("alsa_input.usb-Elgato_Systems_Elgato_Wave_1_TEST-00.mono-fallback"));
            expect (cardOfNodeName ("headroom.mic").empty());
            expect (cardOfNodeName ("alsa_output.nodots").empty());
        }

        beginTest ("a device that is not plugged in is named from its id");
        {
            expectEquals (friendlyNameFromNodeName ("alsa_output.usb-Generic_USB_Audio-00.analog-stereo"), std::string ("Generic USB Audio"));
            expectEquals (friendlyNameFromNodeName ("alsa_input.usb-Elgato_Systems_Elgato_Wave_3_BS49-00.mono-fallback"), std::string ("Elgato Systems Elgato Wave 3 BS49"));
            expectEquals (friendlyNameFromNodeName ("some.other.node"), std::string ("some.other.node"));
        }
    }
};

static MultiIoRulesTests multiIoRulesTests;

// =====================================================================================================
// Input channels in the engine
// =====================================================================================================

class MultiIoEngineTests final : public juce::UnitTest
{
public:
    MultiIoEngineTests() : juce::UnitTest ("Input channels in the engine", "headroom") {}

    void runTest() override
    {
        beginTest ("an input slot mixes into both mixes like an app slot");
        testMix();

        beginTest ("the two mixes and the mutes of an input channel are its own");
        testIndependentMixes();

        beginTest ("every input slot has its own pair, eight of them sum");
        testAllSlots();

        beginTest ("an input channel runs its own effects");
        testEffects();

        beginTest ("inactive or unlinked slots add nothing, and a mixer without inputs is bit for bit what it was");
        testNoInputs();

        beginTest ("the live-cycle counters follow what is linked");
        testLiveCycles();

        beginTest ("input slots are chunked like the others, and never reach the processed mic");
        testChunkingAndMicOut();

        beginTest ("meters of an input channel");
        testMeters();
    }

private:
    static constexpr double kRate = test::kRate;

    static void prepared (EngineHarness& h)
    {
        h.engine->prepare (kRate);
        h.engine->setChannelActive (kMicChannel, false);        // keep the mic out of these tests
    }

    void expectAll (const std::vector<float>& v, int from, int to, float expected, float tolerance, const juce::String& what)
    {
        float worst = 0.0f;

        for (int i = from; i < to; ++i)
            worst = juce::jmax (worst, std::abs (v[(size_t) i] - expected));

        expect (worst <= tolerance, what + ": worst error " + juce::String (worst) + " vs expected " + juce::String (expected));
    }

    void testMix()
    {
        EngineHarness h;
        prepared (h);
        h.engine->setChannelActive (kInputChannel0, true);
        h.engine->setChannelActive (0, true);
        h.setInputInput (0, 0.1f, 0.25f);
        h.setAppInput (0, 0.2f, 0.2f);

        h.settle();
        expectAll (h.monitorL(), 0, kFrames, 0.3f, 1.0e-5f, "monitor L = app + input");
        expectAll (h.monitorR(), 0, kFrames, 0.45f, 1.0e-5f, "monitor R = app + input");
        expectAll (h.streamL(), 0, kFrames, 0.3f, 1.0e-5f, "stream L");

        h.engine->setChannelGainDb (kInputChannel0, Mix::Monitor, -6.0206f);
        h.settle();
        expectAll (h.monitorL(), 0, kFrames, 0.25f, 1.0e-4f, "monitor L with the input at -6 dB");
        expectAll (h.streamL(), 0, kFrames, 0.3f, 1.0e-5f, "stream L untouched");
    }

    void testIndependentMixes()
    {
        EngineHarness h;
        prepared (h);
        h.engine->setChannelActive (kInputChannel0 + 2, true);
        h.setInputInput (2, 0.2f);

        h.engine->setChannelMuted (kInputChannel0 + 2, Mix::Monitor, true);
        h.settle();
        expectAll (h.monitorL(), 0, kFrames, 0.0f, 1.0e-6f, "muted in the Headphones mix");
        expectAll (h.streamL(), 0, kFrames, 0.2f, 1.0e-5f, "still in the Stream mix");

        h.engine->setChannelMuted (kInputChannel0 + 2, Mix::Monitor, false);
        h.engine->setChannelGainDb (kInputChannel0 + 2, Mix::Stream, kMinusInfDb);
        h.settle();
        expectAll (h.monitorL(), 0, kFrames, 0.2f, 1.0e-5f, "back in the Headphones mix");
        expectAll (h.streamL(), 0, kFrames, 0.0f, 1.0e-6f, "-inf in the Stream mix");

        h.engine->setChannelInputTrimDb (kInputChannel0 + 2, -6.0206f);
        h.engine->setChannelGainDb (kInputChannel0 + 2, Mix::Stream, 0.0f);
        h.settle();
        expectAll (h.monitorL(), 0, kFrames, 0.1f, 1.0e-4f, "input trim");
    }

    void testAllSlots()
    {
        EngineHarness h;
        prepared (h);
        float expected = 0.0f;

        for (int k = 0; k < kMaxInputChannels; ++k)
        {
            h.engine->setChannelActive (kInputChannel0 + k, true);
            h.setInputInput (k, 0.01f * (float) (k + 1));
            expected += 0.01f * (float) (k + 1);
        }

        h.settle();
        expectAll (h.monitorL(), 0, kFrames, expected, 1.0e-4f, "all eight inputs");

        // One at a time: the right slot answers to its own channel index.
        h.engine->setChannelActive (kInputChannel0 + 3, false);
        h.settle();
        expectAll (h.monitorL(), 0, kFrames, expected - 0.04f, 1.0e-4f, "input 3 switched off");

        // The app slots are not the input slots.
        h.setAppInput (3, 0.5f);
        h.settle();
        expectAll (h.monitorL(), 0, kFrames, expected - 0.04f, 1.0e-4f, "an app buffer in slot 3 of a channel that is not active adds nothing");
    }

    void testEffects()
    {
        EngineHarness h;
        prepared (h);
        h.engine->setChannelActive (kInputChannel0, true);
        h.setInputInput (0, 0.4f);

        auto slot = std::make_shared<FxSlot> (std::make_shared<HalvingProcessor>(), "half", 1);
        slot->configure (kRate, kMaxBlock);
        h.engine->setChain (kInputChannel0, FxChain::create ({ slot }));

        h.settle();
        expectAll (h.monitorL(), 0, kFrames, 0.2f, 1.0e-4f, "the effect on the input channel halves it");

        // An unlinked input channel with an effect keeps running (a tail rings out), like an app channel.
        h.unlinkInput (0);
        h.settle();
        expectAll (h.monitorL(), 0, kFrames, 0.0f, 1.0e-4f, "unlinked: silence in, silence out");
    }

    void testNoInputs()
    {
        // The reference: the app slots alone.
        EngineHarness reference;
        prepared (reference);
        reference.engine->setChannelActive (0, true);
        reference.engine->setChannelActive (1, true);
        reference.setAppSignal (0, test::makeSine (kFrames, kRate, 440.0, 0.2), test::makeSine (kFrames, kRate, 330.0, 0.2));
        reference.setAppInput (1, 0.1f, 0.05f);

        // The same, with buffers behind input slots that are not active, and with active input slots that are not linked.
        EngineHarness other;
        prepared (other);
        other.engine->setChannelActive (0, true);
        other.engine->setChannelActive (1, true);
        other.setAppSignal (0, test::makeSine (kFrames, kRate, 440.0, 0.2), test::makeSine (kFrames, kRate, 330.0, 0.2));
        other.setAppInput (1, 0.1f, 0.05f);
        other.setInputInput (0, 0.3f);
        other.setInputInput (7, 0.3f);
        other.engine->setChannelActive (kInputChannel0 + 3, true);      // active, nothing linked, no effects

        for (int i = 0; i < 3; ++i)
        {
            reference.run (kFrames);
            other.run (kFrames);

            expect (reference.monitorL() == other.monitorL() && reference.monitorR() == other.monitorR(), "Headphones mix is bit for bit the same");
            expect (reference.streamL() == other.streamL() && reference.streamR() == other.streamR(), "Stream mix is bit for bit the same");
        }
    }

    void testLiveCycles()
    {
        EngineHarness h;
        prepared (h);
        expectEquals ((int) h.engine->inputLiveCycles[2].load(), 0);

        h.setInputInput (2, 0.1f);
        h.run (256);
        h.run (256);
        h.run (256);
        expectEquals ((int) h.engine->inputLiveCycles[2].load(), 3);
        expectEquals ((int) h.engine->inputLiveCycles[1].load(), 0);       // nothing linked in the others

        h.unlinkInput (2);
        h.run (256);
        expectEquals ((int) h.engine->inputLiveCycles[2].load(), 0);       // an unlinked slot starts again
    }

    void testChunkingAndMicOut()
    {
        EngineHarness h (4 * kMaxBlock + 100);
        prepared (h);
        h.engine->setChannelActive (kInputChannel0 + 1, true);

        // A ramp: a chunk that started from the wrong place would show as a jump.
        std::vector<float> ramp ((size_t) h.capacity);

        for (size_t i = 0; i < ramp.size(); ++i)
            ramp[i] = 0.00005f * (float) i;

        h.setInputSignal (1, ramp, ramp);
        h.settle();
        h.run (h.capacity);

        const int latency = h.engine->getMasterLatencySamples();
        float worst = 0.0f;

        for (int i = latency; i < h.capacity; ++i)
            worst = juce::jmax (worst, std::abs (h.monitorL()[(size_t) i] - ramp[(size_t) (i - latency)]));

        expect (worst < 1.0e-4f, "a ramp across five chunks comes out as a ramp (worst error " + juce::String (worst) + ")");
        expectEquals (test::peakAbs (h.micOutL().data(), h.capacity), 0.0f);        // only the mic feeds "Headroom Mic"
    }

    void testMeters()
    {
        EngineHarness h;
        prepared (h);
        h.engine->setChannelActive (kInputChannel0, true);
        h.setInputInput (0, 0.5f, 0.25f);
        h.settle();

        const auto levels = h.engine->readChannelLevels (kInputChannel0);
        expectWithinAbsoluteError (levels.input.l, 0.5f, 1.0e-4f);
        expectWithinAbsoluteError (levels.input.r, 0.25f, 1.0e-4f);
        expectWithinAbsoluteError (levels.monitor.l, 0.5f, 1.0e-4f);
        expectWithinAbsoluteError (levels.stream.r, 0.25f, 1.0e-4f);
    }
};

static MultiIoEngineTests multiIoEngineTests;

// =====================================================================================================
// The model
// =====================================================================================================

class MultiIoModelTests final : public juce::UnitTest
{
public:
    MultiIoModelTests() : juce::UnitTest ("Input channels and outputs in the model", "headroom") {}

    void runTest() override
    {
        beginTest ("a new model has no input channel and one automatic output");
        testDefaults();

        beginTest ("an old file with one output device becomes a list of one, and nothing else changes");
        testMigration();

        beginTest ("the output list: repeats go, the Headphones list is never empty, the Stream list has no auto, the old properties follow");
        testOutputs();

        beginTest ("input channels: own slots, round trip, limits");
        testInputChannels();

        beginTest ("loaded input channels are repaired");
        testRepair();
    }

private:
    static juce::File makeTempDir()
    {
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("headroom-test-" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()));
        dir.createDirectory();
        return dir;
    }

    void testDefaults()
    {
        AppState state;

        for (const auto& info : state.getChannelInfos())
            expect (! info.isInput(), info.id);

        expectEquals ((int) state.getInputEndpoints().size(), 0);
        expectEquals (state.nextFreeInputSlot(), 0);
        expectEquals (state.getOutputs (Mix::Monitor).joinIntoString ("|"), juce::String ("auto"));
        expect (state.getOutputs (Mix::Stream).isEmpty());
        expectEquals (state.getOutputDevice(), juce::String ("auto"));
        expectEquals (state.getMix (Mix::Monitor)[ids::outputs].toString(), juce::String ("auto"));
        expectEquals (state.getMix (Mix::Monitor)[ids::outputDevice].toString(), juce::String ("auto"));
        expect (state.getMix (Mix::Stream).hasProperty (ids::outputs));
        expectEquals (state.getMix (Mix::Stream)[ids::outputs].toString(), juce::String());
    }

    void testMigration()
    {
        auto dir = makeTempDir();
        const auto file = dir.getChildFile ("state.xml");

        // A file as the last release wrote it: one output device in SETTINGS and on the monitor MIX, no input channels.
        file.replaceWithText (
            "<HEADROOM version=\"1\">"
              "<SETTINGS inputDevice=\"auto\" outputDevice=\"alsa_output.usb-Generic_USB_Audio-00.analog-stereo\" latency=\"128\"/>"
              "<MIXES>"
                "<MIX id=\"monitor\" gainDb=\"-3\" muted=\"0\" limiter=\"1\" outputDevice=\"alsa_output.usb-Generic_USB_Audio-00.analog-stereo\"/>"
                "<MIX id=\"stream\" gainDb=\"0\" muted=\"0\" limiter=\"1\"/>"
              "</MIXES>"
              "<CHANNELS>"
                "<CHANNEL id=\"mic\" kind=\"mic\" name=\"Mic\" slot=\"-1\"/>"
                "<CHANNEL id=\"music\" kind=\"app\" name=\"Music\" colour=\"3FB8AF\" icon=\"music\" slot=\"2\"/>"
                "<CHANNEL id=\"chat\" kind=\"app\" name=\"Chat\" colour=\"E0708F\" icon=\"message\" slot=\"3\"/>"
              "</CHANNELS>"
            "</HEADROOM>");

        AppState state (file);
        expect (state.load() == AppState::LoadResult::Loaded);

        expectEquals (state.getOutputs (Mix::Monitor).joinIntoString ("|"), juce::String ("alsa_output.usb-Generic_USB_Audio-00.analog-stereo"));
        expectEquals (state.getOutputDevice(), juce::String ("alsa_output.usb-Generic_USB_Audio-00.analog-stereo"));
        expect (state.getOutputs (Mix::Stream).isEmpty());
        expectEquals ((double) state.getMix (Mix::Monitor)[ids::gainDb], -3.0);        // the rest of the mix is as it was

        const auto channels = state.getChannelInfos();
        expectEquals ((int) channels.size(), 3);
        expectEquals ((int) state.getInputEndpoints().size(), 0);                      // no input channels appear by themselves

        for (const auto& info : channels)
            expect (! info.isInput(), info.id);

        expectEquals (state.findChannel ("music")[ids::slot].operator int(), 2);       // the slots are where they were
        expectEquals (state.findChannel ("chat")[ids::slot].operator int(), 3);
        expectEquals (state.nextFreeSlot(), 0);

        // The model saves what it migrated and reads it back as it is.
        expect (state.saveNow());
        AppState again (file);
        expect (again.load() == AppState::LoadResult::Loaded);
        expectEquals (again.getOutputs (Mix::Monitor).joinIntoString ("|"), juce::String ("alsa_output.usb-Generic_USB_Audio-00.analog-stereo"));
        expect (file.loadFileAsString().contains ("outputs=\"alsa_output.usb-Generic_USB_Audio-00.analog-stereo\""));

        // SETTINGS is what the backend was driven from: when the two old properties disagree it wins.
        file.replaceWithText ("<HEADROOM version=\"1\"><SETTINGS outputDevice=\"settings-one\"/><MIXES><MIX id=\"monitor\" outputDevice=\"mix-one\"/></MIXES></HEADROOM>");
        {
            AppState s (file);
            s.load();
            expectEquals (s.getOutputDevice(), juce::String ("settings-one"));
        }

        // No output at all in the file: automatic. An explicit list is kept as it is (with the first mirrored for old readers).
        file.replaceWithText ("<HEADROOM version=\"1\"><SETTINGS/></HEADROOM>");
        {
            AppState s (file);
            s.load();
            expectEquals (s.getOutputs (Mix::Monitor).joinIntoString ("|"), juce::String ("auto"));
        }

        file.replaceWithText ("<HEADROOM version=\"1\"><SETTINGS outputDevice=\"old\"/><MIXES>"
                              "<MIX id=\"monitor\" outputDevice=\"old\" outputs=\"a|b| |a|auto\"/><MIX id=\"stream\" outputs=\"c|auto|c\"/></MIXES></HEADROOM>");
        {
            AppState s (file);
            s.load();
            expectEquals (s.getOutputs (Mix::Monitor).joinIntoString ("|"), juce::String ("a|b|auto"));
            expectEquals (s.getOutputs (Mix::Stream).joinIntoString ("|"), juce::String ("c"));
            expectEquals (s.getOutputDevice(), juce::String ("a"));
            expectEquals (s.getSetting (ids::outputDevice).toString(), juce::String ("a"));
        }

        dir.deleteRecursively();
    }

    void testOutputs()
    {
        AppState state;

        state.setOutputs (Mix::Monitor, juce::StringArray { "a", "b", "a", "  ", "c " });
        expectEquals (state.getOutputs (Mix::Monitor).joinIntoString ("|"), juce::String ("a|b|c"));
        expectEquals (state.getOutputDevice(), juce::String ("a"));
        expectEquals (state.getSetting (ids::outputDevice).toString(), juce::String ("a"));
        expectEquals (state.getMix (Mix::Monitor)[ids::outputDevice].toString(), juce::String ("a"));

        state.setOutputs (Mix::Monitor, {});
        expectEquals (state.getOutputs (Mix::Monitor).joinIntoString ("|"), juce::String ("auto"));
        expectEquals (state.getOutputDevice(), juce::String ("auto"));

        state.setOutputs (Mix::Stream, juce::StringArray { "auto", "x", "y" });
        expectEquals (state.getOutputs (Mix::Stream).joinIntoString ("|"), juce::String ("x|y"));
        state.setOutputs (Mix::Stream, {});
        expect (state.getOutputs (Mix::Stream).isEmpty());
        expectEquals (state.getOutputDevice(), juce::String ("auto"));      // the Stream list never touches the headphones' mirror

        const auto parsed = AppState::splitOutputs (" a | b||a|c ");
        expectEquals (parsed.joinIntoString ("|"), juce::String ("a|b|c"));
        expectEquals (AppState::joinOutputs (parsed), juce::String ("a|b|c"));
        expect (AppState::splitOutputs ({}).isEmpty());
    }

    void testInputChannels()
    {
        AppState state;

        const auto first = state.addInputChannel ("alsa_input.usb-Generic_USB_Audio-00.analog-stereo", "USB Audio", true);
        expect (first.isValid());
        expectEquals (first[ids::kind].toString(), values::kindInput);
        expectEquals (first[ids::slot].operator int(), 0);
        expectEquals (first[ids::device].toString(), juce::String ("alsa_input.usb-Generic_USB_Audio-00.analog-stereo"));
        expectEquals (first[ids::channels].toString(), values::inputStereo);
        expectEquals (AppState::describe (first).monitorDb, kMinusInfDb);  // silent in the headphones (no feedback), heard in Stream
        expectEquals (AppState::describe (first).streamDb, 0.0f);
        expect (AppState::describe (first).isInput() && ! AppState::describe (first).isApp() && ! AppState::describe (first).isMic());
        expectEquals (AppState::getFx (first).getNumChildren(), 0);

        const auto second = state.addInputChannel ("alsa_input.pci-0000_00_1f.3.analog-stereo", "", false);
        expectEquals (second[ids::slot].operator int(), 1);
        expectEquals (second[ids::name].toString(), juce::String ("Input 2"));
        expectEquals (second[ids::channels].toString(), values::inputMono);
        expect (first[ids::id].toString() != second[ids::id].toString());
        expect (first[ids::colour].toString() != second[ids::colour].toString());

        expect (! state.addInputChannel ("", "No device", false).isValid());     // nothing to listen to

        // The two numberings do not touch: the app slots are all free but the shipped four, whatever the inputs hold.
        expectEquals (state.nextFreeSlot(), 4);
        expectEquals (state.nextFreeInputSlot(), 2);
        expectEquals ((int) state.getChannelEndpoints().size(), 4);             // the app channels only
        expectEquals ((int) state.getInputEndpoints().size(), 2);

        const auto endpoints = state.getInputEndpoints();

        if (endpoints.size() == 2)
        {
            expectEquals (endpoints[0].deviceId, juce::String ("alsa_input.usb-Generic_USB_Audio-00.analog-stereo"));
            expect (endpoints[0].stereo && ! endpoints[1].stereo);
            expectEquals (endpoints[1].slot, 1);
        }

        // Filling every slot: 16 app channels and 8 inputs at the same time.
        while (state.nextFreeSlot() >= 0)
            state.addAppChannel();

        while (state.nextFreeInputSlot() >= 0)
            expect (state.addInputChannel ("alsa_input.dev" + juce::String (state.nextFreeInputSlot()), "In", false).isValid());

        expectEquals (state.nextFreeSlot(), -1);
        expectEquals (state.nextFreeInputSlot(), -1);
        expect (! state.addInputChannel ("alsa_input.one-too-many", "Nine", false).isValid());
        expect (! state.addAppChannel().isValid());
        expectEquals (state.getChannels().getNumChildren(), 1 + kMaxAppChannels + kMaxInputChannels);

        // Removing one frees its slot, and only an input slot.
        expect (state.removeChannel (first[ids::id].toString()));
        expectEquals (state.nextFreeInputSlot(), 0);
        expectEquals (state.nextFreeSlot(), -1);

        // A round trip through the file.
        auto dir = makeTempDir();
        const auto file = dir.getChildFile ("state.xml");
        AppState saved (file);
        const auto added = saved.addInputChannel ("alsa_input.usb-Capture-00.analog-stereo", "Capture", true);
        saved.setOutputs (Mix::Monitor, juce::StringArray { "alsa_output.a", "alsa_output.b" });
        saved.setOutputs (Mix::Stream, juce::StringArray { "alsa_output.c" });
        expect (saved.saveNow());

        AppState loaded (file);
        expect (loaded.load() == AppState::LoadResult::Loaded);
        const auto back = AppState::describe (loaded.findChannel (added[ids::id].toString()));
        expect (back.isInput() && back.stereo);
        expectEquals (back.device, juce::String ("alsa_input.usb-Capture-00.analog-stereo"));
        expectEquals (back.slot, 0);
        expectEquals (back.name, juce::String ("Capture"));
        expectEquals (loaded.getOutputs (Mix::Monitor).joinIntoString ("|"), juce::String ("alsa_output.a|alsa_output.b"));
        expectEquals (loaded.getOutputs (Mix::Stream).joinIntoString ("|"), juce::String ("alsa_output.c"));
        expect (file.loadFileAsString().contains ("outputs=\"alsa_output.a|alsa_output.b\""));

        dir.deleteRecursively();
    }

    void testRepair()
    {
        auto dir = makeTempDir();
        const auto file = dir.getChildFile ("state.xml");

        // Two inputs fighting over slot 1, one with a slot that does not exist, one with no device and a strange channels value,
        // an app channel with a slot an input also has, and a channel called "mic" claiming to be an input.
        file.replaceWithText (
            "<HEADROOM version=\"1\">"
              "<SETTINGS/>"
              "<CHANNELS>"
                "<CHANNEL id=\"mic\" kind=\"input\" name=\"Mic\"/>"
                "<CHANNEL id=\"app\" kind=\"app\" name=\"App\" slot=\"1\"/>"
                "<CHANNEL id=\"a\" kind=\"input\" name=\"A\" device=\"alsa_input.a\" channels=\"stereo\" slot=\"1\"/>"
                "<CHANNEL id=\"b\" kind=\"input\" name=\"B\" device=\"alsa_input.b\" channels=\"mono\" slot=\"1\"/>"
                "<CHANNEL id=\"c\" kind=\"input\" name=\"C\" slot=\"99\" channels=\"surround\"/>"
              "</CHANNELS>"
            "</HEADROOM>");

        AppState state (file);
        expect (state.load() == AppState::LoadResult::Loaded);

        expect (state.getMicChannel().isValid() && AppState::describe (state.getMicChannel()).isMic());      // the mic stays the mic
        expectEquals (AppState::describe (state.findChannel ("app")).slot, 1);                                // app slots are not input slots

        std::set<int> slots;

        for (const auto& endpoint : state.getInputEndpoints())
            slots.insert (endpoint.slot);

        expectEquals ((int) state.getInputEndpoints().size(), 3);
        expectEquals ((int) slots.size(), 3);                                                                // no shared input slot any more
        expect (*slots.rbegin() < kMaxInputChannels);

        const auto c = AppState::describe (state.findChannel ("c"));
        expect (c.isInput() && ! c.stereo && c.device.isEmpty());                                            // "not connected", mono
        expect (AppState::describe (state.findChannel ("a")).stereo);
        expectEquals (AppState::describe (state.findChannel ("a")).device, juce::String ("alsa_input.a"));

        dir.deleteRecursively();
    }
};

static MultiIoModelTests multiIoModelTests;

// =====================================================================================================
// The controller, with the demo backend
// =====================================================================================================

class MultiIoControllerTests final : public juce::UnitTest
{
public:
    MultiIoControllerTests() : juce::UnitTest ("Input channels and outputs in the controller", "headroom") {}

    void runTest() override
    {
        AppController::Options options;
        options.demo = true;            // NullBackend, never started: no threads in unit tests
        options.persist = false;
        options.startBackend = false;

        AppController controller (options);
        controller.start();

        auto& state = controller.getState();
        auto& engine = controller.getEngine();

        const juce::String webcam = "alsa_input.usb-Generic_USB_Webcam_DEMO-02.analog-stereo";
        const juce::String waveOut = "alsa_output.usb-Elgato_Systems_Elgato_Wave_3_DEMO-00.analog-stereo";
        const juce::String hdmi = "alsa_output.pci-0000_01_00.1.hdmi-stereo";

        beginTest ("an input channel reaches the engine, the backend and both mixes");
        {
            const auto id = controller.addInputChannel (webcam, "Webcam");
            expect (id.isNotEmpty());
            expectEquals (controller.engineIndexFor (id), kInputChannel0);

            const auto info = AppState::describe (state.findChannel (id));
            expect (info.isInput() && info.stereo);                       // the demo webcam mic has a left and a right

            const auto status = controller.getBackendStatus();
            expectEquals ((int) status.inputChannels.size(), 1);

            if (! status.inputChannels.empty())
            {
                expectEquals (status.inputChannels[0].channelId, id);
                expectEquals (status.inputChannels[0].label, juce::String ("USB Webcam Mic"));
                expect (status.inputChannels[0].connected);
            }

            // What the backend hands the engine for that slot is mixed at the channel's faders.
            std::vector<float> in (2048, 0.2f), monL (2048), monR (2048), strL (2048), strR (2048);
            EngineIO io;
            io.numFrames = 2048;
            io.sampleRate = 48000.0;
            io.inputIn[0][0] = io.inputIn[0][1] = in.data();
            io.monitorOut[0] = monL.data();  io.monitorOut[1] = monR.data();
            io.streamOut[0] = strL.data();   io.streamOut[1] = strR.data();

            for (int i = 0; i < 4; ++i)
                engine.process (io);

            // A new input starts silent in the headphones (no feedback through speakers) and at 0 dB in the Stream mix.
            expectWithinAbsoluteError (monL.back(), 0.0f, 1.0e-6f);
            expectWithinAbsoluteError (strL.back(), 0.2f, 1.0e-4f);

            controller.setChannelLevel (id, Mix::Monitor, 0.0f);

            for (int i = 0; i < 4; ++i)
                engine.process (io);

            expectWithinAbsoluteError (monL.back(), 0.2f, 1.0e-4f);          // and is heard once its fader comes up

            controller.setChannelLevel (id, Mix::Monitor, -6.0206f);
            controller.toggleMute (id, Mix::Stream);

            for (int i = 0; i < 4; ++i)
                engine.process (io);

            expectWithinAbsoluteError (monL.back(), 0.1f, 1.0e-3f);
            expectWithinAbsoluteError (strL.back(), 0.0f, 1.0e-5f);
        }

        beginTest ("an input channel of a device that is not there says so and has no sound");
        {
            const auto id = controller.addInputChannel ("alsa_input.usb-Not_Plugged_In-00.analog-stereo", "Gone");
            expect (id.isNotEmpty());
            expectEquals (controller.engineIndexFor (id), kInputChannel0 + 1);

            bool found = false;

            for (const auto& input : controller.getBackendStatus().inputChannels)
                if (input.channelId == id)
                {
                    found = true;
                    expect (! input.connected);
                }

            expect (found);

            // "Change input...": the same channel, another device.
            controller.setChannelDevice (id, webcam);
            expectEquals (state.findChannel (id)[ids::device].toString(), webcam);

            for (const auto& input : controller.getBackendStatus().inputChannels)
                if (input.channelId == id)
                    expect (input.connected);

            controller.setChannelStereo (id, false);
            expect (! AppState::describe (state.findChannel (id)).stereo);
            controller.setChannelStereo (id, true);
            expect (AppState::describe (state.findChannel (id)).stereo);
        }

        beginTest ("removing an input channel switches its slot off and tells the backend");
        {
            const auto id = state.getInputEndpoints().front().channelId;
            controller.removeChannel (id);
            expectEquals ((int) state.getInputEndpoints().size(), 1);
            expectEquals ((int) controller.getBackendStatus().inputChannels.size(), 1);

            std::vector<float> in (2048, 0.2f), monL (2048), monR (2048);
            EngineIO io;
            io.numFrames = 2048;
            io.sampleRate = 48000.0;
            io.inputIn[0][0] = io.inputIn[0][1] = in.data();
            io.monitorOut[0] = monL.data();  io.monitorOut[1] = monR.data();

            for (int i = 0; i < 4; ++i)
                engine.process (io);

            expectWithinAbsoluteError (monL.back(), 0.0f, 1.0e-6f);       // slot 0 has no channel any more
        }

        beginTest ("one output: what it always was (the automatic Wave), and setOutputDevice picks one device");
        {
            auto status = controller.getBackendStatus();
            expectEquals ((int) status.headphoneOutputs.size(), 1);
            expect (status.headphoneOutputs.size() == 1 && status.headphoneOutputs[0].automatic && status.headphoneOutputs[0].present);
            expectEquals (status.outputLabel, juce::String ("Elgato Wave:3 Headphones"));
            expect (status.streamOutputs.empty());

            controller.setOutputDevice (hdmi);
            status = controller.getBackendStatus();
            expectEquals ((int) status.headphoneOutputs.size(), 1);
            expectEquals (status.outputLabel, juce::String ("HDMI Audio"));
            expectEquals (state.getMix (Mix::Monitor)[ids::outputDevice].toString(), hdmi);
            expectEquals (state.getSetting (ids::outputDevice).toString(), hdmi);

            controller.setOutputDevice ("auto");
            expectEquals (controller.getBackendStatus().outputLabel, juce::String ("Elgato Wave:3 Headphones"));
        }

        beginTest ("ticking a second device keeps what Automatic plays through and adds the device");
        {
            controller.setOutputSelected (Mix::Monitor, hdmi, true);

            const auto list = state.getOutputs (Mix::Monitor);
            expectEquals (list.joinIntoString ("|"), waveOut + "|" + hdmi);          // Automatic became the device it means
            expectEquals (state.getOutputDevice(), waveOut);

            const auto status = controller.getBackendStatus();
            expectEquals ((int) status.headphoneOutputs.size(), 2);
            expectEquals (status.outputLabel, juce::String ("Elgato Wave:3 Headphones"));

            if (status.headphoneOutputs.size() == 2)
            {
                expectEquals (status.headphoneOutputs[1].label, juce::String ("HDMI Audio"));
                expect (status.headphoneOutputs[1].present && ! status.headphoneOutputs[1].automatic);
            }
        }

        beginTest ("the last device of the Headphones mix cannot be unticked; Automatic is a single choice");
        {
            controller.setOutputSelected (Mix::Monitor, waveOut, false);
            expectEquals (state.getOutputs (Mix::Monitor).joinIntoString ("|"), hdmi);

            controller.setOutputSelected (Mix::Monitor, hdmi, false);              // the last one: nothing happens
            expectEquals (state.getOutputs (Mix::Monitor).joinIntoString ("|"), hdmi);

            controller.setOutputSelected (Mix::Monitor, "auto", true);
            expectEquals (state.getOutputs (Mix::Monitor).joinIntoString ("|"), juce::String ("auto"));

            controller.setOutputSelected (Mix::Monitor, "auto", false);            // and it cannot be unticked either
            expectEquals (state.getOutputs (Mix::Monitor).joinIntoString ("|"), juce::String ("auto"));

            controller.setMixOutputs (Mix::Monitor, {});
            expectEquals (state.getOutputs (Mix::Monitor).joinIntoString ("|"), juce::String ("auto"));
        }

        beginTest ("the Stream mix's extra devices: ticked and unticked freely");
        {
            controller.setOutputSelected (Mix::Stream, hdmi, true);
            expectEquals (state.getOutputs (Mix::Stream).joinIntoString ("|"), hdmi);

            auto status = controller.getBackendStatus();
            expectEquals ((int) status.streamOutputs.size(), 1);
            expect (status.streamOutputs.size() == 1 && status.streamOutputs[0].present);

            controller.setOutputSelected (Mix::Stream, "auto", true);               // there is no automatic extra device
            expectEquals (state.getOutputs (Mix::Stream).joinIntoString ("|"), hdmi);

            controller.setOutputSelected (Mix::Stream, hdmi, false);
            expect (state.getOutputs (Mix::Stream).isEmpty());
            expect (controller.getBackendStatus().streamOutputs.empty());
            expectEquals (state.getOutputs (Mix::Monitor).joinIntoString ("|"), juce::String ("auto"));
        }

        beginTest ("a ticked device that is not there is listed as missing and the others play");
        {
            controller.setMixOutputs (Mix::Monitor, juce::StringArray { "alsa_output.usb-Desk_Speakers_X1-00.analog-stereo", hdmi });
            const auto status = controller.getBackendStatus();
            expectEquals ((int) status.headphoneOutputs.size(), 2);

            if (status.headphoneOutputs.size() == 2)
            {
                expect (! status.headphoneOutputs[0].present);
                expectEquals (status.headphoneOutputs[0].label, juce::String ("alsa_output.usb-Desk_Speakers_X1-00.analog-stereo"));
                expect (status.headphoneOutputs[1].present);
            }

            expectEquals (status.outputLabel, juce::String ("HDMI Audio"));
            controller.setOutputDevice ("auto");
        }

        beginTest ("the setup wizard keeps a list of outputs it was not asked to change");
        {
            controller.setMixOutputs (Mix::Monitor, juce::StringArray { waveOut, hdmi });

            auto choices = SetupChoices::startingPoint (controller);
            expectEquals (choices.outputDevice, waveOut);                     // it shows the first
            choices.apply (controller);
            expectEquals (state.getOutputs (Mix::Monitor).joinIntoString ("|"), waveOut + "|" + hdmi);

            choices.outputDevice = hdmi;                                      // an explicit choice in the wizard is one device
            choices.apply (controller);
            expectEquals (state.getOutputs (Mix::Monitor).joinIntoString ("|"), hdmi);

            controller.setOutputDevice ("auto");
        }

        beginTest ("the apps list and the wizard's channels are the app channels only");
        {
            const auto choices = SetupChoices::startingPoint (controller);

            for (const auto& channel : choices.channels)
                expect (AppState::describe (state.findChannel (channel.id)).isApp(), channel.id);

            expectEquals ((int) choices.channels.size(), 4);
        }
    }
};

static MultiIoControllerTests multiIoControllerTests;
}
