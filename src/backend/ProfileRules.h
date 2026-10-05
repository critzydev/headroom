// Which profile of an ALSA device is usable for the mic and the headphones, and which one to switch to.
// Free of PipeWire (plain data and pure functions), so it can be tested anywhere.
#pragma once

#include <string>
#include <vector>

namespace hr::pw
{

struct ProfileRec
{
    int index = -1;
    std::string name, description;
    int priority = 0;
    bool available = true;
    int sinks = 0;      // number of Audio/Sink nodes the profile creates
    int sources = 0;    // number of Audio/Source nodes the profile creates
};

/** One active route (Route param): where a device's node gets its volume and mute. */
struct RouteRec
{
    int index = -1;
    int direction = -1;         // SPA_DIRECTION_INPUT (0) = capture, SPA_DIRECTION_OUTPUT (1) = playback
    int device = -1;            // card.profile.device of the node it belongs to
    std::string name;
    bool mute = false;
    bool softMute = false;
    std::vector<float> volumes; // linear, as pactl would show cubed
};

struct DeviceState
{
    std::vector<ProfileRec> profiles;
    int activeProfile = -1;
    std::string activeProfileName;
    std::vector<RouteRec> routes;
    bool profilesSeen = false;
    bool activeSeen = false;
};

/**
    A profile is usable when it has a source and a sink and is neither "off" nor an S/PDIF (iec958) profile, which the
    Wave:3 doesn't have. For an Elgato device (USB vendor 0x0fd9) "pro-audio" is not usable either: it has no ACP routes,
    so neither WirePlumber nor Headroom can set or restore the mic's hardware gain in it. For any other device
    "pro-audio" counts as good: Headroom does not fight a profile somebody chose on purpose.
*/
namespace profiles
{
    /** nullptr when the profile is usable; else a short plain reason, for the log. */
    const char* whyNotUsable (const ProfileRec& profile, bool elgato);

    bool isGoodProfile (const ProfileRec& profile, bool elgato);

    /** The active profile is a usable one (false too when it is not in the list, or nothing is known yet). */
    bool activeProfileIsGood (const DeviceState& state, bool elgato);

    /** Why the active profile is not usable (see whyNotUsable); a generic reason if it is not in the list. */
    const char* whyActiveProfileIsNotGood (const DeviceState& state, bool elgato);

    /** The profile every device prefers when nothing else is said (a mono mic and stereo headphones). */
    constexpr const char* kDefaultPreferred = "output:analog-stereo+input:mono-fallback";

    /** Index of the best usable, available profile (analog stereo + mono-fallback first), or -1. */
    int bestProfile (const DeviceState& state, bool elgato);

    /** The same, with the profile that gets the head start named by the caller: an Elgato model whose card does not have the
        Wave:3's layout (an XLR interface with a stereo or multichannel mic) passes "" and gets the highest priority one. */
    int bestProfile (const DeviceState& state, bool elgato, const char* preferred);

    int findProfile (const DeviceState& state, const char* name);
}

} // namespace hr::pw
