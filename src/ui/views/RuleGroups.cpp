// RuleGroups implementation.
#include "ui/views/RuleGroups.h"

namespace hr
{
namespace
{
    struct SeededGroup
    {
        const char* name;
        std::vector<const char*> patterns;
    };

    // The rules AppState seeds on first run, by what they mean rather than how they match.
    const std::vector<SeededGroup>& seededGroups()
    {
        static const std::vector<SeededGroup> groups {
            { "Discord",                          { "discord", "vesktop", "webcord", "webrtc voiceengine" } },
            { "Spotify",                          { "spotify" } },
            { "Windows games (Steam / Proton)",   { "*.exe", "*wine*", "*proton*", "steam_app_*" } },
        };

        return groups;
    }

    int findGroup (const juce::String& pattern)
    {
        const auto& groups = seededGroups();

        for (size_t g = 0; g < groups.size(); ++g)
            for (auto* candidate : groups[g].patterns)
                if (pattern.equalsIgnoreCase (candidate))
                    return (int) g;

        return -1;
    }
}

std::vector<RuleGroup> groupRules (const std::vector<RoutingRule>& rules)
{
    std::vector<RuleGroup> result;
    std::vector<int> rowOfGroup (seededGroups().size(), -1);

    for (const auto& rule : rules)
    {
        const int g = findGroup (rule.pattern);

        if (g < 0)
        {
            RuleGroup row;
            row.name = friendlyAppName (rule.pattern);
            row.patterns.add (rule.pattern);
            row.channelId = rule.channelId;
            result.push_back (std::move (row));
            continue;
        }

        if (rowOfGroup[(size_t) g] < 0)
        {
            RuleGroup row;
            row.name = seededGroups()[(size_t) g].name;
            row.channelId = rule.channelId;
            row.isSeeded = true;
            rowOfGroup[(size_t) g] = (int) result.size();
            result.push_back (std::move (row));
        }

        result[(size_t) rowOfGroup[(size_t) g]].patterns.add (rule.pattern);
    }

    return result;
}

juce::String friendlyAppName (const juce::String& pattern)
{
    auto text = pattern.trim().toLowerCase();

    if (text.endsWith (".exe"))
        text = text.dropLastCharacters (4);

    text = text.removeCharacters ("*?").replaceCharacters ("_-", "  ").trim();

    if (text.isEmpty())
        return pattern.trim();

    juce::StringArray words;
    words.addTokens (text, " ", "");
    words.removeEmptyStrings();

    for (auto& word : words)
        word = word.substring (0, 1).toUpperCase() + word.substring (1);

    return words.joinIntoString (" ");
}

juce::String describePatterns (const juce::StringArray& patterns)
{
    return patterns.joinIntoString (", ");
}
}
