// Implementation of AppRouter (rule matching and metadata moves).
#include "AppRouter.h"

#include "VirtualNodes.h"

#include "backend/RuleMatcher.h"

namespace hr::pw
{

void AppRouter::setRules (std::vector<hr::RoutingRule> newRules, const juce::String& fallbackChannel)
{
    bool same = fallbackChannel == fallback && newRules.size() == rules.size();

    for (size_t i = 0; same && i < newRules.size(); ++i)
        same = newRules[i].pattern == rules[i].pattern && newRules[i].channelId == rules[i].channelId;

    rules = std::move (newRules);
    fallback = fallbackChannel;

    // New rules apply to streams that are already playing; unchanged rules must not
    // pull a stream back that the user moved by hand.
    if (! same)
        placed.clear();
}

void AppRouter::routeNow (uint32_t streamNodeId, const juce::String& channelId)
{
    manual[streamNodeId] = channelId;
    placed.erase (streamNodeId);
}

void AppRouter::reset()
{
    placed.clear();
    manual.clear();
}

void AppRouter::evaluate()
{
    if (! registry.metadataReady())
        return;

    std::set<uint32_t> present;

    for (const auto& entry : registry.nodes())
    {
        const auto& stream = entry.second;

        if (! stream.isStreamOutput() || stream.isOurs())
            continue;

        present.insert (stream.id);

        // Wait for the bound info: application.process.binary only shows up there.
        if (! stream.infoReceived || stream.dontReconnect)
            continue;

        juce::String channel;
        const auto explicitChoice = manual.find (stream.id);

        if (explicitChoice != manual.end())
            channel = explicitChoice->second;
        else
            channel = matchRoutingRule (rules, toJuce (stream.binary), toJuce (stream.appName));

        if (channel.isEmpty())
            continue;

        const auto* sink = registry.nodeByName (VirtualNodes::channelNodeName (channel));

        if (sink == nullptr || sink->serial == 0)
            continue;   // the channel's sink doesn't exist (yet)

        const auto already = placed.find (stream.id);

        if (already != placed.end() && already->second.sinkId == sink->id && already->second.sinkSerial == sink->serial)
            continue;

        const std::string nodeId = std::to_string (sink->id);
        const std::string serial = std::to_string (sink->serial);

        const bool ok = registry.setMetadata (stream.id, "target.node", "Spa:Id", nodeId.c_str())
                     && registry.setMetadata (stream.id, "target.object", "Spa:Id", serial.c_str());

        if (ok)
        {
            placed[stream.id] = { sink->id, sink->serial };
            pwLog ("routed stream %u (%s) to %s", stream.id,
                   stream.binary.empty() ? stream.appName.c_str() : stream.binary.c_str(), sink->name.c_str());
        }
    }

    for (auto it = placed.begin(); it != placed.end();)
        it = present.count (it->first) == 0 ? placed.erase (it) : std::next (it);

    for (auto it = manual.begin(); it != manual.end();)
        it = present.count (it->first) == 0 ? manual.erase (it) : std::next (it);
}

juce::String AppRouter::channelOf (uint32_t streamNodeId) const
{
    for (const auto& entry : registry.links())
    {
        if (entry.second.outNode != streamNodeId)
            continue;

        if (const auto* target = registry.node (entry.second.inNode))
            if (startsWith (target->name, kChannelPrefix))
                return VirtualNodes::channelIdFromNodeName (target->name);
    }

    return {};
}

} // namespace hr::pw
