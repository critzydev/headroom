// Shared app-to-channel rule matching so every backend routes exactly like the NullBackend demo.
#pragma once

#include "backend/BackendTypes.h"

#include <vector>

namespace hr
{
// Returns the channelId of the first rule whose wildcard matches the app's binary, then (for the whole rule list)
// its application name; both compared case-insensitively. Returns an empty string if nothing matches, in which
// case the stream stays on the default sink (= the fallback channel).
juce::String matchRoutingRule (const std::vector<RoutingRule>& rules, const juce::String& binary, const juce::String& appName);
}
