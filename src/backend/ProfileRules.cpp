// Implementation of the profile rules (usable / best profile of an ALSA device).
#include "backend/ProfileRules.h"

namespace hr::pw::profiles
{

const char* whyNotUsable (const ProfileRec& profile, bool elgato)
{
    if (profile.name == "off")
        return "the device is switched off";

    if (profile.name.find ("iec958") != std::string::npos)
        return "S/PDIF only";

    if (elgato && profile.name == "pro-audio")
        return "no hardware gain control in Pro Audio";

    if (profile.sinks <= 0 || profile.sources <= 0)
        return "no mic or no headphones";

    return nullptr;
}

bool isGoodProfile (const ProfileRec& profile, bool elgato)
{
    return whyNotUsable (profile, elgato) == nullptr;
}

bool activeProfileIsGood (const DeviceState& state, bool elgato)
{
    for (const auto& profile : state.profiles)
        if (profile.index == state.activeProfile)
            return isGoodProfile (profile, elgato);

    return false;
}

const char* whyActiveProfileIsNotGood (const DeviceState& state, bool elgato)
{
    for (const auto& profile : state.profiles)
        if (profile.index == state.activeProfile)
            if (const char* reason = whyNotUsable (profile, elgato))
                return reason;

    return "no mic, no headphones, or S/PDIF";
}

int bestProfile (const DeviceState& state, bool elgato)
{
    return bestProfile (state, elgato, kDefaultPreferred);
}

int bestProfile (const DeviceState& state, bool elgato, const char* preferred)
{
    int best = -1;
    int bestScore = 0;
    bool haveBest = false;

    for (const auto& profile : state.profiles)
    {
        if (! profile.available || ! isGoodProfile (profile, elgato))
            continue;

        int score = profile.priority;

        if (preferred != nullptr && preferred[0] != '\0' && profile.name == preferred)
            score += 1000000;
        else if (profile.name == "pro-audio")
            score -= 1000000;   // (not an Elgato device:) fine to stay in, but never the one we pick for someone

        if (! haveBest || score > bestScore)
        {
            best = profile.index;
            bestScore = score;
            haveBest = true;
        }
    }

    return best;
}

int findProfile (const DeviceState& state, const char* name)
{
    for (const auto& profile : state.profiles)
        if (profile.name == name)
            return profile.index;

    return -1;
}

} // namespace hr::pw::profiles
