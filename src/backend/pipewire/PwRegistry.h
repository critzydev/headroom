// Mirror of the PipeWire graph (nodes, ports, links, devices, "default" metadata) kept on the loop thread.
#pragma once

#include "PwCommon.h"

#include "backend/ElgatoModels.h"

namespace hr::pw
{

struct NodeRec
{
    uint32_t id = SPA_ID_INVALID;
    uint64_t serial = 0;
    std::string name, description, nick, mediaClass;
    std::string appName, binary, iconName, mediaName;
    uint32_t deviceId = SPA_ID_INVALID;    // owning pw_device, if any (device.id)
    uint32_t clientId = SPA_ID_INVALID;    // the client that created the node (client.id)
    int cardProfileDevice = -1;            // which ALSA device of the card this node is
    int prioritySession = 0;
    pw_node_state state = PW_NODE_STATE_CREATING;
    bool infoReceived = false;             // the bound node info (full props) has arrived
    bool dontReconnect = false;

    bool isStreamOutput() const { return mediaClass == "Stream/Output/Audio"; }
    bool isSink() const { return mediaClass == "Audio/Sink" || mediaClass == "Audio/Sink/Virtual"; }
    bool isSource() const { return mediaClass == "Audio/Source" || mediaClass == "Audio/Source/Virtual"; }
    bool isOurs() const { return startsWith (name, kNodePrefix); }
};

struct PortRec
{
    uint32_t id = SPA_ID_INVALID;
    uint32_t nodeId = SPA_ID_INVALID;
    std::string name;
    std::string channel;          // audio.channel: "FL", "FR", "MONO", ...
    bool isInput = false;
    bool isMonitor = false;
};

struct LinkRec
{
    uint32_t id = SPA_ID_INVALID;
    uint32_t outNode = SPA_ID_INVALID, outPort = SPA_ID_INVALID;
    uint32_t inNode = SPA_ID_INVALID, inPort = SPA_ID_INVALID;
};

struct DeviceRec
{
    uint32_t id = SPA_ID_INVALID;
    uint64_t serial = 0;
    std::string name, description, nick, api, mediaClass;
    int vendorId = -1;
    int productId = -1;
    bool infoReceived = false;

    /** Who this device is: Elgato (and which model) or not, from its USB ids, else from its card name. */
    elgato::Identity identity() const
    {
        return elgato::identify (vendorId, productId, name, description + " " + nick);
    }

    bool isElgato() const noexcept { return identity().elgato; }
};

/**
    Keeps plain-data mirrors of everything Headroom needs to know and binds the objects that
    only reveal their full properties (or params) after a bind: nodes, audio devices and the
    "default" metadata. All hooks and proxies live here, so nothing else has to remember to
    remove a listener.
*/
class PwRegistry
{
public:
    PwRegistry();
    ~PwRegistry();

    PwRegistry (const PwRegistry&) = delete;
    PwRegistry& operator= (const PwRegistry&) = delete;

    /** Start mirroring. Call with the loop lock held / on the loop thread. */
    void attach (pw_core* core);

    /** Destroys every bound proxy and clears the mirror. */
    void detach();

    bool isAttached() const noexcept { return registry != nullptr; }

    // ---- change notification (loop thread) --------------------------------------------------
    std::function<void()> onChanged;
    std::function<void (uint32_t deviceId, uint32_t paramId, uint32_t index, const spa_pod* param)> onDeviceParam;
    std::function<void (uint32_t deviceId)> onDeviceRemoved;
    std::function<void()> onMetadataBound;

    // ---- read access -------------------------------------------------------------------------
    const std::map<uint32_t, NodeRec>& nodes() const noexcept { return nodeRecs; }
    const std::map<uint32_t, PortRec>& ports() const noexcept { return portRecs; }
    const std::map<uint32_t, LinkRec>& links() const noexcept { return linkRecs; }
    const std::map<uint32_t, DeviceRec>& devices() const noexcept { return deviceRecs; }

    const NodeRec* node (uint32_t id) const;
    /** With duplicate names (a node being recreated) the newest one wins. */
    const NodeRec* nodeByName (const std::string& name) const;
    const DeviceRec* device (uint32_t id) const;
    /** The first Elgato Wave (any model) among the audio devices, or nullptr. */
    const DeviceRec* findWaveDevice() const;
    /** Who a node is: its owner's USB ids when they are known, else its node name (ElgatoModels.h). */
    elgato::Identity identityOf (const NodeRec& node) const;
    /** An Elgato Wave microphone or interface of any model (or one we do not know yet). */
    bool isWaveNode (const NodeRec& node) const;
    /** The original Wave:3 (USB 0fd9:0070): the device's ids when known, else the node name. */
    bool isWave3Node (const NodeRec& node) const;

    /** Which ports of a node: the null-audio-sink based virtual sources flag their capture ports
        as monitor ports, real ALSA sources don't, so callers that want "the capture ports" use Any. */
    enum class PortKind { Any, MonitorOnly, NonMonitor };
    std::vector<PortRec> portsOf (uint32_t nodeId, bool input, PortKind kind) const;
    const PortRec* findPortByName (uint32_t nodeId, bool input, const std::string& name) const;
    bool hasLink (uint32_t outPort, uint32_t inPort) const;
    bool nodeHasLinks (uint32_t nodeId) const;

    // ---- device params -----------------------------------------------------------------------
    /** Subscribes to EnumProfile / Profile / Route of a device (replays current values). */
    bool subscribeDeviceParams (uint32_t deviceId);
    bool setDeviceParam (uint32_t deviceId, uint32_t paramId, const spa_pod* param);

    // ---- "default" metadata ------------------------------------------------------------------
    bool metadataReady() const noexcept { return metadata != nullptr && metadata->proxy.alive(); }
    std::string metadataValue (uint32_t subject, const std::string& key) const;
    bool setMetadata (uint32_t subject, const char* key, const char* type, const char* value);

private:
    struct ProxyEntry
    {
        ProxyEntry (PwRegistry* ownerToUse, uint32_t idToUse) : owner (ownerToUse), id (idToUse) {}
        PwRegistry* owner;
        uint32_t id;
        ProxyHolder proxy;
        bool paramsSubscribed = false;
    };

    void handleGlobal (uint32_t id, const char* type, const spa_dict* props);
    void handleGlobalRemove (uint32_t id);
    void addNode (uint32_t id, const spa_dict* props);
    void addPort (uint32_t id, const spa_dict* props);
    void addLink (uint32_t id, const spa_dict* props);
    void addDevice (uint32_t id, const spa_dict* props);
    void addMetadata (uint32_t id, const spa_dict* props);

    static void mergeNodeProps (NodeRec& rec, const spa_dict* props);
    static void mergeDeviceProps (DeviceRec& rec, const spa_dict* props);

    void handleNodeInfo (uint32_t id, const pw_node_info* info);
    void handleDeviceInfo (uint32_t id, const pw_device_info* info);
    void handleMetadataProperty (uint32_t subject, const char* key, const char* type, const char* value);
    void notifyChanged();

    static void onGlobalCb (void* data, uint32_t id, uint32_t permissions, const char* type,
                            uint32_t version, const spa_dict* props);
    static void onGlobalRemoveCb (void* data, uint32_t id);
    static void onNodeInfoCb (void* data, const pw_node_info* info);
    static void onDeviceInfoCb (void* data, const pw_device_info* info);
    static void onDeviceParamCb (void* data, int seq, uint32_t id, uint32_t index, uint32_t next,
                                 const spa_pod* param);
    static int onMetadataPropertyCb (void* data, uint32_t subject, const char* key, const char* type,
                                     const char* value);

    pw_core* coreRef = nullptr;
    pw_registry* registry = nullptr;
    spa_hook registryHook {};
    bool registryHookActive = false;

    std::map<uint32_t, NodeRec> nodeRecs;
    std::map<uint32_t, PortRec> portRecs;
    std::map<uint32_t, LinkRec> linkRecs;
    std::map<uint32_t, DeviceRec> deviceRecs;

    std::map<uint32_t, std::unique_ptr<ProxyEntry>> nodeProxies;
    std::map<uint32_t, std::unique_ptr<ProxyEntry>> deviceProxies;
    std::unique_ptr<ProxyEntry> metadata;

    std::map<std::pair<uint32_t, std::string>, std::string> metadataValues;
};

} // namespace hr::pw
