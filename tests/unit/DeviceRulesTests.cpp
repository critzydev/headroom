// Unit tests for the device rules with made-up devices: which device "auto" picks, which ports carry
// the mic and the headphones, what WaveGuard may do for each kind of input, the words it uses, and the profile rules per
// model. The Elgato ones are simulated from published dumps and reports, not from hardware we have.
#include "Juce.h"

#include "backend/DeviceRules.h"
#include "backend/ElgatoModels.h"
#include "backend/ProfileRules.h"

#include <string>
#include <vector>

namespace hr
{
namespace
{
    using namespace devices;
    using elgato::identify;

    // One USB card as PipeWire shows it: the ids of the device, the names of its two nodes and its ports.
    struct Card
    {
        const char* what;
        int vendor, product;
        std::string source, sink;
        std::string description;
        std::vector<std::string> captureChannels, playbackChannels;
        int sourcePriority = 2000, sinkPriority = 1000;

        elgato::Identity who() const      { return identify (vendor, product, source, description); }

        Candidate asSource() const
        {
            const auto id = who();
            return { source, true, sourcePriority, id.wave };
        }

        Candidate asSink() const
        {
            const auto id = identify (vendor, product, sink, description);
            return { sink, true, sinkPriority, id.wave };
        }
    };

    std::vector<PortFacts> ports (const std::vector<std::string>& channels, uint32_t firstId = 100)
    {
        std::vector<PortFacts> result;

        for (size_t i = 0; i < channels.size(); ++i)
            result.push_back ({ firstId + (uint32_t) i, channels[i] });

        return result;
    }

    std::vector<std::string> aux (int count)
    {
        std::vector<std::string> names;

        for (int i = 0; i < count; ++i)
            names.push_back ("AUX" + std::to_string (i));

        return names;
    }

    // A Blue Yeti-style stereo USB mic.
    Card stereoMic()
    {
        return { "stereo USB mic", 0xb58e, 0x9e84,
                 "alsa_input.usb-Blue_Microphones_Yeti_Stereo_Microphone_REV8-00.analog-stereo",
                 "alsa_output.usb-Blue_Microphones_Yeti_Stereo_Microphone_REV8-00.analog-stereo",
                 "Yeti Stereo Microphone", { "FL", "FR" }, { "FL", "FR" } };
    }

    // A Rode- or Shure-style mono USB mic.
    Card monoMic()
    {
        return { "mono USB mic", 0x19f7, 0x0003,
                 "alsa_input.usb-RODE_Microphones_RODE_NT-USB-00.mono-fallback",
                 "alsa_output.usb-RODE_Microphones_RODE_NT-USB-00.analog-stereo",
                 "RODE NT-USB", { "MONO" }, { "FL", "FR" } };
    }

    // A USB audio interface with many channels, in the multichannel profile.
    Card interface8()
    {
        return { "audio interface", 0x1235, 0x8211,
                 "alsa_input.usb-Focusrite_Scarlett_18i8-00.multichannel-input",
                 "alsa_output.usb-Focusrite_Scarlett_18i8-00.multichannel-output",
                 "Scarlett 18i8", { "FL", "FR", "FC", "LFE", "RL", "RR", "SL", "SR" }, { "FL", "FR", "FC", "LFE", "RL", "RR", "SL", "SR" } };
    }

    Card wave1()
    {
        return { "Wave:1", 0x0fd9, 0x006e,
                 "alsa_input.usb-Elgato_Systems_Elgato_Wave_1_BS49-00.mono-fallback",
                 "alsa_output.usb-Elgato_Systems_Elgato_Wave_1_BS49-00.analog-stereo",
                 "Wave:1", { "MONO" }, { "FL", "FR" }, 3000, 1500 };
    }

    Card wave3()
    {
        return { "Wave:3", 0x0fd9, 0x0070,
                 "alsa_input.usb-Elgato_Systems_Elgato_Wave_3_BS49-00.mono-fallback",
                 "alsa_output.usb-Elgato_Systems_Elgato_Wave_3_BS49-00.analog-stereo",
                 "Wave:3", { "MONO" }, { "FL", "FR" }, 3000, 1500 };
    }

    Card waveXlr()
    {
        return { "Wave XLR", 0x0fd9, 0x007d,
                 "alsa_input.usb-Elgato_Systems_Elgato_Wave_XLR_AB12-00.mono-fallback",
                 "alsa_output.usb-Elgato_Systems_Elgato_Wave_XLR_AB12-00.analog-stereo",
                 "Wave XLR", { "MONO" }, { "FL", "FR" }, 3000, 1500 };
    }

    Card waveXlrMk2()
    {
        return { "Wave XLR MK.2", 0x0fd9, 0x00b6,
                 "alsa_input.usb-Elgato_Elgato_Wave_XLR_MK.2_CD34-00.analog-stereo",
                 "alsa_output.usb-Elgato_Elgato_Wave_XLR_MK.2_CD34-00.analog-stereo",
                 "Wave XLR MK.2", { "FL", "FR" }, { "FL", "FR" }, 3000, 1500 };
    }

    Card waveXlrPro()
    {
        return { "Wave XLR Pro", 0x0fd9, 0x00b4,
                 "alsa_input.usb-Elgato_Elgato_Wave_XLR_Pro_EF56-00.multichannel-input",
                 "alsa_output.usb-Elgato_Elgato_Wave_XLR_Pro_EF56-00.multichannel-output",
                 "Wave XLR Pro", aux (18), aux (17), 3000, 1500 };
    }

    // A capture card with an audio input: an Elgato device, but not a microphone.
    Card captureCard()
    {
        return { "capture card", 0x0fd9, 0x7f01,
                 "alsa_input.usb-Elgato_Systems_Elgato_Game_Capture_HD60_X_G1-00.analog-stereo",
                 "alsa_output.usb-Elgato_Systems_Elgato_Game_Capture_HD60_X_G1-00.analog-stereo",
                 "Elgato Game Capture HD60 X", { "FL", "FR" }, { "FL", "FR" }, 2500, 1000 };
    }

    pw::ProfileRec profile (int index, const char* name, int sinks, int sources, int priority)
    {
        pw::ProfileRec p;
        p.index = index;
        p.name = name;
        p.description = name;
        p.sinks = sinks;
        p.sources = sources;
        p.priority = priority;
        return p;
    }

    pw::DeviceState state (std::vector<pw::ProfileRec> profiles, const char* active)
    {
        pw::DeviceState s;
        s.profiles = std::move (profiles);
        s.profilesSeen = s.activeSeen = true;

        for (const auto& p : s.profiles)
            if (p.name == active)
            {
                s.activeProfile = p.index;
                s.activeProfileName = p.name;
            }

        return s;
    }
}

class DeviceRulesTests final : public juce::UnitTest
{
public:
    DeviceRulesTests() : juce::UnitTest ("Device rules", "headroom") {}

    void runTest() override
    {
        selectionTests();
        listTests();
        portTests();
        guardTests();
        wordingTests();
        profileTests();
    }

private:
    // ---- Which device ------------------------------------------------------------------------------------------------
    void selectionTests()
    {
        beginTest ("auto prefers an Elgato Wave over a better-ranked ordinary mic");
        {
            const std::vector<Candidate> inputs { monoMic().asSource(), wave1().asSource(), stereoMic().asSource() };
            expectEquals (pickCandidate (inputs, "auto"), 1);
            expectEquals (pickCandidate (inputs, ""), 1);
        }

        beginTest ("auto picks the Wave of any model, by priority among them");
        {
            for (const auto& card : { wave1(), wave3(), waveXlr(), waveXlrMk2(), waveXlrPro() })
            {
                const std::vector<Candidate> inputs { stereoMic().asSource(), card.asSource() };
                expectEquals (pickCandidate (inputs, "auto"), 1, card.what);
            }

            auto low = wave1(), high = waveXlr();
            low.sourcePriority = 2000;
            high.sourcePriority = 3000;
            expectEquals (pickCandidate ({ low.asSource(), high.asSource() }, "auto"), 1);
            expectEquals (pickCandidate ({ high.asSource(), low.asSource() }, "auto"), 0);
        }

        beginTest ("without a Wave: the real device with the highest priority, else any node");
        {
            auto quiet = monoMic();
            quiet.sourcePriority = 1500;
            auto loud = stereoMic();
            loud.sourcePriority = 2500;
            expectEquals (pickCandidate ({ quiet.asSource(), loud.asSource() }, "auto"), 1);

            const Candidate virtualNode { "some.virtual.source", false, 9000, false };
            expectEquals (pickCandidate ({ virtualNode, quiet.asSource() }, "auto"), 1);        // a real device beats a virtual node
            expectEquals (pickCandidate ({ virtualNode }, "auto"), 0);                           // but a virtual node is better than nothing
            expectEquals (pickCandidate ({}, "auto"), -1);
        }

        beginTest ("an Elgato capture card is not preferred: it is not a microphone");
        {
            // The card's source has a better priority than the Yeti, and neither is a Wave.
            const auto card = captureCard();
            expect (! card.who().wave && card.who().elgato);
            expectEquals (pickCandidate ({ stereoMic().asSource(), card.asSource() }, "auto"), 1);     // by priority, like any other

            auto lowCard = captureCard();
            lowCard.sourcePriority = 100;
            expectEquals (pickCandidate ({ lowCard.asSource(), stereoMic().asSource() }, "auto"), 1);
        }

        beginTest ("an id that is present is honoured, one that is gone falls back to auto");
        {
            const std::vector<Candidate> inputs { stereoMic().asSource(), wave1().asSource(), monoMic().asSource() };
            expectEquals (pickCandidate (inputs, stereoMic().source), 0);
            expectEquals (pickCandidate (inputs, monoMic().source), 2);
            expectEquals (pickCandidate (inputs, wave1().source), 1);
            expectEquals (pickCandidate (inputs, "alsa_input.usb-Unplugged-00.analog-stereo"), 1);
        }

        beginTest ("a Wave's normal node beats its Pro Audio node, even when the Pro Audio one has the higher priority");
        {
            const Candidate normal { "alsa_input.usb-Elgato_Systems_Elgato_Wave_3_BS49-00.mono-fallback", true, 3000, true };
            const Candidate pro { "alsa_input.usb-Elgato_Systems_Elgato_Wave_3_BS49-00.pro-input-0", true, 4000, true };

            expectEquals (pickCandidate ({ normal, pro }, "auto"), 0);
            expectEquals (pickCandidate ({ pro, normal }, "auto"), 1);
            expectEquals (pickCandidate ({ pro }, "auto"), 0);                       // alone, it is still the Wave
            expectEquals (pickCandidate ({ normal, pro }, pro.name), 1);             // and a choice by name is honoured
        }

        beginTest ("headphones: the same rules for the outputs");
        {
            const Candidate hdmi { "alsa_output.pci-0000_01_00.1.hdmi-stereo", true, 700, false };
            expectEquals (pickCandidate ({ hdmi, wave1().asSink(), monoMic().asSink() }, "auto"), 1);
            expectEquals (pickCandidate ({ hdmi, monoMic().asSink() }, "auto"), 1);                 // 1000 beats 700
            expectEquals (pickCandidate ({ hdmi, waveXlr().asSink() }, waveXlr().sink), 1);
        }
    }

    void listTests()
    {
        beginTest ("the list: Waves first, then by label, then by id");
        {
            std::vector<hr::DeviceInfo> list;
            const auto add = [&] (const char* id, const char* label, bool isWave) { list.push_back ({ id, label, isWave }); };
            add ("b", "Yeti", false);
            add ("a2", "Built-in Audio", false);
            add ("w2", "Elgato Wave XLR", true);
            add ("a1", "Built-in Audio", false);
            add ("w1", "Elgato Wave:1", true);

            sortDeviceList (list);

            expectEquals (list[0].id, juce::String ("w2"));         // "Elgato Wave XLR" sorts before "Elgato Wave:1": a space comes before a colon
            expectEquals (list[1].id, juce::String ("w1"));
            expectEquals (list[2].id, juce::String ("a1"));
            expectEquals (list[3].id, juce::String ("a2"));
            expectEquals (list[4].id, juce::String ("b"));
        }
    }

    // ---- Which ports -------------------------------------------------------------------------------------------------
    void portTests()
    {
        beginTest ("a stereo USB mic: both channels, which the engine takes as (L+R)/2");
        {
            const auto result = pickMicPorts (ports (stereoMic().captureChannels), stereoMic().who().elgato);
            expectEquals ((int) result.left, 100);
            expectEquals ((int) result.right, 101);
        }

        beginTest ("a mono USB mic: the one port, nothing on the right");
        {
            const auto result = pickMicPorts (ports (monoMic().captureChannels), monoMic().who().elgato);
            expectEquals ((int) result.left, 100);
            expect (result.right == kNoPort);
        }

        beginTest ("an audio interface with many channels: the first channel only");
        {
            const auto result = pickMicPorts (ports (interface8().captureChannels), interface8().who().elgato);
            expectEquals ((int) result.left, 100);
            expect (result.right == kNoPort);

            // The same in the Pro Audio profile (AUX channels), 18 of them.
            const auto raw = pickMicPorts (ports (aux (18)), false);
            expectEquals ((int) raw.left, 100);
            expect (raw.right == kNoPort);

            // The mic is channel 1 by name, not by position, when the names are there.
            const auto shuffled = pickMicPorts ({ { 7, "FR" }, { 8, "FL" }, { 9, "FC" } }, false);
            expectEquals ((int) shuffled.left, 8);
            expect (shuffled.right == kNoPort);
        }

        beginTest ("the Wave:3 maps exactly as before: its one MONO port, and the headphones FL / FR");
        {
            const auto card = wave3();
            const auto mic = pickMicPorts (ports (card.captureChannels), card.who().elgato);
            expectEquals ((int) mic.left, 100);
            expect (mic.right == kNoPort);

            const auto phones = pickPlaybackPorts (ports (card.playbackChannels, 200), 0);
            expectEquals ((int) phones.left, 200);
            expectEquals ((int) phones.right, 201);
        }

        beginTest ("every Elgato model listens to channel 1 only: the second channel of an XLR interface is not the mic");
        {
            for (const auto& card : { wave1(), wave3(), waveXlr(), waveXlrMk2(), waveXlrPro() })
            {
                const auto result = pickMicPorts (ports (card.captureChannels), card.who().elgato);
                expectEquals ((int) result.left, 100, card.what);
                expect (result.right == kNoPort, card.what);
            }
        }

        beginTest ("stereo ports in a different order and a pair that has no FL still map");
        {
            const auto swapped = pickStereoPorts ({ { 5, "FR" }, { 4, "FL" } });
            expectEquals ((int) swapped.left, 4);
            expectEquals ((int) swapped.right, 5);

            const auto unnamed = pickStereoPorts ({ { 9, "" }, { 10, "" }, { 11, "" } });
            expectEquals ((int) unnamed.left, 9);
            expectEquals ((int) unnamed.right, 10);

            const auto onlyMono = pickStereoPorts ({ { 3, "MONO" } });
            expectEquals ((int) onlyMono.left, 3);
            expect (onlyMono.right == kNoPort);

            const auto none = pickStereoPorts ({});
            expect (none.left == kNoPort && none.right == kNoPort);
        }

        beginTest ("the Pro's headphones listen to the second pair of its playback channels, if it has them");
        {
            const auto pro = elgato::findByPid (0x00b4);
            expect (pro != nullptr);
            const int pair = pro != nullptr ? pro->monitorPair : 0;
            expectEquals (pair, 1);

            const auto phones = pickPlaybackPorts (ports (aux (17), 300), pair);
            expectEquals ((int) phones.left, 302);
            expectEquals ((int) phones.right, 303);

            // A card that shows only two playback ports (a stereo profile) gets them, whatever the model says.
            const auto stereoOnly = pickPlaybackPorts (ports ({ "FL", "FR" }, 300), pair);
            expectEquals ((int) stereoOnly.left, 300);
            expectEquals ((int) stereoOnly.right, 301);

            // Three ports are not a second pair.
            const auto three = pickPlaybackPorts (ports ({ "FL", "FR", "FC" }, 300), pair);
            expectEquals ((int) three.left, 300);
            expectEquals ((int) three.right, 301);
        }
    }

    // ---- What WaveGuard may do ---------------------------------------------------------------------------------------
    void guardTests()
    {
        beginTest ("every Elgato Wave model gets capture-first, dead-mic recovery and the unmute");
        {
            for (const auto& model : elgato::models())
            {
                const auto plan = guardPlanFor (identify (0x0fd9, (int) model.pid, "", ""));
                expect (plan.captureFirst && plan.recoverSilentMic && plan.unmute, model.name);
                expect (mayFixProfile (identify (0x0fd9, (int) model.pid, "", "")), model.name);
            }

            // A Wave that is not in the table yet is treated the same.
            const auto unknown = identify (0x0fd9, 0x1234, "alsa_input.usb-Elgato_Systems_Elgato_Wave_Z_1-00.analog-stereo", "Elgato Wave Z");
            expect (guardPlanFor (unknown).captureFirst && guardPlanFor (unknown).recoverSilentMic && mayFixProfile (unknown));
        }

        beginTest ("no other mic ever has its profile cycled, its route unmuted or its links held back");
        {
            for (const auto& card : { stereoMic(), monoMic(), interface8() })
            {
                const auto plan = guardPlanFor (card.who());
                expect (! plan.captureFirst && ! plan.recoverSilentMic && ! plan.unmute, card.what);
                expect (! mayFixProfile (card.who()), card.what);
            }

            const auto none = guardPlanFor (identify (-1, -1, "alsa_input.pci-0000_00_1f.3.analog-stereo", "Built-in Audio"));
            expect (! none.captureFirst && ! none.recoverSilentMic && ! none.unmute);
        }

        beginTest ("an Elgato capture card or camera is not touched either");
        {
            const auto card = captureCard().who();
            expect (card.elgato && ! card.wave);
            const auto plan = guardPlanFor (card);
            expect (! plan.captureFirst && ! plan.recoverSilentMic && ! plan.unmute);
            expect (! mayFixProfile (card));
        }
    }

    // ---- Words -------------------------------------------------------------------------------------------------------
    void wordingTests()
    {
        const auto words = [] (const Card& card, hr::MicState state)
        {
            return micMessage (state, wordingFor (card.who(), elgato::label (card.who(), true, card.description, "", "", "")));
        };

        beginTest ("the Wave:3's sentences are the ones it has always had");
        {
            expectEquals (words (wave3(), hr::MicState::SilentFromStart),
                          juce::String ("No sound from your mic. If the ring on your Wave:3 is red, tap the top of the mic to unmute."));
            expectEquals (words (wave3(), hr::MicState::MutedOnDevice),
                          juce::String (juce::CharPointer_UTF8 ("Mic muted on the Wave:3 \xe2\x80\x94 tap the top to unmute")));
            expectEquals (words (wave3(), hr::MicState::Recovering), juce::String ("Fixing your mic. This takes a few seconds."));
            expect (words (wave3(), hr::MicState::Ok).isEmpty() && words (wave3(), hr::MicState::Starting).isEmpty());
        }

        beginTest ("another Elgato model is named, and is not told to tap a ring it may not have");
        {
            expectEquals (words (wave1(), hr::MicState::SilentFromStart),
                          juce::String ("No sound from your mic. If your Wave:1 is muted, unmute it on the device."));
            expectEquals (words (waveXlr(), hr::MicState::MutedOnDevice),
                          juce::String (juce::CharPointer_UTF8 ("Mic muted on the Wave XLR \xe2\x80\x94 unmute it on the device")));
            expect (! words (waveXlrMk2(), hr::MicState::SilentFromStart).contains ("Wave:3"));
            expect (words (waveXlrMk2(), hr::MicState::SilentFromStart).contains ("Wave XLR MK.2"));
        }

        beginTest ("any other mic gets the plain sentences, with no model in them");
        {
            for (const auto& card : { stereoMic(), monoMic(), interface8() })
            {
                expectEquals (words (card, hr::MicState::SilentFromStart), juce::String ("No sound from your mic. Check that it is plugged in and not muted."));
                expectEquals (words (card, hr::MicState::MutedOnDevice), juce::String ("Your mic is muted. Unmute it on the device."));
            }
        }

        beginTest ("no microphone found does not tell anyone to plug in a Wave:3");
        {
            const auto text = micMessage (hr::MicState::NotFound, MicWording());
            expect (text.isNotEmpty() && ! text.contains ("Wave"));
        }
    }

    // ---- Profiles, per model -----------------------------------------------------------------------------------------
    void profileTests()
    {
        // What the UAC1 cards (Wave:1, Wave:3, Wave XLR) offer: the Wave:3's profile set, from its real dump.
        const auto uac1 = [] (const char* active)
        {
            return state ({ profile (0, "off", 0, 0, 0), profile (1, "output:analog-stereo", 1, 0, 6500),
                            profile (2, "output:analog-stereo+input:mono-fallback", 1, 1, 6001),
                            profile (3, "output:iec958-stereo", 1, 0, 5500),
                            profile (4, "output:iec958-stereo+input:mono-fallback", 1, 1, 5001),
                            profile (5, "input:mono-fallback", 0, 1, 1), profile (6, "pro-audio", 2, 1, 1) }, active);
        };

        beginTest ("Wave:1 and Wave XLR (UAC1 cards, simulated): iec958 and Pro Audio are not usable, analog + mono-fallback is chosen");
        {
            for (const int pid : { 0x006e, 0x0070, 0x007d })
            {
                const auto* model = elgato::findByPid (pid);
                expect (model != nullptr);

                for (const char* bad : { "pro-audio", "output:iec958-stereo+input:mono-fallback", "output:iec958-stereo", "off", "input:mono-fallback" })
                {
                    const auto s = uac1 (bad);
                    expect (! pw::profiles::activeProfileIsGood (s, true), std::string (model->name) + " " + bad);
                    expectEquals (pw::profiles::bestProfile (s, true, model->goodProfile), 2, std::string (model->name) + " " + bad);
                }

                expect (pw::profiles::activeProfileIsGood (uac1 ("output:analog-stereo+input:mono-fallback"), true));
            }
        }

        beginTest ("Wave XLR MK.2 (UAC2 card with a stereo mic, simulated): its own best profile, not a mono pin");
        {
            const auto mk2 = [] (const char* active)
            {
                return state ({ profile (0, "off", 0, 0, 0), profile (1, "output:analog-stereo", 1, 0, 6500),
                                profile (2, "output:analog-stereo+input:analog-stereo", 1, 1, 6600),
                                profile (3, "output:analog-stereo+input:mono-fallback", 1, 1, 6001),
                                profile (4, "input:analog-stereo", 0, 1, 100), profile (5, "pro-audio", 2, 1, 1) }, active);
            };

            const auto* model = elgato::findByPid (0x00b6);
            expect (model != nullptr && std::string (model->goodProfile).empty());

            const auto s = mk2 ("pro-audio");
            expect (! pw::profiles::activeProfileIsGood (s, true));
            expectEquals (pw::profiles::bestProfile (s, true, model->goodProfile), 2);          // the stereo mic, by priority
            expectEquals (pw::profiles::bestProfile (s, true), 3);                              // with the old default pin it would have been mono-fallback

            expect (pw::profiles::activeProfileIsGood (mk2 ("output:analog-stereo+input:analog-stereo"), true));
        }

        beginTest ("Wave XLR Pro (multichannel card, simulated): the multichannel profile is the one to be in");
        {
            const auto pro = [] (const char* active)
            {
                return state ({ profile (0, "off", 0, 0, 0), profile (1, "output:multichannel-output+input:multichannel-input", 1, 1, 10),
                                profile (2, "pro-audio", 2, 1, 1) }, active);
            };

            const auto* model = elgato::findByPid (0x00b4);
            expect (model != nullptr);

            const auto s = pro ("pro-audio");
            expect (! pw::profiles::activeProfileIsGood (s, true));
            expectEquals (pw::profiles::bestProfile (s, true, model->goodProfile), 1);
            expect (pw::profiles::activeProfileIsGood (pro ("output:multichannel-output+input:multichannel-input"), true));
        }

        beginTest ("another vendor's card in Pro Audio is left alone, and the old two-argument call still prefers analog + mono-fallback");
        {
            const auto s = uac1 ("pro-audio");
            expect (pw::profiles::activeProfileIsGood (s, false));                          // accepted: someone chose it
            expectEquals (pw::profiles::bestProfile (s, false), 2);
            expectEquals (pw::profiles::bestProfile (s, false, pw::profiles::kDefaultPreferred), 2);
            expectEquals (pw::profiles::bestProfile (s, false, ""), 2);                     // no preference: the highest priority usable one (Pro Audio still last)
            expectEquals (pw::profiles::bestProfile (s, false, nullptr), 2);
        }
    }
};

static DeviceRulesTests deviceRulesTests;
}
