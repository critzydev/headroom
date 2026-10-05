// Wildcard matching of routing rules against an app's binary and name.
#include "backend/RuleMatcher.h"

namespace hr
{
juce::String matchRoutingRule (const std::vector<RoutingRule>& rules, const juce::String& binary, const juce::String& appName)
{
    // Binary first (more specific: "discord" vs a browser's "WEBRTC VoiceEngine"), then the application name.
    for (const auto* subject : { &binary, &appName })
    {
        if (subject->isEmpty())
            continue;

        for (const auto& rule : rules)
            if (rule.pattern.isNotEmpty() && subject->matchesWildcard (rule.pattern, true))
                return rule.channelId;
    }

    return {};
}
}
