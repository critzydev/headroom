// Choosing and describing devices without PipeWire: the "auto" choice, the order of the lists, which ports carry the mic and
// the headphones, what WaveGuard may do for a given input, and the words it uses.
// Plain data in, plain data out: the PipeWire layer builds these from its registry, and every rule here is unit-tested on
// every platform with made-up devices.
#pragma once

#include "backend/BackendTypes.h"
#include "backend/ElgatoModels.h"

#include <cstdint>
#include <string>
#include <vector>

namespace hr::devices
{
// ---- Which device ----------------------------------------------------------------------------------------------------
// A sink or a source that could be chosen (Headroom's own nodes and nodes without a name are never candidates).
struct Candidate
{
    std::string name;           // node.name, which is also the id the settings store
    bool real = false;          // belongs to a device (an ALSA card, a Bluetooth device), not a virtual or app node
    int priority = 0;           // priority.session
    bool wave = false;          // an Elgato Wave microphone / interface (elgato::Identity::wave)
};

// An id that is present is honoured. "auto" (or an id that is not there any more) prefers an Elgato Wave, then a real device,
// then anything, each by priority.session; among a Wave's own nodes a Pro Audio one (".pro-" in its name) always loses to a
// normal one, because Pro Audio has no hardware gain control. Returns an index into `candidates`, or -1.
int pickCandidate (const std::vector<Candidate>& candidates, const std::string& requestedId);

// Wave first, then by label, then by id: the order of the lists does not jump around between refreshes.
void sortDeviceList (std::vector<hr::DeviceInfo>& list);

// The card a node belongs to, from its udev-style name, for nodes that have no device object to compare ("alsa_input.usb-Elgato_
// Systems_Elgato_Wave_3_ABC-00.mono-fallback" and "alsa_output.usb-Elgato_Systems_Elgato_Wave_3_ABC-00.analog-stereo" are one
// card). Empty when the name has no such shape. Used with the pw_device id, which is what normally tells two nodes of one device.
std::string cardOfNodeName (const std::string& nodeName);

// What a device that is not plugged in is called, from its node.name alone ("alsa_output.usb-Generic_USB_Audio-00.analog-stereo" ->
// "Generic USB Audio"): the label of a device Headroom has not seen since it started. The id itself when it has no known shape.
std::string friendlyNameFromNodeName (const std::string& nodeName);

// A mix plays to several devices at once. `requested` is what the model stores: "auto" or node.names, in order.
// The result has one entry per request that is not a repeat: where a request lands in `candidates` (-1 = that device is not there)
// and whether it was "auto". Two requests that land on the same device give one entry (the first). With `fallBackToAuto`, a list
// in which no device is there at all (one unplugged output, the case with a single device before there were several) gets the
// automatic choice as a last entry, so that a mix is never left without anywhere to play. The Stream mix's extra outputs do not
// fall back: nothing plays there until the device is back.
struct OutputChoice
{
    std::string requested;
    int index = -1;
    bool automatic = false;
};

std::vector<OutputChoice> resolveOutputs (const std::vector<Candidate>& candidates, const std::vector<std::string>& requested,
                                          bool fallBackToAuto);

// ---- Which ports ------------------------------------------------------------------------------------------------------
constexpr uint32_t kNoPort = 0xFFFFFFFFu;       // SPA_ID_INVALID

struct PortFacts
{
    uint32_t id = kNoPort;
    std::string channel;                        // audio.channel: "FL", "FR", "MONO", "AUX0", ...
};

struct StereoPorts
{
    uint32_t left = kNoPort;
    uint32_t right = kNoPort;                   // kNoPort: a mono mic, or one channel of the pair is missing
};

// FL/FR by channel, else MONO, else the first two in the order given (ascending port id).
StereoPorts pickStereoPorts (const std::vector<PortFacts>& ports);

// The capture ports Headroom listens to.
//   * an Elgato device: the first channel only. The mic is channel 1 on every model; whatever else the stream carries (the
//     second channel of an XLR interface's stereo stream, the Pro's aux and line channels) is not it;
//   * any other device with one or two ports: as they are (the engine takes a mono mic as it is, and a pair as (L+R)/2);
//   * any other device with more than two (an audio interface): the first channel only, because that is where the mic is,
//     and summing it with an empty input only costs 6 dB and adds the other input's noise.
StereoPorts pickMicPorts (const std::vector<PortFacts>& ports, bool elgato);

// The playback ports of the headphones. `pair` 0 is the first two (as pickStereoPorts); a model whose headphones listen to a
// later pair (Elgato::Model::monitorPair) gets that pair if the device has enough ports, else the first.
StereoPorts pickPlaybackPorts (const std::vector<PortFacts>& ports, int pair);

// The ports of a capture device that feed an input channel's left and right inputs. The slot always gets a pair:
//   * mono: the device's first channel (FL or MONO by name, else the first port) on both sides, whatever else the device has;
//   * stereo: FL / FR (else the first two ports); a device with only one port feeds both sides with it.
// With more than two ports the first channel(s) are taken, as for the mic (pickMicPorts): the rest of an audio interface is
// not this channel's.
StereoPorts pickInputPorts (const std::vector<PortFacts>& ports, bool stereo);

// One link from an output port to an input port.
struct PlannedLink
{
    uint32_t out = kNoPort;
    uint32_t in = kNoPort;
};

// The engine's two output ports to every device's pair (left to left, right to right). A device with one playback port gets only
// the left side, as a single output always did. A port that is missing makes no link.
std::vector<PlannedLink> planFanOut (uint32_t outLeft, uint32_t outRight, const std::vector<StereoPorts>& sinks);

// A capture device's ports (a mono one on both sides) to an input slot's two inputs.
std::vector<PlannedLink> planInputSlot (const StereoPorts& source, uint32_t slotLeft, uint32_t slotRight);

// ---- What WaveGuard may do ---------------------------------------------------------------------------------------------
// WaveGuard's job exists for the Elgato firmware that shares one clock between capture and playback. Nothing
// of it touches any other device.
struct GuardPlan
{
    bool captureFirst = false;          // link the mic first and let the headphones wait for its buffers
    bool recoverSilentMic = false;      // a mic that is silent from the start: cycle the device's profile, twice at most
    bool unmute = false;                // undo a software mute on the mic's route
};

GuardPlan guardPlanFor (const elgato::Identity& input);

// Capture-first for a device that is an input channel (not the main mic): playback on the same Elgato device waits until that
// capture flows. This is the whole of what an input channel gets of WaveGuard: no recovery, no profile change, no unmute (those
// belong to the main mic and to the headphone device).
bool inputNeedsCaptureFirst (const elgato::Identity& device);

// The gate that holds back playback on a device until the capture of an input channel on the same device flows, with the same
// timings as the main mic's WaveGuard (500 ms to see buffers, 4 s to give up on a capture that cannot be linked). Plain state,
// updated on every pass of the backend; open also when there is nothing to wait for (no device, no capture needed).
class CaptureGate
{
public:
    static constexpr int64_t kConfirmMs = 500;
    static constexpr int64_t kLinkGiveUpMs = 4000;
    static constexpr uint32_t kLiveCycles = 3;

    // `needed`: this input channel's device plays one of the mixes too. `present`: its ports are there and can be linked.
    // `linked`: all its capture links exist. `liveCycles`: the engine's count of consecutive cycles with a buffer in the slot.
    // Returns true when playback on the device may be linked.
    bool update (bool needed, bool present, bool linked, uint32_t liveCycles, int64_t nowMs);
    bool isOpen() const noexcept            { return open; }

private:
    enum class Phase { Idle, Linking, Confirming, Open };
    Phase phase = Phase::Idle;
    int64_t phaseStartMs = 0;
    bool open = true;
};

// Profiles are only ever switched on an Elgato Wave device.
bool mayFixProfile (const elgato::Identity& device);

// ---- Words -----------------------------------------------------------------------------------------------------------
struct MicWording
{
    bool elgato = false;
    bool wave3 = false;                 // the one with the tap-to-mute ring we have in hand: its own sentences
    std::string shortName;              // "Wave XLR"
};

MicWording wordingFor (const elgato::Identity& input, const std::string& inputLabel);

// The one sentence for the status banner; empty when the mic needs no words.
juce::String micMessage (hr::MicState state, const MicWording& wording);
}
