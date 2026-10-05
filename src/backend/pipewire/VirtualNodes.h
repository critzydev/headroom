// The virtual devices Headroom publishes: one sink per channel, "Headroom Mic" and "Headroom Stream Mix".
#pragma once

#include "PwCommon.h"
#include "PwRegistry.h"

#include "backend/AudioBackend.h"

namespace hr::pw
{

/**
    Creates adapter nodes (support.null-audio-sink) through the server's "adapter" factory.
    Everything is object.linger=false, so the nodes disappear if Headroom dies. Renaming a
    channel is destroy + recreate under the same node.name.
*/
class VirtualNodes
{
public:
    VirtualNodes() = default;

    void attach (pw_core* core);
    void detach();

    void setChannels (std::vector<hr::ChannelEndpoint> channels);
    const std::vector<hr::ChannelEndpoint>& channels() const noexcept { return channelList; }

    /** Makes the set of nodes match the wanted set. Idempotent; call whenever anything may have changed. */
    void sync (int64_t nowMs);

    /** True once every wanted node has been created (they may not have appeared in the registry yet). */
    bool allCreated() const;

    /** A proxy event happened (created, failed, removed): the owner should run its update pass. */
    std::function<void()> onEvent;

    static std::string channelNodeName (const juce::String& channelId);
    static juce::String channelIdFromNodeName (const std::string& nodeName);

private:
    struct Wanted
    {
        std::string name, description, mediaClass;
        bool isSink = false;
    };

    struct Node
    {
        Wanted wanted;
        ProxyHolder proxy;
        int failures = 0;
        int64_t retryAtMs = 0;
    };

    std::vector<Wanted> buildWanted() const;
    void create (Node& node);

    pw_core* coreRef = nullptr;
    std::vector<hr::ChannelEndpoint> channelList;
    std::map<std::string, std::unique_ptr<Node>> nodes;
};

} // namespace hr::pw
