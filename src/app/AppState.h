// AppState: the ValueTree model, its first-run defaults, typed helpers and debounced persistence.
#pragma once

#include "Juce.h"
#include "backend/BackendTypes.h"
#include "engine/EngineTypes.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace hr
{
// Identifiers for every type / property in the schema. Use these, never string literals.
namespace ids
{
#define HR_ID(name) inline const juce::Identifier name { #name }
    // Node types
    HR_ID (HEADROOM);  HR_ID (SETTINGS);  HR_ID (MIXES);  HR_ID (MIX);  HR_ID (CHANNELS);  HR_ID (CHANNEL);
    HR_ID (FX);  HR_ID (SLOT);  HR_ID (ROUTING);  HR_ID (RULE);

    // HEADROOM
    HR_ID (version);

    // SETTINGS
    HR_ID (inputDevice);  HR_ID (outputDevice);  HR_ID (latency);  HR_ID (makeDefault);  HR_ID (keepDefault);  HR_ID (startAtLogin);
    HR_ID (uiScale);  HR_ID (restoreMicGain);  HR_ID (restoreMicGainChosen);  HR_ID (micGain);  HR_ID (headphoneVolume);  HR_ID (firstRunDone);
    HR_ID (fallbackChannel);  HR_ID (pluginFolders);  HR_ID (micPreset);
    HR_ID (clipGuard);  HR_ID (restoreClipGuard);  HR_ID (safeGain);  HR_ID (safeGainBase);      // Wave:3 hardware
    HR_ID (experimentalHardware);                                                                // opt-in: USB controls of models Headroom could not try

    // MIX (also uses id, outputDevice, which stays as the first of outputs for a Headroom that predates it)
    HR_ID (id);  HR_ID (gainDb);  HR_ID (muted);  HR_ID (limiter);
    HR_ID (outputs);        // "name1|name2": every device the mix plays to at once

    // CHANNEL
    HR_ID (kind);  HR_ID (name);  HR_ID (colour);  HR_ID (icon);  HR_ID (slot);
    HR_ID (device);  HR_ID (channels);      // input channels only: the capture device's node.name, and how much of it is taken (mono | stereo)
    HR_ID (monitorDb);  HR_ID (monitorMuted);  HR_ID (streamDb);  HR_ID (streamMuted);  HR_ID (linked);

    // SLOT
    HR_ID (uid);  HR_ID (crashId);  HR_ID (type);  HR_ID (builtinId);  HR_ID (desc);
    HR_ID (bypassed);  HR_ID (disabled);  HR_ID (state);

    // RULE
    HR_ID (pattern);  HR_ID (channel);
#undef HR_ID
}

constexpr int kStateVersion = 1;

// Values of CHANNEL kind, SLOT type and MIX id.
namespace values
{
    inline const juce::String kindMic { "mic" }, kindApp { "app" }, kindInput { "input" };
    inline const juce::String inputMono { "mono" }, inputStereo { "stereo" };      // CHANNEL channels (input channels)
    inline const juce::String outputAuto { "auto" };                              // the device the backend prefers (an Elgato Wave first)
    inline const juce::String slotBuiltin { "builtin" }, slotPlugin { "plugin" };
    inline const juce::String mixMonitor { "monitor" }, mixStream { "stream" };
    inline const juce::String micChannelId { "mic" };
}

// The eight channel-tag colours as RRGGBB hex, in pick order.
const std::vector<juce::String>& getChannelColourPalette();

// Read-only snapshots for UI code that doesn't want to poke at ValueTree properties.
struct ChannelInfo
{
    juce::String id, kind, name, colour, icon;
    juce::String device;                        // input channels: the capture device (node.name), "" for the others
    int slot = -1;                              // app channels 0-15, input channels 0-7 (separate numbering), the mic -1
    float monitorDb = 0.0f, streamDb = 0.0f;
    bool monitorMuted = false, streamMuted = false, linked = false;
    bool stereo = false;                        // input channels: take the device's left and right (else its first channel, on both sides)

    bool isMic() const noexcept                 { return kind == values::kindMic; }
    bool isInput() const noexcept               { return kind == values::kindInput; }
    bool isApp() const noexcept                 { return ! isMic() && ! isInput(); }        // anything else is an app channel, as before
    float getDb (Mix m) const noexcept          { return m == Mix::Monitor ? monitorDb : streamDb; }
    bool isMuted (Mix m) const noexcept         { return m == Mix::Monitor ? monitorMuted : streamMuted; }
};

struct SlotInfo
{
    juce::String uid, type, builtinId, name, descXml, stateBase64;
    uint32_t crashId = 0;
    bool bypassed = false, disabled = false;

    bool isBuiltin() const noexcept             { return type == values::slotBuiltin; }
};

class AppState : private juce::ValueTree::Listener, private juce::Timer
{
public:
    enum class LoadResult
    {
        Loaded,                 // read state.xml fine
        CreatedDefaults,        // no file yet: first run
        RecoveredFromCorrupt,   // unreadable / wrong root: defaults used, old file kept as state.xml.bak
        UnsupportedVersion      // written by a newer Headroom: defaults used, old file kept as state.xml.bak
    };

    // stateFile may be juce::File() for a purely in-memory model (nothing is ever written).
    // The constructor already populates first-run defaults; load() replaces them with the saved model.
    explicit AppState (const juce::File& stateFile = {});
    ~AppState() override;

    // ---- Persistence (message thread) -------------------------------------------------------------
    LoadResult load();
    bool saveNow();                                     // atomic write; false on failure
    void setBeforeSave (std::function<void()> callback);// AppController captures plugin states here
    bool hasUnsavedChanges() const noexcept             { return dirty; }
    const juce::File& getFile() const noexcept          { return file; }

    // ---- Tree access. The root object is stable for the lifetime of the AppState.
    // Listen with addListener()/removeListener() below, NOT getRoot().addListener(): a ValueTree listener lives on the
    // ValueTree handle it was added to and silently disappears when that (temporary) handle is destroyed.
    // Listeners are told about changes anywhere in the tree.
    void addListener (juce::ValueTree::Listener* listener)       { root.addListener (listener); }
    void removeListener (juce::ValueTree::Listener* listener)    { root.removeListener (listener); }

    juce::ValueTree getRoot() const noexcept            { return root; }
    juce::ValueTree getSettings() const                 { return root.getChildWithName (ids::SETTINGS); }
    juce::ValueTree getMixes() const                    { return root.getChildWithName (ids::MIXES); }
    juce::ValueTree getChannels() const                 { return root.getChildWithName (ids::CHANNELS); }
    juce::ValueTree getRouting() const                  { return root.getChildWithName (ids::ROUTING); }

    juce::ValueTree getMix (Mix mix) const;
    juce::ValueTree findChannel (const juce::String& channelId) const;     // invalid ValueTree if unknown
    juce::ValueTree getMicChannel() const               { return findChannel (values::micChannelId); }
    juce::ValueTree findSlot (const juce::String& slotUid) const;          // FX slot in any channel
    static juce::ValueTree getFx (const juce::ValueTree& channel);         // its FX child (invalid if none)
    static juce::String getChannelIdOfSlot (const juce::ValueTree& slotTree);

    static ChannelInfo describe (const juce::ValueTree& channel);
    static SlotInfo describeSlot (const juce::ValueTree& slotTree);
    std::vector<ChannelInfo> getChannelInfos() const;                      // mic first, then app channels in model order
    std::vector<ChannelEndpoint> getChannelEndpoints() const;              // app channels only
    std::vector<InputEndpoint> getInputEndpoints() const;                  // input channels only
    std::vector<RoutingRule> getRoutingRules() const;

    // ---- Settings ---------------------------------------------------------------------------------
    juce::var getSetting (const juce::Identifier& property) const;
    void setSetting (const juce::Identifier& property, const juce::var& value);

    juce::String getInputDevice() const                 { return getSetting (ids::inputDevice).toString(); }
    // The first device the Headphones mix plays to ("auto" when it is the automatic choice): what a single-output screen shows.
    juce::String getOutputDevice() const                { return getOutputs (Mix::Monitor)[0]; }
    int getLatency() const                              { return (int) getSetting (ids::latency); }
    bool getMakeDefault() const                         { return (int) getSetting (ids::makeDefault) != 0; }
    bool getKeepDefault() const                         { return (int) getSetting (ids::keepDefault) != 0; }    // only matters while makeDefault is on
    juce::String getFallbackChannel() const             { return getSetting (ids::fallbackChannel).toString(); }
    juce::StringArray getPluginFolders() const;

    // ---- Outputs -----------------------------------------------------------------------
    // Every device a mix plays to at once, as node.names. The Headphones list is never empty ("auto" is the default and what an
    // empty list becomes); the Stream list is the extra devices only (empty = "Headroom Stream Mix" alone) and has no "auto".
    // The first of the Headphones list is mirrored to the old `outputDevice` (SETTINGS and MIX) so the file still says something
    // sensible to a Headroom that predates the list.
    juce::StringArray getOutputs (Mix mix) const;
    void setOutputs (Mix mix, const juce::StringArray& devices);
    static juce::StringArray splitOutputs (const juce::String& joined);     // "a|b" -> { a, b }: trimmed, no empties, no repeats
    static juce::String joinOutputs (const juce::StringArray& devices);

    // ---- Channels ---------------------------------------------------------------------------------
    // Adds an app channel in the next free slot (0-15) with the next unused colour. An empty name becomes
    // "Channel N". Returns an invalid ValueTree if all 16 slots are taken.
    juce::ValueTree addAppChannel (const juce::String& name = {});
    // Adds an input channel for a capture device (a second mic, a capture card, a line-in): next free input slot (0-7), next
    // unused colour, no effects, 0 dB in both mixes. Returns an invalid ValueTree if all 8 slots are taken or there is no device.
    juce::ValueTree addInputChannel (const juce::String& deviceId, const juce::String& name, bool stereo);
    // The mic cannot be removed. Also drops the channel's routing rules and repoints the fallback channel.
    bool removeChannel (const juce::String& channelId);
    int nextFreeSlot() const;                           // app slot; -1 if none
    int nextFreeInputSlot() const;                      // input slot; -1 if none
    juce::String nextFreeColour() const;                // RRGGBB

    // ---- Effects ----------------------------------------------------------------------------------
    // index < 0 appends. Returns the new SLOT (invalid if the channel doesn't exist).
    juce::ValueTree addBuiltinSlot (const juce::String& channelId, const juce::String& builtinId, int index = -1);
    juce::ValueTree addPluginSlot (const juce::String& channelId, const juce::PluginDescription& description, int index = -1);
    bool removeSlot (const juce::String& slotUid);
    bool moveSlot (const juce::String& slotUid, int newIndex);
    // Stores captured plugin state. markDirty=false is for the pre-save capture so it can't retrigger a save.
    void setSlotState (const juce::String& slotUid, const juce::String& base64, bool markDirty = true);
    uint32_t nextCrashId() noexcept                     { return ++crashIdCounter; }

    // ---- Routing rules ----------------------------------------------------------------------------
    // Upsert by (lower-cased) pattern.
    void setRule (const juce::String& pattern, const juce::String& channelId);
    void removeRule (const juce::String& pattern);

    static juce::String mixId (Mix mix);

private:
    juce::ValueTree createDefaults();
    void repair (juce::ValueTree& tree);
    juce::ValueTree makeChannel (const juce::String& id, const juce::String& kindValue, const juce::String& channelName,
                                 const juce::String& colourHex, const juce::String& iconName, int slotIndex,
                                 float monitorDbValue, bool monitorMutedValue);
    juce::ValueTree makeBuiltinSlot (const juce::String& builtinId);
    juce::String makeUid() const;
    juce::String makeChannelId (const juce::String& name) const;
    void refreshCrashCounter();
    void markDirty();

    // juce::ValueTree::Listener: any change schedules an autosave.
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override  { markDirty(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override               { markDirty(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override        { markDirty(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override                { markDirty(); }

    void timerCallback() override;

    const juce::File file;
    juce::ValueTree root { ids::HEADROOM };
    std::function<void()> beforeSave;
    uint32_t crashIdCounter = 0;
    bool dirty = false;
    bool suppressDirty = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppState)
};
}
