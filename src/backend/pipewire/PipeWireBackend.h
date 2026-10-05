// The real audio backend on Linux: PipeWire graph, virtual devices, routing, WaveGuard.
#pragma once

#include "backend/AudioBackend.h"

#include <memory>

namespace hr
{

/**
    Implements hr::AudioBackend on libpipewire. All PipeWire work happens on a private
    pw_thread_loop; the public methods are called from the JUCE message thread, take the
    loop lock briefly and never wait for PipeWire. getX() return snapshots under a plain lock,
    and onChanged is posted to the message thread (at most about 20 times a second).
*/
class PipeWireBackend final : public AudioBackend
{
public:
    PipeWireBackend();
    ~PipeWireBackend() override;

    bool start (AudioEngine& engine) override;
    void stop() override;

    void setChannels (const std::vector<ChannelEndpoint>& channels) override;
    void setDevices (const juce::String& inputId, const juce::StringArray& headphones, const juce::StringArray& stream) override;
    void setInputChannels (const std::vector<InputEndpoint>& inputs) override;
    void setRoutingRules (const std::vector<RoutingRule>& rules, const juce::String& fallbackChannelId) override;
    void setMakeDefault (bool makeDefault) override;
    void setKeepDefault (bool keepDefault) override;
    void setLatencyQuantum (int frames) override;
    void routeApp (uint32_t nodeId, const juce::String& channelId) override;
    void runMicRecovery() override;
    void setHardwareMicGain (float linear0to1) override;
    void setHardwareHeadphoneVolume (float linear0to1) override;
    void setClipGuard (bool on) override;
    void requestHardwarePermission() override;
    void setExperimentalHardware (bool on) override;

    HardwareControls getHardwareControls() const override;
    std::vector<DeviceInfo> getInputs() const override;
    std::vector<DeviceInfo> getOutputs() const override;
    std::vector<AppStreamInfo> getAppStreams() const override;
    BackendStatus getStatus() const override;

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace hr
