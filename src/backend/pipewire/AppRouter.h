// Moves app playback streams to channel sinks according to the routing rules (metadata target.node/target.object).
#pragma once

#include "PwCommon.h"
#include "PwRegistry.h"

#include "backend/AudioBackend.h"

namespace hr::pw
{

/**
    Does what "pactl move-sink-input" does: writes target.node and target.object into the
    "default" metadata with the stream's id as subject; WirePlumber then relinks the stream.
    Each stream is placed once (when its properties are known), and again if the sink it was
    placed on is recreated. A user who later moves a stream by hand is left alone.
*/
class AppRouter
{
public:
    explicit AppRouter (PwRegistry& registryToUse) : registry (registryToUse) {}

    void setRules (std::vector<hr::RoutingRule> newRules, const juce::String& fallbackChannel);

    /** Route this stream to a channel now, and keep it there (the explicit choice beats the rules). */
    void routeNow (uint32_t streamNodeId, const juce::String& channelId);

    /** Apply rules to every stream that isn't placed yet. Cheap; call on every pass. */
    void evaluate();

    void reset();

    /** The channel a stream's output is currently linked to, "" if it isn't ours. */
    juce::String channelOf (uint32_t streamNodeId) const;

private:
    struct Placed
    {
        uint32_t sinkId = SPA_ID_INVALID;
        uint64_t sinkSerial = 0;
    };

    PwRegistry& registry;
    std::vector<hr::RoutingRule> rules;
    juce::String fallback;
    std::map<uint32_t, Placed> placed;
    std::map<uint32_t, juce::String> manual;
};

} // namespace hr::pw
