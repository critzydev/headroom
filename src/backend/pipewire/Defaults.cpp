// Implementation of Defaults (default sink/source metadata).
#include "Defaults.h"

namespace hr::pw
{

Defaults::Defaults (PwRegistry& registryToUse) : registry (registryToUse)
{
    sink.key = "default.configured.audio.sink";
    sink.what = "output";
    source.key = "default.configured.audio.source";
    source.what = "input";
}

juce::String Defaults::gaveUpMessage()
{
    return "Another program keeps changing the default device. Headroom stopped switching it back.";
}

void Defaults::setEnabled (bool shouldBeDefault)
{
    enabled = shouldBeDefault;

    if (! enabled)
    {
        sink.keeper.reset();
        source.keeper.reset();
    }
}

void Defaults::setKeep (bool shouldKeep)
{
    if (keep == shouldKeep)
        return;

    keep = shouldKeep;

    // A fresh start either way: switching it on takes the current default as it is, switching it off ends the "given up".
    sink.keeper.reset();
    source.keeper.reset();
}

void Defaults::setTargets (const std::string& sinkNodeName, const std::string& sourceNodeName)
{
    if (sink.target != sinkNodeName)
    {
        sink.target = sinkNodeName;
        sink.asserted = false;
    }

    if (source.target != sourceNodeName)
    {
        source.target = sourceNodeName;
        source.asserted = false;
    }
}

void Defaults::metadataRebound()
{
    sink.asserted = false;
    source.asserted = false;
    sink.keeper.reset();
    source.keeper.reset();
}

void Defaults::reset()
{
    sink.asserted = source.asserted = false;
    sink.assertedNodeId = source.assertedNodeId = SPA_ID_INVALID;
    sink.lastWriteMs = source.lastWriteMs = 0;
    sink.keeper.reset();
    source.keeper.reset();
}

std::string Defaults::nameOf (const std::string& jsonValue)
{
    if (jsonValue.empty())
        return {};

    const auto parsed = juce::JSON::parse (toJuce (jsonValue));
    return toStd (parsed.getProperty ("name", juce::var()).toString());
}

void Defaults::evaluate (int64_t now)
{
    if (! registry.metadataReady())
        return;

    evaluateKind (sink, now);
    evaluateKind (source, now);
}

bool Defaults::writeTarget (Kind& kind, int64_t now)
{
    if (now - kind.lastWriteMs < 500)
        return false;

    const auto* node = registry.nodeByName (kind.target);

    if (node == nullptr)
        return false;

    const std::string value = "{ \"name\": \"" + kind.target + "\" }";

    if (! registry.setMetadata (0, kind.key, "Spa:String:JSON", value.c_str()))
        return false;

    kind.asserted = true;
    kind.assertedNodeId = node->id;
    kind.lastWriteMs = now;
    return true;
}

void Defaults::evaluateKind (Kind& kind, int64_t now)
{
    const std::string current = nameOf (registry.metadataValue (0, kind.key));

    if (! enabled)
    {
        // Give the hardware back, but only if the value is ours.
        if (startsWith (current, kNodePrefix) && now - kind.lastWriteMs >= 1000)
        {
            registry.setMetadata (0, kind.key, nullptr, nullptr);
            kind.lastWriteMs = now;
            pwLog ("cleared %s", kind.key);
        }

        kind.asserted = false;
        kind.keeper.reset();
        return;
    }

    if (kind.target.empty())
        return;

    const auto* node = registry.nodeByName (kind.target);

    if (node == nullptr)
        return;   // our node doesn't exist (yet)

    if (current == kind.target)
    {
        kind.asserted = true;
        kind.assertedNodeId = node->id;
        kind.keeper.update (false, {}, now);
        return;
    }

    const bool firstTime = ! kind.asserted;
    const bool recreated = kind.asserted && kind.assertedNodeId != node->id;

    if (firstTime || recreated)
    {
        // Start-up, or our node is new: not somebody else's doing, so no debounce and no counting.
        if (writeTarget (kind, now))
            pwLog ("set %s to %s", kind.key, kind.target.c_str());

        return;
    }

    // Another Headroom node picked on purpose (e.g. "Headroom Music" in the KDE applet) still goes through Headroom,
    // so the lock leaves it alone.
    if (keep && startsWith (current, kNodePrefix) && registry.nodeByName (current) != nullptr)
    {
        kind.keeper.update (false, {}, now);
        return;
    }

    if (keep)
    {
        keepKind (kind, current, now);
        return;
    }

    // Without the lock a configured default that is empty, or names a device which doesn't exist (an old profile's
    // node names, an unplugged device), is put back: it can't be what the user is listening to. A device that exists
    // is a deliberate choice: leave it.
    const bool namesNothing = ! current.empty() && registry.nodeByName (current) == nullptr;

    if ((current.empty() || namesNothing) && writeTarget (kind, now))
        pwLog ("set %s to %s", kind.key, kind.target.c_str());
}

/** keepDefault: anything but Headroom's own node (another device, a device that is gone, nothing at all) is put back. */
void Defaults::keepKind (Kind& kind, const std::string& current, int64_t now)
{
    switch (kind.keeper.update (true, current, now))
    {
        case hr::DefaultKeeper::Action::None:
            break;

        case hr::DefaultKeeper::Action::GiveUp:
            pwLog ("keep default: default %s was changed more than %d times in %d seconds by another program, "
                   "no longer switching it back", kind.what, hr::DefaultKeeper::kMaxReverts, (int) (hr::DefaultKeeper::kWindowMs / 1000));
            break;

        case hr::DefaultKeeper::Action::Revert:
            if (writeTarget (kind, now))
            {
                kind.keeper.reverted (now);

                if (current.empty())
                    pwLog ("keep default: default %s was cleared, switching back to Headroom", kind.what);
                else
                    pwLog ("keep default: default %s was changed to %s, switching back to Headroom", kind.what, current.c_str());
            }

            break;
    }
}

} // namespace hr::pw
