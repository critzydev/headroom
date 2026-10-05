// Listing and choosing capture / playback devices ("auto" prefers an Elgato Wave of any model; the rules are in backend/DeviceRules.h).
#pragma once

#include "PwCommon.h"
#include "PwRegistry.h"

#include "backend/AudioBackend.h"

namespace hr::pw
{

/** Selectable devices, never Headroom's own nodes. Ids are node.name. */
std::vector<hr::DeviceInfo> listDevices (const PwRegistry& registry, bool inputs);

/**
    "auto" (or an id that is not present any more) prefers an Elgato Wave of any model, then the node with the
    highest priority.session. An explicit id that exists is honoured. Returns nullptr if no
    device of that kind exists.
*/
const NodeRec* pickDevice (const PwRegistry& registry, bool inputs, const std::string& requestedId);

/** One device a mix plays to. `node` is nullptr when that device is not there. */
struct ResolvedOutput
{
    const NodeRec* node = nullptr;
    std::string requested;              // the stored id ("auto" or a node.name)
    bool automatic = false;             // it was "auto"
};

/**
    Everything a mix plays to, from the ids the model stores (devices::resolveOutputs): "auto" is what it always was, an id that
    is there is honoured, the same device twice is one entry, and an id that is not there stays in the list as an entry without
    a node (so the UI can say so). With `fallBackToAuto`, a list in which no device is there gets the automatic choice appended.
*/
std::vector<ResolvedOutput> pickOutputs (const PwRegistry& registry, const std::vector<std::string>& requested, bool fallBackToAuto);

/** The capture device of an input channel: exactly the node it names, never an "automatic" one. nullptr when it is not there. */
const NodeRec* findInputNode (const PwRegistry& registry, const std::string& nodeName);

/** Friendly name for the UI: "Elgato Wave:1" / "Elgato Wave XLR Headphones" for a Wave (ElgatoModels.h), the device's own description otherwise. */
juce::String deviceLabel (const PwRegistry& registry, const NodeRec& node, bool input);

} // namespace hr::pw
