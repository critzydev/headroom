// Implementation of VirtualNodes: create / rename / remove the virtual sinks and sources.
#include "VirtualNodes.h"

namespace hr::pw
{

std::string VirtualNodes::channelNodeName (const juce::String& channelId)
{
    return std::string (kChannelPrefix) + toStd (channelId);
}

juce::String VirtualNodes::channelIdFromNodeName (const std::string& nodeName)
{
    if (! startsWith (nodeName, kChannelPrefix))
        return {};

    return toJuce (nodeName.substr (std::strlen (kChannelPrefix)));
}

void VirtualNodes::attach (pw_core* core)
{
    detach();
    coreRef = core;
}

void VirtualNodes::detach()
{
    // Destroying the proxies removes the nodes from the graph (object.linger=false).
    nodes.clear();
    coreRef = nullptr;
}

void VirtualNodes::setChannels (std::vector<hr::ChannelEndpoint> channels)
{
    channelList = std::move (channels);
}

std::vector<VirtualNodes::Wanted> VirtualNodes::buildWanted() const
{
    std::vector<Wanted> result;

    for (const auto& channel : channelList)
    {
        Wanted w;
        w.name = channelNodeName (channel.channelId);
        w.description = "Headroom " + toStd (channel.displayName);
        w.mediaClass = "Audio/Sink";
        w.isSink = true;
        result.push_back (std::move (w));
    }

    Wanted mic;
    mic.name = kMicSourceName;
    mic.description = "Headroom Mic";
    mic.mediaClass = "Audio/Source/Virtual";
    result.push_back (std::move (mic));

    Wanted stream;
    stream.name = kStreamSourceName;
    stream.description = "Headroom Stream Mix";
    stream.mediaClass = "Audio/Source/Virtual";
    result.push_back (std::move (stream));

    return result;
}

void VirtualNodes::create (Node& node)
{
    if (coreRef == nullptr)
        return;

    Props props;
    props.set ("factory.name", "support.null-audio-sink")
         .set (PW_KEY_NODE_NAME, node.wanted.name)
         .set (PW_KEY_NODE_DESCRIPTION, node.wanted.description)
         .set (PW_KEY_MEDIA_CLASS, node.wanted.mediaClass)
         .set ("audio.position", "FL,FR")
         .set (PW_KEY_NODE_VIRTUAL, "true")
         .set (PW_KEY_OBJECT_LINGER, "false");

    if (node.wanted.isSink)
        props.set ("monitor.channel-volumes", "true");

    void* raw = pw_core_create_object (coreRef, "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, props.dict(), 0);

    node.proxy.adopt (raw);
    node.proxy.onEvent = [this] { if (onEvent) onEvent(); };

    if (raw == nullptr)
        pwLog ("could not create %s", node.wanted.name.c_str());
    else
        pwLog ("created %s", node.wanted.name.c_str());
}

void VirtualNodes::sync (int64_t now)
{
    if (coreRef == nullptr)
        return;

    const auto wanted = buildWanted();

    // Remove what is no longer wanted, and recreate what was renamed.
    for (auto it = nodes.begin(); it != nodes.end();)
    {
        const auto match = std::find_if (wanted.begin(), wanted.end(),
                                         [&] (const Wanted& w) { return w.name == it->first; });

        const bool stale = match == wanted.end() || match->description != it->second->wanted.description;

        if (stale)
        {
            pwLog ("removing %s", it->first.c_str());
            it = nodes.erase (it);
        }
        else
        {
            ++it;
        }
    }

    for (const auto& w : wanted)
    {
        auto found = nodes.find (w.name);

        if (found == nodes.end())
        {
            auto node = std::make_unique<Node>();
            node->wanted = w;
            create (*node);
            nodes[w.name] = std::move (node);
            continue;
        }

        auto& node = *found->second;
        const bool broken = node.proxy.gone || node.proxy.serverRemoved || node.proxy.failed || ! node.proxy.alive();

        if (! broken)
        {
            if (node.proxy.boundId != SPA_ID_INVALID)
                node.failures = 0;   // it lived; the next failure starts the backoff over

            node.retryAtMs = 0;
            continue;
        }

        // Back off (0.5 s ... 5 s) so a node the server keeps refusing doesn't spin.
        if (node.retryAtMs == 0)
        {
            ++node.failures;
            node.retryAtMs = now + juce::jmin (5000, 500 << juce::jmin (node.failures - 1, 4));
        }

        if (now < node.retryAtMs)
            continue;

        node.retryAtMs = 0;
        node.proxy.destroy();
        create (node);
    }
}

bool VirtualNodes::allCreated() const
{
    for (const auto& entry : nodes)
        if (! entry.second->proxy.alive() || entry.second->proxy.failed)
            return false;

    return ! nodes.empty();
}

} // namespace hr::pw
