// Implementation of device listing and selection. The rules themselves (which device "auto" means, the order of the list,
// the labels of the Elgato models) are plain functions in backend/DeviceRules and backend/ElgatoModels, tested everywhere.
#include "DeviceSelector.h"

#include "backend/DeviceRules.h"

namespace hr::pw
{

namespace
{
    bool isCandidate (const NodeRec& node, bool inputs)
    {
        if (node.isOurs() || node.name.empty())
            return false;

        return inputs ? node.isSource() : node.isSink();
    }
}

juce::String deviceLabel (const PwRegistry& registry, const NodeRec& node, bool input)
{
    const auto who = registry.identityOf (node);

    if (who.wave)
    {
        std::string deviceDescription, deviceNick;

        if (const auto* owner = registry.device (node.deviceId))
        {
            deviceDescription = owner->description;
            deviceNick = owner->nick;
        }

        return toJuce (elgato::label (who, input, deviceDescription, deviceNick, node.description, node.nick));
    }

    if (! node.description.empty())
        return toJuce (node.description);

    if (! node.nick.empty())
        return toJuce (node.nick);

    return toJuce (node.name);
}

std::vector<hr::DeviceInfo> listDevices (const PwRegistry& registry, bool inputs)
{
    std::vector<hr::DeviceInfo> result;

    for (const auto& entry : registry.nodes())
    {
        const auto& node = entry.second;

        if (! isCandidate (node, inputs))
            continue;

        hr::DeviceInfo info;
        info.id = toJuce (node.name);
        info.label = deviceLabel (registry, node, inputs);
        info.isWave = registry.isWaveNode (node);
        info.channels = (int) registry.portsOf (node.id, ! inputs, PwRegistry::PortKind::Any).size();     // a source's capture ports are its outputs
        result.push_back (std::move (info));
    }

    devices::sortDeviceList (result);
    return result;
}

namespace
{
    // The nodes that could be chosen, as the pure rules see them (candidates[i] is nodes[i]).
    void collectCandidates (const PwRegistry& registry, bool inputs, std::vector<devices::Candidate>& candidates, std::vector<const NodeRec*>& nodes)
    {
        for (const auto& entry : registry.nodes())
        {
            const auto& node = entry.second;

            if (! isCandidate (node, inputs))
                continue;

            devices::Candidate candidate;
            candidate.name = node.name;
            candidate.real = node.deviceId != SPA_ID_INVALID;
            candidate.priority = node.prioritySession;
            candidate.wave = registry.isWaveNode (node);
            candidates.push_back (std::move (candidate));
            nodes.push_back (&node);
        }
    }
}

const NodeRec* pickDevice (const PwRegistry& registry, bool inputs, const std::string& requestedId)
{
    std::vector<devices::Candidate> candidates;
    std::vector<const NodeRec*> nodes;
    collectCandidates (registry, inputs, candidates, nodes);

    const int index = devices::pickCandidate (candidates, requestedId);
    return index >= 0 ? nodes[(size_t) index] : nullptr;
}

std::vector<ResolvedOutput> pickOutputs (const PwRegistry& registry, const std::vector<std::string>& requested, bool fallBackToAuto)
{
    std::vector<devices::Candidate> candidates;
    std::vector<const NodeRec*> nodes;
    collectCandidates (registry, false, candidates, nodes);

    std::vector<ResolvedOutput> result;

    for (const auto& choice : devices::resolveOutputs (candidates, requested, fallBackToAuto))
    {
        ResolvedOutput output;
        output.requested = choice.requested;
        output.automatic = choice.automatic;
        output.node = choice.index >= 0 ? nodes[(size_t) choice.index] : nullptr;
        result.push_back (std::move (output));
    }

    return result;
}

const NodeRec* findInputNode (const PwRegistry& registry, const std::string& nodeName)
{
    if (nodeName.empty())
        return nullptr;

    for (const auto& entry : registry.nodes())
        if (isCandidate (entry.second, true) && entry.second.name == nodeName)
            return &entry.second;

    return nullptr;
}

} // namespace hr::pw
