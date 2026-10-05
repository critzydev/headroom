// The interface between the app and whatever moves audio: PipeWireBackend on Linux, NullBackend elsewhere.
#pragma once

#include "Juce.h"
#include "backend/BackendTypes.h"

#include <functional>
#include <memory>
#include <vector>

namespace hr
{
class AudioEngine;

class AudioBackend
{
public:
    virtual ~AudioBackend() = default;
    virtual bool start (AudioEngine&) = 0;                    // message thread
    virtual void stop() = 0;

    // Model -> backend (message thread; implementations must not block for long).
    // Implementations must accept these before start() too: AppController pushes the whole model first.
    virtual void setChannels (const std::vector<ChannelEndpoint>&) = 0;   // create / rename / remove virtual sinks
    // The mic, and every device each mix plays to at once. "auto" = prefer a Wave. `headphones` is never empty; a
    // device that is not there is left out while the others play (all of them missing: the automatic choice, as with one device).
    // `stream` lists the extra devices the Stream mix also plays to ("Headroom Stream Mix" always gets it): empty = none.
    virtual void setDevices (const juce::String& inputId, const juce::StringArray& headphones, const juce::StringArray& stream) = 0;
    // The input channels: capture devices that are mixed like app channels. The whole list every time.
    virtual void setInputChannels (const std::vector<InputEndpoint>&) = 0;
    virtual void setRoutingRules (const std::vector<RoutingRule>&, const juce::String& fallbackChannelId) = 0;
    virtual void setMakeDefault (bool) = 0;                   // set Headroom sinks/sources as system defaults
    virtual void setKeepDefault (bool) = 0;                   // ... and put them back whenever anything else changes them (only while makeDefault is on)
    virtual void setLatencyQuantum (int frames) = 0;          // 128 / 256 / 512
    virtual void routeApp (uint32_t nodeId, const juce::String& channelId) = 0; // move a live stream now
    virtual void runMicRecovery() = 0;                        // user pressed "Fix my mic"
    virtual void setHardwareMicGain (float linear0to1) = 0;   // Wave:3 "Mic Capture Volume" via device Route
    virtual void setHardwareHeadphoneVolume (float linear0to1) = 0;

    // Wave:3 hardware controls over USB. Defaults do nothing: a backend without USB support (or a test
    // double) needs no code for them. All message thread; none of them blocks, the USB work is on the backend's own thread.
    virtual void setClipGuard (bool)                        {}      // the state to keep the mic in (applied when possible)
    virtual void requestHardwarePermission()                {}      // "Allow hardware controls": asks for the password once
    virtual void setExperimentalHardware (bool)             {}      // the opt-in for models whose USB protocol is documented but untried

    // Backend -> UI (message thread)
    virtual HardwareControls getHardwareControls() const    { return {}; }
    virtual std::vector<DeviceInfo> getInputs() const = 0;
    virtual std::vector<DeviceInfo> getOutputs() const = 0;
    virtual std::vector<AppStreamInfo> getAppStreams() const = 0;
    virtual BackendStatus getStatus() const = 0;
    std::function<void()> onChanged;                          // lists or status changed (called on message thread)
};

std::unique_ptr<AudioBackend> createPlatformBackend();        // PipeWireBackend on Linux, NullBackend elsewhere
}
