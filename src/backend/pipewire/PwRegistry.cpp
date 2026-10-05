// Implementation of the graph mirror: registry events, bound node/device info, "default" metadata.
#include "PwRegistry.h"

#include <algorithm>

namespace hr::pw
{

namespace
{
    void assignIfPresent (std::string& target, const spa_dict* props, const char* key)
    {
        if (props == nullptr)
            return;

        if (const char* value = spa_dict_lookup (props, key))
            target = value;
    }
} // namespace

PwRegistry::PwRegistry() = default;

PwRegistry::~PwRegistry()
{
    detach();
}

// -------------------------------------------------------------------------------------------------
// Lifetime

void PwRegistry::attach (pw_core* core)
{
    detach();
    coreRef = core;

    registry = pw_core_get_registry (core, PW_VERSION_REGISTRY, 0);

    if (registry == nullptr)
    {
        pwLog ("could not get the registry");
        return;
    }

    static const pw_registry_events events = []
    {
        pw_registry_events e {};
        e.version = PW_VERSION_REGISTRY_EVENTS;
        e.global = &PwRegistry::onGlobalCb;
        e.global_remove = &PwRegistry::onGlobalRemoveCb;
        return e;
    }();

    std::memset (&registryHook, 0, sizeof (registryHook));
    pw_registry_add_listener (registry, &registryHook, &events, this);
    registryHookActive = true;
}

void PwRegistry::detach()
{
    // Order matters: hooks first, then the proxies they belong to.
    if (registryHookActive)
    {
        spa_hook_remove (&registryHook);
        registryHookActive = false;
    }

    metadata.reset();
    nodeProxies.clear();

    for (const auto& entry : deviceProxies)
        if (onDeviceRemoved)
            onDeviceRemoved (entry.first);

    deviceProxies.clear();

    if (registry != nullptr)
    {
        pw_proxy_destroy (reinterpret_cast<pw_proxy*> (registry));
        registry = nullptr;
    }

    nodeRecs.clear();
    portRecs.clear();
    linkRecs.clear();
    deviceRecs.clear();
    metadataValues.clear();
    coreRef = nullptr;
}

void PwRegistry::notifyChanged()
{
    if (onChanged)
        onChanged();
}

// -------------------------------------------------------------------------------------------------
// Registry events

void PwRegistry::onGlobalCb (void* data, uint32_t id, uint32_t, const char* type, uint32_t, const spa_dict* props)
{
    static_cast<PwRegistry*> (data)->handleGlobal (id, type, props);
}

void PwRegistry::onGlobalRemoveCb (void* data, uint32_t id)
{
    static_cast<PwRegistry*> (data)->handleGlobalRemove (id);
}

void PwRegistry::handleGlobal (uint32_t id, const char* type, const spa_dict* props)
{
    if (type == nullptr)
        return;

    if (std::strcmp (type, PW_TYPE_INTERFACE_Node) == 0)
        addNode (id, props);
    else if (std::strcmp (type, PW_TYPE_INTERFACE_Port) == 0)
        addPort (id, props);
    else if (std::strcmp (type, PW_TYPE_INTERFACE_Link) == 0)
        addLink (id, props);
    else if (std::strcmp (type, PW_TYPE_INTERFACE_Device) == 0)
        addDevice (id, props);
    else if (std::strcmp (type, PW_TYPE_INTERFACE_Metadata) == 0)
        addMetadata (id, props);
    else
        return;

    notifyChanged();
}

void PwRegistry::handleGlobalRemove (uint32_t id)
{
    bool touched = false;

    if (nodeRecs.erase (id) > 0)
    {
        nodeProxies.erase (id);
        touched = true;
    }

    touched |= portRecs.erase (id) > 0;
    touched |= linkRecs.erase (id) > 0;

    if (deviceRecs.erase (id) > 0)
    {
        if (onDeviceRemoved)
            onDeviceRemoved (id);

        deviceProxies.erase (id);
        touched = true;
    }

    if (metadata != nullptr && metadata->id == id)
    {
        pwLog ("the default metadata went away");
        metadata.reset();
        metadataValues.clear();
        touched = true;
    }

    if (touched)
        notifyChanged();
}

// -------------------------------------------------------------------------------------------------
// Nodes

void PwRegistry::mergeNodeProps (NodeRec& rec, const spa_dict* props)
{
    if (props == nullptr)
        return;

    assignIfPresent (rec.name, props, PW_KEY_NODE_NAME);
    assignIfPresent (rec.description, props, PW_KEY_NODE_DESCRIPTION);
    assignIfPresent (rec.nick, props, PW_KEY_NODE_NICK);
    assignIfPresent (rec.mediaClass, props, PW_KEY_MEDIA_CLASS);
    assignIfPresent (rec.appName, props, PW_KEY_APP_NAME);
    assignIfPresent (rec.binary, props, PW_KEY_APP_PROCESS_BINARY);
    assignIfPresent (rec.iconName, props, PW_KEY_APP_ICON_NAME);
    assignIfPresent (rec.mediaName, props, PW_KEY_MEDIA_NAME);

    if (const char* value = spa_dict_lookup (props, PW_KEY_OBJECT_SERIAL))
        rec.serial = parseU64 (value);

    if (const char* value = spa_dict_lookup (props, PW_KEY_DEVICE_ID))
        rec.deviceId = (uint32_t) parseInt (value, -1);

    if (const char* value = spa_dict_lookup (props, PW_KEY_CLIENT_ID))
        rec.clientId = (uint32_t) parseInt (value, -1);

    if (const char* value = spa_dict_lookup (props, "card.profile.device"))
        rec.cardProfileDevice = parseInt (value, -1);

    if (const char* value = spa_dict_lookup (props, PW_KEY_PRIORITY_SESSION))
        rec.prioritySession = parseInt (value, 0);

    if (const char* value = spa_dict_lookup (props, PW_KEY_NODE_DONT_RECONNECT))
        rec.dontReconnect = std::strcmp (value, "true") == 0 || std::strcmp (value, "1") == 0;
}

void PwRegistry::addNode (uint32_t id, const spa_dict* props)
{
    NodeRec rec;
    rec.id = id;
    mergeNodeProps (rec, props);
    nodeRecs[id] = rec;

    // Streams and audio devices only reveal application.process.binary, media.name,
    // card.profile.device ... in their bound info, so bind those. Our own nodes need nothing.
    const bool wanted = ! rec.isOurs() && (rec.isStreamOutput() || startsWith (rec.mediaClass, "Audio/"));

    if (! wanted || registry == nullptr)
        return;

    void* raw = pw_registry_bind (registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);

    if (raw == nullptr)
        return;

    auto entry = std::make_unique<ProxyEntry> (this, id);
    entry->proxy.adopt (raw);

    ProxyEntry* raw_entry = entry.get();
    entry->proxy.addObjectListener ([raw_entry] (spa_hook* hook)
    {
        static const pw_node_events events = []
        {
            pw_node_events e {};
            e.version = PW_VERSION_NODE_EVENTS;
            e.info = &PwRegistry::onNodeInfoCb;
            return e;
        }();

        return pw_node_add_listener (reinterpret_cast<pw_node*> (raw_entry->proxy.get()), hook, &events, raw_entry);
    });

    nodeProxies[id] = std::move (entry);
}

void PwRegistry::onNodeInfoCb (void* data, const pw_node_info* info)
{
    auto* entry = static_cast<ProxyEntry*> (data);
    entry->owner->handleNodeInfo (entry->id, info);
}

void PwRegistry::handleNodeInfo (uint32_t id, const pw_node_info* info)
{
    auto found = nodeRecs.find (id);

    if (found == nodeRecs.end() || info == nullptr)
        return;

    auto& rec = found->second;

    if ((info->change_mask & PW_NODE_CHANGE_MASK_PROPS) != 0)
        mergeNodeProps (rec, info->props);

    if ((info->change_mask & PW_NODE_CHANGE_MASK_STATE) != 0)
        rec.state = info->state;

    rec.infoReceived = true;
    notifyChanged();
}

// -------------------------------------------------------------------------------------------------
// Ports and links

void PwRegistry::addPort (uint32_t id, const spa_dict* props)
{
    PortRec rec;
    rec.id = id;
    rec.nodeId = (uint32_t) parseInt (dictGet (props, PW_KEY_NODE_ID), -1);
    rec.name = dictGet (props, PW_KEY_PORT_NAME);
    rec.channel = dictGet (props, PW_KEY_AUDIO_CHANNEL);
    rec.isInput = dictGet (props, PW_KEY_PORT_DIRECTION) == "in";

    const std::string monitor = dictGet (props, PW_KEY_PORT_MONITOR);
    rec.isMonitor = monitor == "true" || monitor == "1";

    portRecs[id] = rec;
}

void PwRegistry::addLink (uint32_t id, const spa_dict* props)
{
    LinkRec rec;
    rec.id = id;
    rec.outNode = (uint32_t) parseInt (dictGet (props, PW_KEY_LINK_OUTPUT_NODE), -1);
    rec.outPort = (uint32_t) parseInt (dictGet (props, PW_KEY_LINK_OUTPUT_PORT), -1);
    rec.inNode = (uint32_t) parseInt (dictGet (props, PW_KEY_LINK_INPUT_NODE), -1);
    rec.inPort = (uint32_t) parseInt (dictGet (props, PW_KEY_LINK_INPUT_PORT), -1);
    linkRecs[id] = rec;
}

// -------------------------------------------------------------------------------------------------
// Devices

void PwRegistry::mergeDeviceProps (DeviceRec& rec, const spa_dict* props)
{
    if (props == nullptr)
        return;

    assignIfPresent (rec.name, props, PW_KEY_DEVICE_NAME);
    assignIfPresent (rec.description, props, PW_KEY_DEVICE_DESCRIPTION);
    assignIfPresent (rec.nick, props, PW_KEY_DEVICE_NICK);
    assignIfPresent (rec.api, props, PW_KEY_DEVICE_API);
    assignIfPresent (rec.mediaClass, props, PW_KEY_MEDIA_CLASS);

    if (const char* value = spa_dict_lookup (props, PW_KEY_OBJECT_SERIAL))
        rec.serial = parseU64 (value);

    if (const char* value = spa_dict_lookup (props, PW_KEY_DEVICE_VENDOR_ID))
        rec.vendorId = parseHexId (value);

    if (const char* value = spa_dict_lookup (props, PW_KEY_DEVICE_PRODUCT_ID))
        rec.productId = parseHexId (value);
}

void PwRegistry::addDevice (uint32_t id, const spa_dict* props)
{
    DeviceRec rec;
    rec.id = id;
    mergeDeviceProps (rec, props);
    deviceRecs[id] = rec;

    if (rec.mediaClass != "Audio/Device" || registry == nullptr)
        return;

    void* raw = pw_registry_bind (registry, id, PW_TYPE_INTERFACE_Device, PW_VERSION_DEVICE, 0);

    if (raw == nullptr)
        return;

    auto entry = std::make_unique<ProxyEntry> (this, id);
    entry->proxy.adopt (raw);

    ProxyEntry* raw_entry = entry.get();
    entry->proxy.addObjectListener ([raw_entry] (spa_hook* hook)
    {
        static const pw_device_events events = []
        {
            pw_device_events e {};
            e.version = PW_VERSION_DEVICE_EVENTS;
            e.info = &PwRegistry::onDeviceInfoCb;
            e.param = &PwRegistry::onDeviceParamCb;
            return e;
        }();

        return pw_device_add_listener (reinterpret_cast<pw_device*> (raw_entry->proxy.get()), hook, &events, raw_entry);
    });

    deviceProxies[id] = std::move (entry);
}

void PwRegistry::onDeviceInfoCb (void* data, const pw_device_info* info)
{
    auto* entry = static_cast<ProxyEntry*> (data);
    entry->owner->handleDeviceInfo (entry->id, info);
}

void PwRegistry::handleDeviceInfo (uint32_t id, const pw_device_info* info)
{
    auto found = deviceRecs.find (id);

    if (found == deviceRecs.end() || info == nullptr)
        return;

    if ((info->change_mask & PW_DEVICE_CHANGE_MASK_PROPS) != 0)
        mergeDeviceProps (found->second, info->props);

    // Re-read Profile/EnumProfile/Route ourselves whenever the device says its params changed, like pw-dump and
    // pipewire-pulse do. On the real Wave:3 the subscription never re-sent the active Profile after a switch, so
    // WaveGuard kept "fixing" a profile that was already fixed and the hardware-gain readback went stale.
    if ((info->change_mask & PW_DEVICE_CHANGE_MASK_PARAMS) != 0 && found->second.infoReceived)
    {
        auto proxy = deviceProxies.find (id);

        if (proxy != deviceProxies.end() && proxy->second->proxy.alive() && proxy->second->paramsSubscribed)
        {
            for (uint32_t i = 0; i < info->n_params; ++i)
            {
                const auto paramId = info->params[i].id;

                if ((paramId == SPA_PARAM_EnumProfile || paramId == SPA_PARAM_Profile || paramId == SPA_PARAM_Route)
                    && (info->params[i].flags & SPA_PARAM_INFO_READ) != 0)
                    pw_device_enum_params (reinterpret_cast<pw_device*> (proxy->second->proxy.get()), 0, paramId, 0,
                                           UINT32_MAX, nullptr);
            }
        }
    }

    found->second.infoReceived = true;
    notifyChanged();
}

void PwRegistry::onDeviceParamCb (void* data, int, uint32_t id, uint32_t index, uint32_t, const spa_pod* param)
{
    auto* entry = static_cast<ProxyEntry*> (data);

    if (param != nullptr && entry->owner->onDeviceParam)
        entry->owner->onDeviceParam (entry->id, id, index, param);
}

bool PwRegistry::subscribeDeviceParams (uint32_t deviceId)
{
    auto found = deviceProxies.find (deviceId);

    if (found == deviceProxies.end() || ! found->second->proxy.alive())
        return false;

    if (found->second->paramsSubscribed)
        return true;

    uint32_t ids[] = { SPA_PARAM_EnumProfile, SPA_PARAM_Profile, SPA_PARAM_Route };
    const int result = pw_device_subscribe_params (reinterpret_cast<pw_device*> (found->second->proxy.get()), ids, 3);
    found->second->paramsSubscribed = result >= 0;
    return found->second->paramsSubscribed;
}

bool PwRegistry::setDeviceParam (uint32_t deviceId, uint32_t paramId, const spa_pod* param)
{
    auto found = deviceProxies.find (deviceId);

    if (found == deviceProxies.end() || ! found->second->proxy.alive() || param == nullptr)
        return false;

    return pw_device_set_param (reinterpret_cast<pw_device*> (found->second->proxy.get()), paramId, 0, param) >= 0;
}

// -------------------------------------------------------------------------------------------------
// Metadata

void PwRegistry::addMetadata (uint32_t id, const spa_dict* props)
{
    if (dictGet (props, PW_KEY_METADATA_NAME) != "default" || metadata != nullptr || registry == nullptr)
        return;

    void* raw = pw_registry_bind (registry, id, PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0);

    if (raw == nullptr)
        return;

    metadata = std::make_unique<ProxyEntry> (this, id);
    metadata->proxy.adopt (raw);

    ProxyEntry* raw_entry = metadata.get();
    metadata->proxy.addObjectListener ([raw_entry] (spa_hook* hook)
    {
        static const pw_metadata_events events = []
        {
            pw_metadata_events e {};
            e.version = PW_VERSION_METADATA_EVENTS;
            e.property = &PwRegistry::onMetadataPropertyCb;
            return e;
        }();

        return pw_metadata_add_listener (reinterpret_cast<pw_metadata*> (raw_entry->proxy.get()), hook, &events, raw_entry);
    });

    pwLog ("bound the default metadata (id %u)", id);

    if (onMetadataBound)
        onMetadataBound();
}

int PwRegistry::onMetadataPropertyCb (void* data, uint32_t subject, const char* key, const char* type, const char* value)
{
    auto* entry = static_cast<ProxyEntry*> (data);
    entry->owner->handleMetadataProperty (subject, key, type, value);
    return 0;
}

void PwRegistry::handleMetadataProperty (uint32_t subject, const char* key, const char*, const char* value)
{
    if (key == nullptr)
    {
        // "clear": everything for this subject (or all of it) is gone.
        for (auto it = metadataValues.begin(); it != metadataValues.end();)
        {
            if (it->first.first == subject || subject == SPA_ID_INVALID)
                it = metadataValues.erase (it);
            else
                ++it;
        }
    }
    else if (value == nullptr)
    {
        const std::pair<uint32_t, std::string> mapKey (subject, std::string (key));
        metadataValues.erase (mapKey);
    }
    else
    {
        const std::pair<uint32_t, std::string> mapKey (subject, std::string (key));
        metadataValues[mapKey] = value;
    }

    notifyChanged();
}

std::string PwRegistry::metadataValue (uint32_t subject, const std::string& key) const
{
    const std::pair<uint32_t, std::string> mapKey (subject, key);
    const auto found = metadataValues.find (mapKey);
    return found != metadataValues.end() ? found->second : std::string();
}

bool PwRegistry::setMetadata (uint32_t subject, const char* key, const char* type, const char* value)
{
    if (! metadataReady())
        return false;

    return pw_metadata_set_property (reinterpret_cast<pw_metadata*> (metadata->proxy.get()),
                                     subject, key, type, value) >= 0;
}

// -------------------------------------------------------------------------------------------------
// Queries

const NodeRec* PwRegistry::node (uint32_t id) const
{
    const auto found = nodeRecs.find (id);
    return found != nodeRecs.end() ? &found->second : nullptr;
}

const NodeRec* PwRegistry::nodeByName (const std::string& name) const
{
    const NodeRec* best = nullptr;

    for (const auto& entry : nodeRecs)
        if (entry.second.name == name && (best == nullptr || entry.first > best->id))
            best = &entry.second;

    return best;
}

const DeviceRec* PwRegistry::device (uint32_t id) const
{
    const auto found = deviceRecs.find (id);
    return found != deviceRecs.end() ? &found->second : nullptr;
}

const DeviceRec* PwRegistry::findWaveDevice() const
{
    for (const auto& entry : deviceRecs)
        if (entry.second.mediaClass == "Audio/Device" && entry.second.identity().wave)
            return &entry.second;

    return nullptr;
}

elgato::Identity PwRegistry::identityOf (const NodeRec& candidate) const
{
    if (const auto* owner = device (candidate.deviceId))
        if (owner->vendorId >= 0 || owner->productId >= 0)
            return elgato::identify (owner->vendorId, owner->productId, candidate.name,
                                     owner->description + " " + owner->nick + " " + candidate.description + " " + candidate.nick);

    return elgato::identify (-1, -1, candidate.name, candidate.description + " " + candidate.nick);
}

bool PwRegistry::isWaveNode (const NodeRec& candidate) const
{
    return identityOf (candidate).wave;
}

bool PwRegistry::isWave3Node (const NodeRec& candidate) const
{
    return identityOf (candidate).isWave3();
}

std::vector<PortRec> PwRegistry::portsOf (uint32_t nodeId, bool input, PortKind kind) const
{
    std::vector<PortRec> result;

    for (const auto& entry : portRecs)
    {
        const auto& port = entry.second;

        if (port.nodeId != nodeId || port.isInput != input)
            continue;

        if ((kind == PortKind::MonitorOnly && ! port.isMonitor) || (kind == PortKind::NonMonitor && port.isMonitor))
            continue;

        result.push_back (port);
    }

    return result;   // std::map order = ascending port id
}

const PortRec* PwRegistry::findPortByName (uint32_t nodeId, bool input, const std::string& name) const
{
    for (const auto& entry : portRecs)
        if (entry.second.nodeId == nodeId && entry.second.isInput == input && entry.second.name == name)
            return &entry.second;

    return nullptr;
}

bool PwRegistry::hasLink (uint32_t outPort, uint32_t inPort) const
{
    for (const auto& entry : linkRecs)
        if (entry.second.outPort == outPort && entry.second.inPort == inPort)
            return true;

    return false;
}

bool PwRegistry::nodeHasLinks (uint32_t nodeId) const
{
    for (const auto& entry : linkRecs)
        if (entry.second.outNode == nodeId || entry.second.inNode == nodeId)
            return true;

    return false;
}

} // namespace hr::pw
