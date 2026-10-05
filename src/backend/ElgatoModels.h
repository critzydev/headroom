// The Elgato Wave family by USB product id: what each model is called, how its PipeWire nodes are named, whether it shares
// one clock between capture and playback, and how far Headroom may go with its USB controls.
// Pure data and functions (no PipeWire, no USB), so every rule is unit-tested on every platform. The facts come from USB
// descriptors, PipeWire and ALSA dumps, user reports and published protocol descriptions; Clock::Assumed marks what is only
// inferred from the family.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hr::elgato
{
constexpr int kVendorId = 0x0fd9;       // Elgato Systems GmbH

enum class Kind
{
    Microphone,         // a USB microphone with its own headphone jack (Wave:1, Wave:3, Wave Neo)
    Interface           // an XLR interface or dock: the microphone is plugged into it
};

// "If playback opens before capture, the mic records silence for the life of that capture stream" (one firmware clock).
enum class Clock
{
    Reported,           // users report it for this model
    Assumed             // no report found, but it is the same firmware family: treated the same, it costs a few ms
};

// What Headroom does over USB for this model, beyond what ALSA gives it through PipeWire (gain and mute routes).
enum class Hardware
{
    None,               // nothing: either no public protocol, or one that does not meet Headroom's safety rules
    Tested,             // the original Wave:3, tried on real hardware by us
    Experimental        // a documented protocol that we have not been able to try; only with the user's opt-in
};

struct Model
{
    uint16_t pid;
    const char* name;           // label base: "Elgato Wave:3" (the headphones are "<name> Headphones")
    const char* shortName;      // "Wave:3", for sentences and the small pill
    const char* nodeFragment;   // the product part of the ALSA node name, udev style: "Wave_3_" in usb-Elgato_Systems_Elgato_Wave_3_<serial>
    Kind kind;
    Clock clock;
    int captureChannels;        // what the kernel offers for the mic (0 = not known to us)
    int playbackChannels;       // ... and for the headphones
    const char* goodProfile;    // the profile the card normally runs in ("" = no preference: the best one the card offers)
    int monitorPair;            // the pair of playback channels the headphones listen to (0 = the first two)
    Hardware hardware;
    int usbConfigBytes;         // size of the config block of its class-request protocol (0 = none documented)
    double usbGainMaxDb;        // top of the mic gain range over that protocol (0 = none documented)
};

// Every model we know, in a stable order.
const std::vector<Model>& models();

const Model* findByPid (int productId);

// By the node.name (or card name) udev made from the USB strings: "alsa_input.usb-Elgato_Systems_Elgato_Wave_3_BS49-00.mono-fallback".
// Needed only where there is no device behind the node (a fake in the integration test, or an old PipeWire); the real thing
// is recognised by its USB ids. The longest matching fragment wins ("Wave_XLR_Dock_MK.2_" before "XLR_Dock_").
const Model* findByNodeName (const std::string& name);

// ---- Who is who ------------------------------------------------------------------------------------------------------
struct Identity
{
    bool elgato = false;            // USB vendor 0x0fd9, or an Elgato node name: the profile rules for Elgato devices apply
    bool wave = false;              // a Wave microphone or XLR interface (or one we have not met): WaveGuard, "auto" preference, gain readback
    const Model* model = nullptr;   // the model, when we know it
    int productId = -1;

    bool isWave3() const noexcept   { return model != nullptr && model->pid == 0x0070; }
};

// vendorId / productId < 0 when there is no device behind the node. `name` is the node.name or the card name;
// `description` is any text the device or node says about itself (description, nick, product name), for models not in the table.
//
//   * an Elgato PID that is in the table: that model;
//   * an Elgato PID that is not: an unknown Elgato device. It counts as a Wave (WaveGuard on, no hardware controls) unless
//     its name says it is not a microphone (a capture card, a camera): such a thing is listed and works, with nothing done to it;
//   * no ids at all: the node name decides (fragment table, then "usb-Elgato...Wave/XLR...");
//   * any other vendor: not Elgato, nothing special.
Identity identify (int vendorId, int productId, const std::string& name, const std::string& description);

// ---- Names -----------------------------------------------------------------------------------------------------------
// "Elgato Wave:3" / "Elgato Wave:3 Headphones". The model's own name when it is known; otherwise whatever the device calls
// itself, with "Elgato " in front when it does not say so.
std::string label (const Identity& who, bool input, const std::string& deviceDescription, const std::string& deviceNick,
                   const std::string& nodeDescription, const std::string& nodeNick);

// "Wave:3" for a known model, else the label without the brand: for sentences about the mic.
std::string shortName (const Identity& who, const std::string& fallbackLabel);

// ---- What each model gets over USB ----------------------------------------------------------------------------------
// The products whose USB controls Headroom drives, by support level. The udev rule covers exactly these.
std::vector<uint16_t> pidsWith (Hardware level);

const char* describe (Hardware level);      // "none" / "tested" / "experimental"
const char* describe (Clock clock);         // "reported" / "assumed"

bool containsIgnoreCase (const std::string& text, const std::string& part);
}
