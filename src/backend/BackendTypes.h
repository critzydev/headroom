// Plain data types exchanged between the UI/AppController and an AudioBackend.
#pragma once

#include "Juce.h"
#include <cstdint>
#include <optional>
#include <vector>

namespace hr
{
struct DeviceInfo      { juce::String id;   /* PipeWire node.name */  juce::String label; bool isWave = false;
                         int channels = 0;      /* audio ports of the node (0 = not known): an input with two is offered as stereo */ };
struct AppStreamInfo   { uint32_t nodeId = 0; juce::String appName, binary, mediaName, iconName;
                         juce::String channelId;   /* channel it is currently routed to, "" = not ours */ };
enum class MicState    { NotFound, Starting, Ok, Recovering, SilentFromStart, MutedOnDevice };

// One device a mix plays to, as the backend has settled on it: the automatic choice is already the device it means.
struct OutputState
{
    juce::String id;                        // node.name of the device; the requested id when it is not there
    juce::String label;                     // model-aware ("Elgato Wave:3 Headphones"); what the id looks like when the device is missing
    bool automatic = false;                 // it is what "Automatic" means (the request was "auto")
    bool present = false;                   // the device is on the machine
    bool playing = false;                   // ... and the mix is linked to it
};

// One input channel and the device it takes its sound from.
struct InputState
{
    juce::String channelId;
    juce::String deviceId;
    juce::String label;                     // model-aware; for a device that is not there, what it was called when it was last seen
    bool connected = false;                 // the device is on the machine (the channel then has its sound)
};

struct BackendStatus   { bool connected = false;              // talking to PipeWire
                         MicState mic = MicState::NotFound;
                         bool outputOk = false;               // the first device of the Headphones mix is linked
                         juce::String inputLabel, outputLabel;     // the mic's, and the first Headphones device's
                         std::vector<OutputState> headphoneOutputs, streamOutputs;   // everything the two mixes play to now (Stream: the extras only)
                         std::vector<InputState> inputChannels;                      // one per input channel of the model
                         double sampleRate = 0; int quantum = 0;
                         // Current Wave hardware values read back from the device (0..1, pactl-style cubic %),
                         // -1 = unknown / not a device with hardware controls. The UI shows these, not the stored setting.
                         float hardwareMicGain = -1.0f, hardwareHeadphoneVolume = -1.0f;
                         juce::String message; };            // human readable, for the status banner
// The USB controls of a model whose protocol is documented publicly but that Headroom has not been able to try:
// only with the user's opt-in. Today that is the mic gain of the Wave XLR. Empty `model` = the selected input is not such a model.
struct ExperimentalControls
{
    juce::String model;                     // "Wave XLR": the selected input is such a model, so Settings offers the opt-in
    bool on = false;                        // the user opted in
    bool available = false;                 // ... and it is on the USB bus
    bool permission = false;                // ... and this user may open it (else "Allow hardware controls" is needed)
    std::optional<float> gainDb;            // what the mic says its gain is, from the last read
    bool busy = false;                      // a change, or the password box, is in progress
    juce::String error;                     // one plain sentence, empty when all is well
};

// What Headroom can do with the mic itself over USB: ClipGuard today. Everything is about the original
// Wave:3 (0fd9:0070) that is the selected input; for anything else usbAvailable is false and the UI shows nothing of it.
struct HardwareControls
{
    bool usbAvailable = false;              // the selected input is a Wave:3 that is on the USB bus (this machine can do it)
    bool usbPermission = false;             // ... and this user may open it (else "Allow hardware controls" is needed)
    std::optional<bool> clipGuard;          // what the mic reports, or is being set to; nullopt = not read yet
    std::optional<float> gainDb;            // the mic's own gain in dB (0 ... 40), from the same read: what its dial says
    std::optional<bool> gainLocked;         // the mic ignores gain changes that come from the computer (Wave Link sets this)
    bool busy = false;                      // a change, or the password box, is in progress
    juce::String error;                     // one plain sentence, empty when all is well
    ExperimentalControls experimental;      // the experimental models, for the selected input (never the Wave:3)
};

struct ChannelEndpoint { juce::String channelId; juce::String displayName; int slot; };   // app channels only
// An input channel: a capture device mixed like an app channel. `stereo` takes its left and right, else its first
// channel on both sides.
struct InputEndpoint   { juce::String channelId; juce::String displayName; juce::String deviceId; int slot; bool stereo; };
struct RoutingRule     { juce::String pattern; juce::String channelId; };  // wildcard, lower-case, matched against binary and app name
}
