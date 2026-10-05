// NullBackend: fake devices and app streams plus a synthetic-signal thread driving the engine.
#pragma once

#include "backend/AudioBackend.h"
#include "engine/EngineTypes.h"

#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <vector>

namespace hr
{
class NullBackend final : public AudioBackend
{
public:
    // micMutedOnDevice: report MicState::MutedOnDevice and feed digital silence (--demo-mic-muted).
    explicit NullBackend (bool micMutedOnDevice = false);
    ~NullBackend() override;

    bool start (AudioEngine&) override;
    void stop() override;

    void setChannels (const std::vector<ChannelEndpoint>&) override;
    void setDevices (const juce::String& inputId, const juce::StringArray& headphones, const juce::StringArray& stream) override;
    void setInputChannels (const std::vector<InputEndpoint>&) override;
    void setRoutingRules (const std::vector<RoutingRule>&, const juce::String& fallbackChannelId) override;
    void setMakeDefault (bool) override;
    void setKeepDefault (bool) override;
    void setLatencyQuantum (int frames) override;
    void routeApp (uint32_t nodeId, const juce::String& channelId) override;
    void runMicRecovery() override;
    void setHardwareMicGain (float linear0to1) override;
    void setHardwareHeadphoneVolume (float linear0to1) override;
    void setClipGuard (bool on) override;
    void requestHardwarePermission() override;
    void setExperimentalHardware (bool on) override         { experimentalHardware = on; }

    HardwareControls getHardwareControls() const override;
    std::vector<DeviceInfo> getInputs() const override;
    std::vector<DeviceInfo> getOutputs() const override;
    std::vector<AppStreamInfo> getAppStreams() const override;
    BackendStatus getStatus() const override;

    // What the app last asked for (there is no system default to change here); the tests read these back.
    bool getMakeDefault() const noexcept    { return makeDefault; }
    bool getKeepDefault() const noexcept    { return keepDefault; }
    bool getExperimentalHardware() const noexcept   { return experimentalHardware; }

    // What the fake Wave:3 says about its own gain over "USB" (the real one reads it from its config block). By default it
    // reports the gain that was set through the slider, like a mic that took the request; a value here plays a mic that
    // did not (gain lock, a turned dial). nullopt goes back to following the slider.
    void setDemoUsbGain (std::optional<float> db, std::optional<bool> locked = std::nullopt);

    // The mic's route coming and going, for tests: while it is absent the hardware gain reads -1 (unknown), and when it
    // comes back the mic has powered up at `gainOnReturn` (its maximum), like a real Wave:3 after a replug.
    void setDemoMicPresent (bool present, float gainOnReturn = 1.0f);

    // The udev rule going away (tests): the permission is gone until it is requested again.
    void revokeHardwarePermission();

private:
    // What kind of synthetic signal a fake app produces. Values are bit positions in the per-slot masks.
    enum class Kind { Music, Game, Chat, System };

    struct FakeStream
    {
        AppStreamInfo info;
        Kind kind;
    };

    class SignalThread;

    void reroute();                 // re-evaluate rules for every fake stream, refresh slot masks, notify
    void refreshSlotMasks();
    void notifyChanged();
    juce::String resolveDeviceLabel (const std::vector<DeviceInfo>& devices, const juce::String& id) const;
    std::vector<OutputState> resolveOutputStates (const juce::StringArray& requested, bool fallBackToAuto) const;
    void refreshInputSlots();

    const bool micMuted;

    // Message thread state
    std::vector<FakeStream> streams;
    std::vector<ChannelEndpoint> endpoints;
    std::vector<RoutingRule> rules;
    juce::String fallbackChannel;
    juce::String inputId { "auto" };
    juce::StringArray headphoneIds { "auto" }, streamIds;       // what each mix plays to (the demo has nowhere to send it, but says where)
    std::vector<InputEndpoint> inputEndpoints;
    int quantum = 256;
    bool running = false;
    bool makeDefault = true, keepDefault = true;
    bool experimentalHardware = false;      // the experimental-hardware opt-in; the demo has no model it applies to
    float hardwareMicGain = 0.5f, hardwareHeadphoneVolume = 0.5f;

    // The fake USB side: no permission until it is requested, then ClipGuard reads off and can be set.
    bool usbPermission = false;
    bool clipGuardOn = false;
    std::optional<bool> desiredClipGuard;
    std::optional<float> demoUsbGainDb;
    bool demoMicPresent = true;
    std::optional<bool> demoUsbGainLocked;

    // Shared with the signal thread: bit k set = signal kind k should be present on app slot s.
    std::array<std::atomic<uint32_t>, kMaxAppChannels> slotKinds;

    // ... and 1 = an input channel's device is on the machine, so its slot has a (speech-like) signal.
    std::array<std::atomic<uint32_t>, kMaxInputChannels> inputLive;

    std::unique_ptr<SignalThread> thread;

    struct Notifier;
    std::unique_ptr<Notifier> notifier;     // coalesces onChanged calls onto the message thread
};
}
