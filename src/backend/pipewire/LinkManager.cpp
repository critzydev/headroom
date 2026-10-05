// Implementation of LinkManager (desired-vs-actual link reconciliation).
#include "LinkManager.h"

namespace hr::pw
{

void LinkManager::attach (pw_core* core)
{
    detach();
    coreRef = core;
}

void LinkManager::detach()
{
    owned.clear();     // destroys the proxies, which removes the links
    retries.clear();
    coreRef = nullptr;
}

int LinkManager::ownedCount (LinkKind kind) const
{
    int count = 0;

    for (const auto& entry : owned)
        if (entry.second->request.kind == kind && entry.second->proxy.alive())
            ++count;

    return count;
}

void LinkManager::reconcile (const std::vector<LinkRequest>& desired, const PwRegistry& registry, int64_t now)
{
    if (coreRef == nullptr)
        return;

    std::set<uint64_t> wanted;

    for (const auto& request : desired)
        wanted.insert (request.key());

    // 1. Forget links that died (server removed them, a port vanished) or that we no longer want.
    for (auto it = owned.begin(); it != owned.end();)
    {
        auto& link = *it->second;
        const bool dead = ! link.proxy.alive() || link.proxy.serverRemoved || link.proxy.gone;
        const bool unwanted = wanted.count (it->first) == 0;

        if (link.proxy.failed)
        {
            auto& retry = retries[it->first];
            ++retry.failures;
            retry.notBeforeMs = now + juce::jmin (10000, 500 << juce::jmin (retry.failures - 1, 5));
        }

        if (dead || unwanted || link.proxy.failed)
            it = owned.erase (it);
        else
            ++it;
    }

    // 2. Create what is missing.
    for (const auto& request : desired)
    {
        const auto key = request.key();

        if (owned.count (key) > 0)
            continue;

        if (registry.hasLink (request.outPort, request.inPort))
        {
            retries.erase (key);   // it exists (ours pending removal, or someone else's): nothing to do
            continue;
        }

        const auto retry = retries.find (key);

        if (retry != retries.end() && now < retry->second.notBeforeMs)
            continue;

        Props props;
        props.set (PW_KEY_LINK_OUTPUT_PORT, std::to_string (request.outPort))
             .set (PW_KEY_LINK_INPUT_PORT, std::to_string (request.inPort))
             .set (PW_KEY_OBJECT_LINGER, "false");

        void* raw = pw_core_create_object (coreRef, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, props.dict(), 0);

        if (raw == nullptr)
        {
            auto& failure = retries[key];
            ++failure.failures;
            failure.notBeforeMs = now + juce::jmin (10000, 500 << juce::jmin (failure.failures - 1, 5));
            continue;
        }

        auto link = std::make_unique<Owned>();
        link->request = request;
        link->proxy.adopt (raw);
        link->proxy.onEvent = [this] { if (onEvent) onEvent(); };
        owned[key] = std::move (link);
    }

    // A link that made it into the registry has earned a clean slate.
    for (auto it = retries.begin(); it != retries.end();)
    {
        const auto outPort = (uint32_t) (it->first >> 32);
        const auto inPort = (uint32_t) (it->first & 0xffffffffu);

        if (registry.hasLink (outPort, inPort))
            it = retries.erase (it);
        else
            ++it;
    }
}

} // namespace hr::pw
