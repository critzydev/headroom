// Keeps the set of links Headroom owns equal to the set it wants: create what is missing, destroy what is stale.
#pragma once

#include "PwCommon.h"
#include "PwRegistry.h"

namespace hr::pw
{

/** Why a link exists. WaveGuard gates the capture and playback kinds independently. */
enum class LinkKind
{
    Internal,   // channel sinks -> engine, engine -> Headroom Mic / Stream Mix
    Capture,    // mic device -> engine
    Playback    // engine -> headphones
};

struct LinkRequest
{
    uint32_t outPort = SPA_ID_INVALID;
    uint32_t inPort = SPA_ID_INVALID;
    LinkKind kind = LinkKind::Internal;

    uint64_t key() const noexcept { return ((uint64_t) outPort << 32) | inPort; }
};

/**
    reconcile() is idempotent and cheap: call it after every registry change. Links are always
    created with object.linger=false. A link the server refuses is retried with a growing pause,
    so a bad port pairing can't spin the loop.
*/
class LinkManager
{
public:
    LinkManager() = default;

    void attach (pw_core* core);
    void detach();

    void reconcile (const std::vector<LinkRequest>& desired, const PwRegistry& registry, int64_t nowMs);

    /** Number of links of this kind that we have created and that are (still) alive. */
    int ownedCount (LinkKind kind) const;

    /** A proxy event happened: the owner should run its update pass soon. */
    std::function<void()> onEvent;

private:
    struct Owned
    {
        LinkRequest request;
        ProxyHolder proxy;
    };

    struct Retry
    {
        int failures = 0;
        int64_t notBeforeMs = 0;
    };

    pw_core* coreRef = nullptr;
    std::map<uint64_t, std::unique_ptr<Owned>> owned;
    std::map<uint64_t, Retry> retries;
};

} // namespace hr::pw
