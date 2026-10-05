// RuleGroups: turns the routing rules into rows a person can read (Apps > Remembered). The model keeps one rule per
// pattern; the seeded ones are shown as three friendly groups and user rules under a proper app name.
#pragma once

#include "Juce.h"
#include "backend/BackendTypes.h"

#include <vector>

namespace hr
{
struct RuleGroup
{
    juce::String name;                  // "Discord", "Windows games (Steam / Proton)", "Spotify", "Firefox"
    juce::StringArray patterns;         // every pattern in the model that this row stands for (changes / removals hit all)
    juce::String channelId;
    bool isSeeded = false;
};

// Keeps the model's order: a group sits where its first rule was.
std::vector<RuleGroup> groupRules (const std::vector<RoutingRule>& rules);

// "spotify" -> "Spotify", "cyberpunk2077.exe" -> "Cyberpunk2077", "steam_app_*" -> "Steam app". Never empty for a
// non-empty pattern.
juce::String friendlyAppName (const juce::String& pattern);

// "discord, vesktop, webcord": what the tooltip says a row matches.
juce::String describePatterns (const juce::StringArray& patterns);
}
