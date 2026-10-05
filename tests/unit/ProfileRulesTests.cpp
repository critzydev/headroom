// Unit tests for the profile rules: which profile of an ALSA device is usable, which one WaveGuard switches to.
// An Elgato device never stays in Pro Audio (no ACP routes there, so no hardware gain); any other device may.
#include "Juce.h"

#include "backend/ProfileRules.h"

#include <string>

namespace hr
{
namespace
{
    using pw::DeviceState;
    using pw::ProfileRec;
    namespace profiles = pw::profiles;

    ProfileRec profile (int index, const char* name, int sinks, int sources, int priority = 0, bool available = true)
    {
        ProfileRec p;
        p.index = index;
        p.name = name;
        p.description = name;
        p.sinks = sinks;
        p.sources = sources;
        p.priority = priority;
        p.available = available;
        return p;
    }

    // What the Wave:3's card offers (profile names as ALSA's card profile set names them), with the given one active.
    DeviceState wave3 (const char* active)
    {
        DeviceState state;
        state.profiles = { profile (0, "off", 0, 0),
                           profile (1, "output:analog-stereo", 1, 0, 6500),
                           profile (2, "output:analog-stereo+input:mono-fallback", 1, 1, 6001),
                           profile (3, "output:iec958-stereo", 1, 0, 5500),
                           profile (4, "output:iec958-stereo+input:mono-fallback", 1, 1, 5001),
                           profile (5, "input:mono-fallback", 0, 1, 1),
                           profile (6, "pro-audio", 2, 1, 1) };
        state.profilesSeen = true;
        state.activeSeen = true;

        for (const auto& p : state.profiles)
            if (p.name == active)
            {
                state.activeProfile = p.index;
                state.activeProfileName = p.name;
            }

        return state;
    }
}

class ProfileRulesTests final : public juce::UnitTest
{
public:
    ProfileRulesTests() : juce::UnitTest ("Profile rules", "headroom") {}

    void runTest() override
    {
        beginTest ("an Elgato device is never left in Pro Audio, the profile with a source and a sink is chosen instead");
        {
            const auto state = wave3 ("pro-audio");

            expect (! profiles::activeProfileIsGood (state, true));
            expect (std::string (profiles::whyActiveProfileIsNotGood (state, true)).find ("no hardware gain control in Pro Audio") != std::string::npos);
            expectEquals (profiles::bestProfile (state, true), 2);          // output:analog-stereo+input:mono-fallback
        }

        beginTest ("Pro Audio is not one of the usable profiles of an Elgato device, whatever its priority");
        {
            auto state = wave3 ("off");
            state.profiles[6].priority = 100000;                            // would win on priority alone
            state.profiles[2].priority = 1;

            expect (! profiles::isGoodProfile (state.profiles[6], true));
            expectEquals (profiles::bestProfile (state, true), 2);
        }

        beginTest ("an Elgato device that offers only Pro Audio (and off) is left as it is: nothing better to switch to");
        {
            DeviceState state;
            state.profiles = { profile (0, "off", 0, 0), profile (1, "pro-audio", 2, 1, 1) };
            state.activeProfile = 1;
            state.activeProfileName = "pro-audio";

            expect (! profiles::activeProfileIsGood (state, true));
            expectEquals (profiles::bestProfile (state, true), -1);
        }

        beginTest ("the analog profile is still good for an Elgato device, and nothing is switched");
        {
            const auto state = wave3 ("output:analog-stereo+input:mono-fallback");

            expect (profiles::activeProfileIsGood (state, true));
            expectEquals (profiles::bestProfile (state, true), 2);
        }

        beginTest ("S/PDIF, off and half profiles are not usable for anyone");
        {
            for (const bool elgato : { true, false })
            {
                const auto state = wave3 ("off");

                expect (! profiles::isGoodProfile (state.profiles[0], elgato));     // off
                expect (! profiles::isGoodProfile (state.profiles[1], elgato));     // sink only
                expect (profiles::isGoodProfile (state.profiles[2], elgato));       // sink and source
                expect (! profiles::isGoodProfile (state.profiles[3], elgato));     // S/PDIF sink only
                expect (! profiles::isGoodProfile (state.profiles[4], elgato));     // S/PDIF with a mic: still S/PDIF
                expect (! profiles::isGoodProfile (state.profiles[5], elgato));     // source only
                expect (std::string (profiles::whyNotUsable (state.profiles[4], elgato)).find ("S/PDIF") != std::string::npos);
                expect (std::string (profiles::whyNotUsable (state.profiles[1], elgato)).find ("no mic") != std::string::npos);
            }
        }

        beginTest ("another device may stay in Pro Audio: it is accepted and left alone");
        {
            const auto state = wave3 ("pro-audio");

            expect (profiles::activeProfileIsGood (state, false));
            expect (profiles::whyNotUsable (state.profiles[6], false) == nullptr);
        }

        beginTest ("another device still prefers analog + mono-fallback to Pro Audio, and takes Pro Audio when it is all there is");
        {
            auto state = wave3 ("pro-audio");
            expectEquals (profiles::bestProfile (state, false), 2);

            state.profiles.erase (state.profiles.begin() + 2);          // no analog + mono-fallback on this one
            expectEquals (profiles::bestProfile (state, false), 6);
            expectEquals (profiles::bestProfile (state, true), -1);
        }

        beginTest ("a profile that is not available is never chosen");
        {
            auto state = wave3 ("pro-audio");
            state.profiles[2].available = false;                        // analog + mono-fallback unplugged

            expectEquals (profiles::bestProfile (state, true), -1);
            expectEquals (profiles::bestProfile (state, false), 6);
        }

        beginTest ("an active profile that is not in the list is not good, and the reason is generic");
        {
            auto state = wave3 ("pro-audio");
            state.activeProfile = 42;

            expect (! profiles::activeProfileIsGood (state, true));
            expect (! profiles::activeProfileIsGood (state, false));
            expect (profiles::whyActiveProfileIsNotGood (state, true) != nullptr);
        }

        beginTest ("findProfile finds a profile by name");
        {
            const auto state = wave3 ("pro-audio");

            expectEquals (profiles::findProfile (state, "off"), 0);
            expectEquals (profiles::findProfile (state, "pro-audio"), 6);
            expectEquals (profiles::findProfile (state, "no-such-profile"), -1);
        }
    }
};

static ProfileRulesTests profileRulesTests;
}
