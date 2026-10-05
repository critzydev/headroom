// AppController: owns the model, engine, backend and plugin manager, and keeps them in sync.
#pragma once

#include "Juce.h"
#include "app/AppState.h"
#include "backend/AudioBackend.h"
#include "engine/AudioEngine.h"
#include "engine/FxChain.h"
#include "plugins/PluginManager.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace hr
{
// A dismissable message for the status banner (plugin crashed, scan finished, ...). Backend mic/device problems
// come from BackendStatus instead; the UI shows those first.
struct Notice
{
    enum class Severity { info, warning, error };

    juce::String id;                        // stable, e.g. "crash:<slotUid>"; posting the same id replaces it
    Severity severity = Severity::info;
    juce::String text;                      // one plain sentence
    juce::String actionLabel;               // e.g. "Turn back on"; empty = no button
    std::function<void()> action;           // runs on the message thread, then the notice is dismissed
};

// All methods are for the message thread. UI code changes things through the methods below (or straight on
// the ValueTree: the controller listens to the model and applies whatever changed).
class AppController : private juce::ValueTree::Listener, private juce::Timer
{
public:
    struct Options
    {
        bool demo = false;              // NullBackend even on Linux (--demo)
        bool demoMicMuted = false;      // NullBackend reports MutedOnDevice (--demo-mic-muted); implies demo
        juce::File stateFile;           // empty = Paths::getStateFile()
        bool persist = true;            // false: model lives in memory only, plugin list isn't cached (screenshots, tests)
        bool startBackend = true;       // false: everything is built but start() doesn't start audio (tests)
        int safeGainRetryMs = 1500;     // how long Safe gain waits for the mic to answer before it asks for the lower gain again
        bool holdDefaults = false;      // the setup wizard is open: do not make Headroom the default device until it is done (setHoldDefaults)
        int safeGainPermissionGraceMs = 5000;   // how long the USB permission may be missing (a replugged mic gets its access a moment
                                                // after it appears) before Safe gain switches itself off
    };

    explicit AppController (const Options& options);
    ~AppController() override;

    // Pushes the whole model into the engine and backend, then starts the backend.
    void start();

    AppState& getState() noexcept               { return state; }
    AudioEngine& getEngine() noexcept           { return *engine; }
    AudioBackend& getBackend() noexcept         { return *backend; }
    PluginManager& getPlugins() noexcept        { return plugins; }
    BackendStatus getBackendStatus() const      { return backend->getStatus(); }

    // Called (message thread) when the backend's device/stream/status lists changed.
    std::function<void()> onBackendChanged;

    // Engine channel index for a model channel: its slot for an app channel, kMicChannel for the mic, kInputChannel0 + its slot
    // for an input channel, -1 if unknown.
    int engineIndexFor (const juce::String& channelId) const;

    // ---- Channels ---------------------------------------------------------------------------------
    // db <= -100 means -inf. If the channel is linked, the other mix follows.
    void setChannelLevel (const juce::String& channelId, Mix mix, float db);
    void setChannelMuted (const juce::String& channelId, Mix mix, bool muted);
    void toggleMute (const juce::String& channelId, Mix mix);
    void setChannelLinked (const juce::String& channelId, bool linked);
    void renameChannel (const juce::String& channelId, const juce::String& newName);
    void setChannelColour (const juce::String& channelId, const juce::String& rrggbb);
    void setChannelIcon (const juce::String& channelId, const juce::String& iconName);
    juce::String addChannel();                                  // an app channel; returns its id, "" if all 16 slots are used
    // An input channel for a capture device (a second mic, a capture card, a line-in), named `name` (the backend's label for the
    // device when empty), stereo when the device has a left and a right. Returns its id, "" if all 8 slots are used.
    juce::String addInputChannel (const juce::String& deviceId, const juce::String& name = {});
    void setChannelDevice (const juce::String& channelId, const juce::String& deviceId);    // "Change input..."
    void setChannelStereo (const juce::String& channelId, bool stereo);                     // input channels: left and right, or the first channel on both sides
    void removeChannel (const juce::String& channelId);         // not the mic

    // ---- Master strips ----------------------------------------------------------------------------
    void setMasterLevel (Mix mix, float db);
    void setMasterMuted (Mix mix, bool muted);
    void setLimiterEnabled (Mix mix, bool enabled);             // "Protect my ears"

    // ---- Devices / settings -----------------------------------------------------------------------
    void setInputDevice (const juce::String& deviceId);         // "auto" = prefer the Wave
    void setOutputDevice (const juce::String& deviceId);        // the Headphones mix plays to this one device ("auto" = prefer the Wave)
    // Everything a mix plays to at once. Headphones keeps at least one ("auto" when the list is empty); the Stream
    // mix's list names the extra devices, "Headroom Stream Mix" always gets the mix.
    void setMixOutputs (Mix mix, const juce::StringArray& deviceIds);
    // One tick in the "Play through" list. "auto" is a choice of its own: ticking it makes it the only one; ticking a device
    // while Automatic is on keeps what Automatic plays through now and adds the device. The last device of the Headphones mix
    // cannot be unticked.
    void setOutputSelected (Mix mix, const juce::String& deviceId, bool selected);
    void setLatency (int frames);                               // 128 / 256 / 512
    void setMakeDefault (bool makeDefault);
    void setKeepDefault (bool keepDefault);                     // only has an effect while makeDefault is on
    void setHoldDefaults (bool hold);                           // the setup wizard is open (true) or done (false): see Options::holdDefaults
    void setFallbackChannel (const juce::String& channelId);
    void setHardwareMicGain (float linear0to1);                 // also stored in settings
    // The user moved the "Mic gain" slider: as setHardwareMicGain, and the first time (only while "Restore mic gain" was
    // never decided, and never while Safe gain is on) it switches "Restore mic gain" on: someone who sets the gain wants it
    // to be there after a reboot. Programmatic changes use setHardwareMicGain and leave that setting alone.
    void userSetHardwareMicGain (float linear0to1);
    void setHardwareHeadphoneVolume (float linear0to1);
    void fixMic();                                              // "Fix my mic" -> backend->runMicRecovery()

    // ---- Wave:3 hardware controls ------------------------------------------------------
    HardwareControls getHardwareControls() const                { return backend->getHardwareControls(); }
    void setClipGuard (bool on);                                // remembered, and set on the mic as soon as it can be
    void setRestoreClipGuard (bool restore);                    // put ClipGuard back at start-up
    void setRestoreMicGain (bool restore);                      // put the mic's own gain back when it appears (start-up, replug)
    void requestHardwarePermission();                           // "Allow hardware controls": the password box, once

    // Experimental: USB controls for a model whose protocol is documented publicly but that Headroom has not been
    // able to try. Off until the user switches it on; Settings shows the row only while such a model is the input.
    void setExperimentalHardware (bool on);
    bool getExperimentalHardware() const;

    // Safe gain: the mic's own gain is held at a safe level (+24 dB of its 0 ... +40 dB) and Headroom adds the
    // difference back on the mic channel, so the level stays what it was. It needs the USB permission ("Allow hardware
    // controls"): the numbers, and the makeup, follow only what the mic itself reports over USB, so a mic that ignored the
    // request (gain lock) never gets makeup on top of its high gain. If the permission goes away, Safe gain switches itself
    // off (makeup 0, the mic's own gain left as it is) and says so once as a notice.
    struct SafeGainInfo
    {
        bool available = false;         // the selected input is a Wave:3 (the maths is about its 0 ... 40 dB range)
        bool on = false;
        bool hardwareKnown = false;     // the mic's gain has been read
        float hardwareDb = 0.0f;        // the mic's own gain now
        float makeupDb = 0.0f;          // what Headroom adds on top (mic channel input trim)
        juce::String problem;           // plain words when the mic did not take the lower gain, else empty
    };

    void setSafeGain (bool on);
    SafeGainInfo getSafeGainInfo() const                        { return safeGainInfo; }

    // ---- App routing ------------------------------------------------------------------------------
    // Moves a live stream now and remembers the choice as a rule for that app's binary (or name).
    void routeApp (uint32_t nodeId, const juce::String& channelId);
    void removeRule (const juce::String& pattern);

    // ---- Effects ----------------------------------------------------------------------------------
    // The add methods return the new slot's uid (empty on failure). index < 0 appends.
    juce::String addBuiltinFx (const juce::String& channelId, const juce::String& builtinId, int index = -1);
    juce::String addPluginFx (const juce::String& channelId, const juce::PluginDescription& description, int index = -1);
    void removeFx (const juce::String& channelId, const juce::String& slotUid);
    void moveFx (const juce::String& channelId, const juce::String& slotUid, int newIndex);
    void setFxBypassed (const juce::String& channelId, const juce::String& slotUid, bool bypassed);
    void openFxEditor (const juce::String& channelId, const juce::String& slotUid);
    void reenableFx (const juce::String& slotUid);              // after a crash disabled it

    // The live processor of a slot (for the inline built-in panels: dynamic_cast to BuiltinProcessor). nullptr if
    // the slot is disabled or failed to load; getFxError() then says why for plugin slots.
    juce::AudioProcessor* getFxProcessor (const juce::String& slotUid) const;

    // Same processor, shared. UI panels that point into a processor (knob attachments, the EQ curve) hold this so the
    // processor cannot be destroyed under them when the engine lets go of a chain (AudioEngine::setChain frees retired
    // snapshots at once when the audio thread is idle). nullptr if the slot has no live processor.
    std::shared_ptr<juce::AudioProcessor> getFxProcessorShared (const juce::String& slotUid) const;
    juce::String getFxError (const juce::String& slotUid) const;

    // Rebuilds the engine's FX chain snapshot for a channel from the model. Called automatically whenever the
    // slot list, order or disabled flags change. Live processors are reused by uid, so their state survives.
    void rebuildChain (const juce::String& channelId);

    // Copies the state of every live processor into the model (also runs before each save and every 5 s).
    void captureFxStates (bool markDirty);

    // Replaces the mic's effects with a preset from engine/builtin/Presets.h ("clean", "broadcast", "noisy", "off").
    // The applied preset's id is kept in SETTINGS micPreset; adding, removing or reordering mic effects afterwards
    // makes it "custom" (the Preset menu then says so).
    void applyMicPreset (const juce::String& presetId);

    // "Hear what's removed" (Noise Removal panel, momentary). While on, the mic's Headphones level is 0 dB and unmuted, and
    // the mic is kept out of the Stream mix and away from apps (Discord): only the engine is told, the model (and so the
    // saved state) never changes, and turning it off applies the model again. Ends by itself after 30 s in case the
    // release was missed. The Noise Removal effect's own switch follows.
    void setHearingRemoved (bool on);
    bool isHearingRemoved() const noexcept                      { return hearingRemoved; }

    // ---- Notices (status banner) ------------------------------------------------------------------
    const std::vector<Notice>& getNotices() const noexcept      { return notices; }
    void postNotice (Notice notice);                            // replaces a notice with the same id
    void dismissNotice (const juce::String& noticeId);
    void runNoticeAction (const juce::String& noticeId);        // runs its action, then dismisses it
    std::function<void()> onNoticesChanged;

private:
    struct LiveSlot
    {
        juce::String channelId;
        FxSlotPtr slot;
    };

    // ValueTree::Listener
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override;
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree& parent, int, int) override;

    void timerCallback() override;

    void markMicPresetCustom (const juce::ValueTree& fxNode);
    void channelPropertyChanged (const juce::ValueTree& channel, const juce::Identifier& property);
    void slotPropertyChanged (const juce::ValueTree& slotTree, const juce::Identifier& property);
    void settingChanged (const juce::Identifier& property);

    void applyChannelToEngine (const juce::ValueTree& channel);
    void applyMixToEngine (const juce::ValueTree& mix);
    void deactivateChannel (const juce::ValueTree& channel);
    void pushChannelsToBackend();
    void pushDevicesToBackend();
    void pushRoutingRules();
    void pushClipGuard();
    void safeGainSwitched (bool on);
    void updateSafeGain();
    void applyMakeup (float db);
    void maybeRestoreMicGain();
    FxSlotPtr createSlot (const SlotInfo& info, juce::String& error);

    const Options options;
    std::unique_ptr<AudioEngine> engine;        // ~200 KB of buffers: keep off the stack
    PluginManager plugins;
    AppState state;
    std::map<juce::String, LiveSlot> liveSlots;             // by slot uid
    std::map<juce::String, juce::String> slotErrors;        // by slot uid
    std::vector<Notice> notices;
    std::unique_ptr<AudioBackend> backend;                  // destroyed first: stops the RT thread before the rest goes
    bool applyingPreset = false;
    bool hearingRemoved = false;
    double hearingRemovedSince = 0.0;
    int tickCount = 0;
    bool started = false;
    bool holdingDefaults = false;                           // the setup wizard is open: Headroom is not made the default device yet

    // Safe gain (message thread).
    SafeGainInfo safeGainInfo;
    float appliedMakeupDb = 0.0f;                           // what the engine's mic trim is set to now
    int safeGainWrites = 0;                                 // times the mic was told to go lower without getting there
    juce::uint32 lastSafeGainWriteMs = 0;
    bool safeGainRestorePending = false;                    // Safe gain was switched off: put the mic's gain back
    bool safeGainAutoOff = false;                           // it is being switched off because the permission went: leave the gain
    bool permissionMissing = false;                         // ... and since when the permission has been missing
    juce::uint32 permissionMissingSinceMs = 0;

    bool micGainApplied = false;                            // "restore mic gain" has been done for this appearance of the mic

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppController)
};
}
