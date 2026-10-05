// Unit tests for the Elgato model table: ids, names, node-name fragments, how a device is recognised, the labels,
// and which models get which kind of USB control.
#include "Juce.h"

#include "backend/ElgatoModels.h"

#include <set>
#include <string>

namespace hr
{
namespace
{
    using namespace elgato;

    // udev's name for a card or node: usb-<manufacturer>_<product>_<serial>-00
    std::string node (const char* manufacturerAndProduct, const char* what = "alsa_input", const char* suffix = ".mono-fallback")
    {
        return std::string (what) + ".usb-" + manufacturerAndProduct + "_SERIAL123-00" + suffix;
    }
}

class ElgatoModelsTests final : public juce::UnitTest
{
public:
    ElgatoModelsTests() : juce::UnitTest ("Elgato models", "headroom") {}

    void runTest() override
    {
        tableTests();
        recognitionTests();
        nameTests();
        hardwareTests();
    }

private:
    void tableTests()
    {
        beginTest ("the table: unique product ids, names and node fragments, and nothing empty");
        {
            std::set<int> pids;
            std::set<std::string> names, fragments;

            for (const auto& model : models())
            {
                expect (pids.insert ((int) model.pid).second, "duplicate pid");
                expect (names.insert (model.name).second, "duplicate name");
                expect (fragments.insert (model.nodeFragment).second, "duplicate fragment");
                expect (std::string (model.name).rfind ("Elgato ", 0) == 0, std::string (model.name) + " starts with the brand");
                expect (std::string (model.nodeFragment).back() == '_', "a fragment ends with the underscore before the serial");
                expect (std::string (model.shortName).size() > 0);
                expect (model.captureChannels >= 0 && model.playbackChannels >= 0);
            }

            expectEquals ((int) models().size(), 9);
        }

        beginTest ("the product ids are the published ones");
        {
            const struct { int pid; const char* name; } expected[] =
            {
                { 0x006e, "Elgato Wave:1" },        { 0x0070, "Elgato Wave:3" },         { 0x00ca, "Elgato Wave:3 MK.2" },
                { 0x0098, "Elgato Wave Neo" },      { 0x007d, "Elgato Wave XLR" },       { 0x00a6, "Elgato XLR Dock" },
                { 0x00b6, "Elgato Wave XLR MK.2" }, { 0x00c7, "Elgato XLR Dock MK.2" }, { 0x00b4, "Elgato Wave XLR Pro" },
            };

            for (const auto& e : expected)
            {
                const auto* model = findByPid (e.pid);
                expect (model != nullptr, "model for " + juce::String::toHexString (e.pid));

                if (model != nullptr)
                    expectEquals (juce::String (model->name), juce::String (e.name));
            }

            expect (findByPid (0x0071) == nullptr, "0071 is the Wave:3 in firmware-update mode, not a model");
            expect (findByPid (0x0084) == nullptr, "a Stream Deck is not a Wave");
            expect (findByPid (-1) == nullptr);
        }

        beginTest ("the Wave:3 keeps what it had: tested hardware, a mono mic, the analog + mono-fallback profile");
        {
            const auto* wave3 = findByPid (0x0070);
            expect (wave3 != nullptr);

            if (wave3 != nullptr)
            {
                expect (wave3->hardware == Hardware::Tested);
                expect (wave3->clock == Clock::Reported);
                expect (wave3->kind == Kind::Microphone);
                expectEquals (wave3->captureChannels, 1);
                expectEquals (wave3->playbackChannels, 2);
                expectEquals (juce::String (wave3->goodProfile), juce::String ("output:analog-stereo+input:mono-fallback"));
                expectEquals (wave3->usbConfigBytes, 16);
                expectEquals (wave3->usbGainMaxDb, 40.0);
                expectEquals (wave3->monitorPair, 0);
            }
        }

        beginTest ("only the multichannel Pro listens on a later pair of playback channels");
        {
            for (const auto& model : models())
                expectEquals (model.monitorPair, model.pid == 0x00b4 ? 1 : 0);
        }

        beginTest ("a model whose card is not the Wave:3's layout has no preferred profile");
        {
            for (const auto& model : models())
            {
                const bool uac1Mono = model.pid == 0x006e || model.pid == 0x0070 || model.pid == 0x0098 || model.pid == 0x007d;
                expectEquals (juce::String (model.goodProfile), juce::String (uac1Mono ? "output:analog-stereo+input:mono-fallback" : ""));
            }
        }
    }

    void recognitionTests()
    {
        beginTest ("a device is recognised by its USB ids, whatever its node is called");
        {
            for (const auto& model : models())
            {
                const auto who = identify (0x0fd9, (int) model.pid, "alsa_card.usb-Something_Else_X-00", "");
                expect (who.elgato && who.wave);
                expect (who.model == &model, std::string ("the ids of ") + model.name);
            }
        }

        beginTest ("the Wave:3 is the only one that isWave3");
        {
            for (const auto& model : models())
                expect (identify (0x0fd9, (int) model.pid, "", "").isWave3() == (model.pid == 0x0070));

            expect (! identify (-1, -1, "", "").isWave3());
        }

        beginTest ("a node with no device behind it is recognised by the udev name of every model");
        {
            const struct { const char* name; int pid; } cases[] =
            {
                { "Elgato_Systems_Elgato_Wave_1_BS4",          0x006e },
                { "Elgato_Systems_Elgato_Wave_3_BS49J1A00000", 0x0070 },
                { "Elgato_Systems_Elgato_Wave_Neo_AB1",        0x0098 },
                { "Elgato_Systems_Elgato_Wave_XLR_AB2",        0x007d },
                { "Elgato_Systems_Elgato_XLR_Dock_AB3",        0x00a6 },
                { "Elgato_Elgato_Wave_XLR_MK.2_AB4",           0x00b6 },
                { "Elgato_Elgato_Wave_XLR_Dock_MK.2_AB5",      0x00c7 },
                { "Elgato_Elgato_Wave_XLR_Pro_AB6",            0x00b4 },
                { "Elgato_Wave_3_MK.2_AB7",                    0x00ca },
            };

            for (const auto& c : cases)
                for (const auto& n : { node (c.name), node (c.name, "alsa_output", ".analog-stereo"), std::string ("alsa_card.usb-") + c.name + "-00" })
                {
                    const auto who = identify (-1, -1, n, "");
                    expect (who.elgato && who.wave, n);
                    expect (who.model != nullptr && who.model->pid == c.pid, n);
                }
        }

        beginTest ("the longest fragment wins: an MK.2 is not taken for the model it is named after");
        {
            expect (findByNodeName (node ("Elgato_Elgato_Wave_XLR_Dock_MK.2_AB5"))->pid == 0x00c7);       // not the XLR Dock, not the Wave XLR
            expect (findByNodeName (node ("Elgato_Elgato_Wave_XLR_MK.2_AB4"))->pid == 0x00b6);            // not the Wave XLR
            expect (findByNodeName (node ("Elgato_Elgato_Wave_XLR_Pro_AB6"))->pid == 0x00b4);
            expect (findByNodeName (node ("Elgato_Wave_3_MK.2_AB7"))->pid == 0x00ca);                     // not the Wave:3
            expect (findByNodeName (node ("Elgato_Systems_Elgato_Wave_3_MK2X"))->pid == 0x0070);          // a serial is not a fragment
        }

        beginTest ("an Elgato product id we do not know: a Wave (WaveGuard on), unless its name says it is a capture card or a camera");
        {
            const auto unknown = identify (0x0fd9, 0x1234, "alsa_input.usb-Elgato_Systems_Elgato_Mic_X_Z-00.analog-stereo", "Elgato Mic X");
            expect (unknown.elgato && unknown.wave && unknown.model == nullptr);

            const auto bare = identify (0x0fd9, 0x1234, "", "");
            expect (bare.elgato && bare.wave && bare.model == nullptr);

            // A newer product whose node name starts like the Wave:3's is not the Wave:3.
            const auto lookAlike = identify (0x0fd9, 0x2222, "alsa_input.usb-Elgato_Systems_Elgato_Wave_3_ZZ-00.mono-fallback", "");
            expect (lookAlike.elgato && lookAlike.wave && lookAlike.model == nullptr && ! lookAlike.isWave3());

            for (const char* thing : { "Elgato Game Capture HD60 X", "Elgato HD60 X", "Elgato 4K X", "Elgato Cam Link 4K", "Elgato Facecam MK.2" })
            {
                const auto who = identify (0x0fd9, 0x9999, std::string ("alsa_input.usb-Elgato_Systems_") + thing + "-00.analog-stereo", thing);
                expect (who.elgato && ! who.wave && who.model == nullptr, thing);
            }

            // A name with "Wave" or "XLR" in it is a mic even if it also says "4K".
            expect (identify (0x0fd9, 0x9998, "", "Elgato Wave 4K").wave);
        }

        beginTest ("other vendors are not Elgato: nothing special, whatever the node is called");
        {
            for (const auto& who : { identify (0x1235, 0x8211, "alsa_input.usb-Focusrite_Scarlett_2i2-00.analog-stereo", "Scarlett 2i2"),
                                     identify (0xb58e, 0x9e84, "alsa_input.usb-Blue_Microphones_Yeti-00.analog-stereo", "Yeti"),
                                     identify (0x1235, 0x8211, "alsa_input.usb-Elgato_Systems_Elgato_Wave_3_X-00.mono-fallback", "") })
            {
                expect (! who.elgato && ! who.wave && who.model == nullptr);
            }

            const auto plain = identify (-1, -1, "alsa_input.pci-0000_00_1f.3.analog-stereo", "Built-in Audio");
            expect (! plain.elgato && ! plain.wave && plain.model == nullptr);

            // A name that says Elgato but is not a mic (no ids): left alone.
            const auto card = identify (-1, -1, "alsa_input.usb-Elgato_Systems_Elgato_Game_Capture_HD60_X_ZZ-00.analog-stereo", "");
            expect (! card.elgato && ! card.wave);
        }
    }

    void nameTests()
    {
        beginTest ("labels: the model's own name, and the same with Headphones for the output");
        {
            for (const auto& model : models())
            {
                const auto who = identify (0x0fd9, (int) model.pid, "", "");
                expectEquals (juce::String (label (who, true, "whatever the card says", "nick", "node description", "node nick")), juce::String (model.name));
                expectEquals (juce::String (label (who, false, "", "", "", "")), juce::String (model.name) + " Headphones");
            }

            const auto xlr = identify (0x0fd9, 0x007d, "", "");
            expectEquals (juce::String (label (xlr, false, "Wave XLR", "", "", "")), juce::String ("Elgato Wave XLR Headphones"));

            const auto wave1 = identify (0x0fd9, 0x006e, "", "");
            expectEquals (juce::String (label (wave1, true, "", "", "", "")), juce::String ("Elgato Wave:1"));
        }

        beginTest ("labels: the Wave:3 reads exactly as it always did, however the card describes itself");
        {
            const auto wave3 = identify (0x0fd9, 0x0070, "", "");

            for (const char* description : { "Wave:3", "Elgato Wave:3", "" })
                expectEquals (juce::String (label (wave3, true, description, "", "", "")), juce::String ("Elgato Wave:3"));

            expectEquals (juce::String (label (wave3, false, "Wave:3", "", "", "")), juce::String ("Elgato Wave:3 Headphones"));
        }

        beginTest ("labels: an unknown Elgato device is called what it calls itself, with the brand in front");
        {
            const auto unknown = identify (0x0fd9, 0x1234, "", "");
            expectEquals (juce::String (label (unknown, true, "Wave Z", "", "", "")), juce::String ("Elgato Wave Z"));
            expectEquals (juce::String (label (unknown, true, "Elgato Wave Z", "", "", "")), juce::String ("Elgato Wave Z"));
            expectEquals (juce::String (label (unknown, true, "", "Z nick", "", "")), juce::String ("Elgato Z nick"));
            expectEquals (juce::String (label (unknown, true, "", "", "node text", "")), juce::String ("Elgato node text"));
            expectEquals (juce::String (label (unknown, false, "Wave Z", "", "", "")), juce::String ("Elgato Wave Z Headphones"));
            expectEquals (juce::String (label (unknown, true, "", "", "", "")), juce::String ("Elgato Wave"));
        }

        beginTest ("short names, for sentences and the pill");
        {
            expectEquals (juce::String (shortName (identify (0x0fd9, 0x0070, "", ""), "Elgato Wave:3")), juce::String ("Wave:3"));
            expectEquals (juce::String (shortName (identify (0x0fd9, 0x00b6, "", ""), "x")), juce::String ("Wave XLR MK.2"));
            expectEquals (juce::String (shortName (identify (0x0fd9, 0x1234, "", ""), "Elgato Wave Z")), juce::String ("Wave Z"));
            expectEquals (juce::String (shortName (identify (0x0fd9, 0x1234, "", ""), "Wave Z")), juce::String ("Wave Z"));
        }
    }

    void hardwareTests()
    {
        beginTest ("USB controls: tested for the Wave:3 alone, experimental for the Wave XLR alone");
        {
            const auto tested = pidsWith (Hardware::Tested);
            const auto experimental = pidsWith (Hardware::Experimental);
            expectEquals ((int) tested.size(), 1);
            expectEquals ((int) experimental.size(), 1);

            if (! tested.empty())
                expectEquals ((int) tested[0], 0x0070);

            if (! experimental.empty())
                expectEquals ((int) experimental[0], 0x007d);

            expectEquals ((int) pidsWith (Hardware::None).size(), 7);
        }

        beginTest ("a model with USB controls says how big its config block is and how far its gain goes; one without says nothing");
        {
            for (const auto& model : models())
            {
                const bool has = model.hardware != Hardware::None;
                expect ((model.usbConfigBytes > 0) == has, model.name);
                expect ((model.usbGainMaxDb > 0.0) == has, model.name);
            }

            expectEquals (findByPid (0x007d)->usbConfigBytes, 34);
            expectEquals (findByPid (0x007d)->usbGainMaxDb, 75.0);
        }

        beginTest ("every Elgato model is treated as sharing the clock; the table says for which ones users reported it");
        {
            for (const auto& model : models())
            {
                const bool reported = model.pid == 0x006e || model.pid == 0x0070 || model.pid == 0x007d || model.pid == 0x00a6;
                expect ((model.clock == Clock::Reported) == reported, model.name);
            }

            expectEquals (juce::String (describe (Clock::Reported)), juce::String ("reported"));
            expectEquals (juce::String (describe (Hardware::Experimental)), juce::String ("experimental"));
        }
    }
};

static ElgatoModelsTests elgatoModelsTests;
}
