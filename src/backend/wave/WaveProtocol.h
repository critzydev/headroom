// Elgato Wave:3 vendor protocol, the pure part: constants and how the 16-byte config block maps to fields.
// No I/O here, so every rule about what may be sent, and what a block must look like before we change it, is unit-tested.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace hr::wave
{
// ---- What Headroom is allowed to say to the device ---------------------------------------------------------
// Only class requests (bmRequestType 0xA1 / 0x21) to wValue 0 with wIndex 0x3303 (0x3300 | interface 3), always all
// 16 bytes. Nothing else is ever sent: not vendor-type requests, not other block ids. A scan of block ids once
// rebooted a Wave:3 into firmware-update mode.
constexpr uint16_t kVendorId = 0x0fd9;
constexpr uint16_t kProductId = 0x0070;             // original Wave:3 only (0071 is its firmware-update mode)
constexpr uint8_t kRequestTypeIn = 0xA1;            // class | interface | device-to-host
constexpr uint8_t kRequestTypeOut = 0x21;           // class | interface | host-to-device
constexpr uint8_t kRequestGet = 0x85;
constexpr uint8_t kRequestSet = 0x05;
constexpr uint16_t kConfigBlock = 0x0000;           // wValue: the config block, the only one we address
constexpr uint16_t kIndex = 0x3303;                 // wIndex
constexpr uint8_t kInterface = 3;                   // the vendor interface that wIndex points at
constexpr int kTimeoutMs = 500;
constexpr int kMinWriteGapMs = 250;

constexpr size_t kConfigSize = 16;                  // API 5.3 / 5.4. API 5.2 has 14 bytes and is refused.
using ConfigBlock = std::array<uint8_t, kConfigSize>;

// Byte offsets in the config block (Kailas Mahavarkar's layout, from Elgato's own Wave Link binary).
constexpr size_t kGainOffset = 0;                   // int16 LE, Q8.8 dB, 0 .. 40
constexpr size_t kMuteOffset = 4;
constexpr size_t kClipGuardOffset = 5;              // bit 0: 0 = off, 1 = on
constexpr size_t kHeadphoneVolumeOffset = 7;        // int16 LE, Q8.8 dB
constexpr size_t kDialModeOffset = 12;              // 1 = mic, 2 = headphones, 3 = mix
constexpr size_t kGainLockOffset = 15;              // 1 = the mic ignores gain changes that come from the computer
constexpr int kMaxGainRaw = 0x2800;                 // 40 dB

// ---- Reading a block ---------------------------------------------------------------------------------------
std::optional<bool> clipGuardOf (const ConfigBlock&);       // nullopt if the byte is not exactly 0 or 1
std::optional<double> gainDbOf (const ConfigBlock&);        // nullopt if outside 0 .. 40 dB
std::optional<bool> gainLockOf (const ConfigBlock&);

// Does this look like the documented layout? Checked before anything is written, so that a firmware we do not know
// is left alone. On false, `why` (if given) says which check failed.
bool looksLikeConfig (const ConfigBlock&, std::string* why = nullptr);

// ---- Changing a block --------------------------------------------------------------------------------------
// A copy with only the ClipGuard byte changed; every other byte (including the ones nobody documents) is kept as read.
ConfigBlock withClipGuard (ConfigBlock block, bool on);

// Is `readBack` what we wrote? The ClipGuard byte and every byte that only software writes must match exactly. The
// bytes the physical controls move (gain, mute pad, headphone volume, monitor mix, dial mode) may differ, because
// someone can turn the dial between our write and our read.
bool writeWasKept (const ConfigBlock& written, const ConfigBlock& readBack);

std::string toHex (const ConfigBlock&);                     // "00 28 00 ..." for the log
std::optional<ConfigBlock> fromHex (const std::string&);    // and back: 16 hex bytes separated by spaces, else nullopt

// ---- Identity, from the descriptors the kernel hands out for the opened device node --------------------------
struct DeviceIds
{
    uint16_t vendor = 0;
    uint16_t product = 0;

    bool isWave3() const noexcept   { return vendor == kVendorId && product == kProductId; }
};

// The first 18 bytes are the USB device descriptor.
std::optional<DeviceIds> parseDeviceIds (const uint8_t* descriptors, size_t size);

// bInterfaceClass of alternate setting 0 of the given interface, from the whole descriptor dump (device descriptor
// first, then the configuration). nullopt if that interface is not there.
std::optional<uint8_t> interfaceClass (const uint8_t* descriptors, size_t size, uint8_t interfaceNumber);
constexpr uint8_t kVendorSpecificClass = 0xFF;
}
