// Elgato Wave XLR (0fd9:007d) vendor protocol, the pure part: what may be sent, and how the 34-byte config block maps to
// fields. EXPERIMENTAL: Headroom has no Wave XLR to try it on. The protocol is the Wave:3's class-request
// dialect with a different block, documented by three independent public sources: Elgato's
// own Wave Link tables as read by Kailas Mahavarkar, rikkichy/openwave, and emaspa/openxlr, which two community testers ran
// on real units. Headroom uses it for one thing: reading and setting the microphone gain, the one control the kernel does not
// give it for this model. No I/O here, so every rule about what may be sent is unit-tested.
#pragma once

#include "backend/wave/WaveProtocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace hr::wave::xlr
{
// ---- What Headroom is allowed to say to the device ---------------------------------------------------------------
// Class requests (bmRequestType 0xA1 / 0x21, bRequest 0x85 / 0x05) with wIndex 0x3303 (0x3300 | interface 3), the same as the
// Wave:3's (WaveProtocol.h; the constants are shared). Only two blocks are ever addressed: the config block (wValue 0, 34
// bytes: read, and written whole) and the device-info block (wValue 0x000A: read only, 51 bytes), whose first two bytes say
// which firmware API the device speaks. Nothing else is sent: not vendor-type requests, not other block ids.
constexpr uint16_t kProductId = 0x007d;
constexpr uint16_t kVersionBlock = 0x000A;

constexpr size_t kConfigSize = 34;                  // API 1.3 and 1.4. API 1.2 has 29 bytes and is refused.
constexpr size_t kVersionSize = 51;                 // what is asked for; API 1.4 has 53, of which only the first two bytes are used

using ConfigBlock = std::array<uint8_t, kConfigSize>;
using VersionBlock = std::array<uint8_t, kVersionSize>;

// Byte offsets in the config block. Elgato's field table (via Kailas): gain 0, mute 4, headphone volume 9, volume select 14,
// low impedance 33. Phantom power at 6 is from openwave (found against the unit's 48 V light, and verified on the XLR Dock).
constexpr size_t kGainOffset = 0;                   // uint16 LE, Q8.8 dB, 0 .. 75 in steps of 0.5
constexpr size_t kMuteOffset = 4;                   // 1 = muted
constexpr size_t kPhantomOffset = 6;                // 1 = 48 V on. Read for the plausibility check, NEVER written by Headroom.
constexpr size_t kHeadphoneVolumeOffset = 9;        // int16 LE, Q8.8 dB, -128 .. 0
constexpr size_t kLowImpedanceOffset = 33;          // 1 = on
constexpr double kMaxGainDb = 75.0;                 // Elgato's limit (openwave allows 80; the lower one is used)
constexpr int kMaxGainRaw = 0x4B00;                 // 75 dB
constexpr double kGainStepDb = 0.5;

// ---- Reading a block ---------------------------------------------------------------------------------------------
std::optional<double> gainDbOf (const ConfigBlock&);        // nullopt if outside 0 .. 75 dB
std::optional<bool> muteOf (const ConfigBlock&);            // nullopt if the byte is not exactly 0 or 1

// Does this look like the documented layout? Checked before anything is written, so that a firmware we do not know is left
// alone: gain 0 to 75 dB, mute / phantom / low-impedance bytes 0 or 1, headphone volume not above 0 dB.
bool looksLikeConfig (const ConfigBlock&, std::string* why = nullptr);

// Firmware API 1.3 or 1.4: the ones whose config block is these 34 bytes (Kailas, from Wave Link's tables; API 1.2 has 29,
// and API 1.7 is a different table that nobody has published).
bool apiIsKnown (const VersionBlock&);
std::string apiText (const VersionBlock&);                  // "1.4", for the log

// ---- Changing a block --------------------------------------------------------------------------------------------
// A copy with only the two gain bytes changed (quantised to 0.5 dB, clamped to 0 .. 75 dB); every other byte, the ones nobody
// documents included, is kept as read.
ConfigBlock withGainDb (ConfigBlock block, double db);
double quantiseGainDb (double db);

// Is `readBack` what we wrote? The gain must be within one step (the firmware quantises, and a turned dial may have moved it
// since); the bytes the physical controls move (mute, headphone volume, volume select) may differ; every other byte, among
// them 48 V phantom power and low impedance, must be exactly what was written.
bool writeWasKept (const ConfigBlock& written, const ConfigBlock& readBack);

std::string toHex (const ConfigBlock&);
}
