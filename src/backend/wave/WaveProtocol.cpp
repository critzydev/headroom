// Wave:3 config block helpers (see WaveProtocol.h).
#include "backend/wave/WaveProtocol.h"

#include <cstdio>

namespace hr::wave
{
namespace
{
    int16_t readInt16 (const ConfigBlock& block, size_t offset)
    {
        return (int16_t) (uint16_t) (block[offset] | (block[offset + 1] << 8));
    }

    // Bytes that the mic's own controls move while we are talking to it (dial value, mute pad, headphone volume,
    // monitor mix, dial mode). A difference there between the block we wrote and the one we read back is not our
    // write going wrong.
    bool movedByPhysicalControls (size_t offset)
    {
        switch (offset)
        {
            case 0: case 1:             // gain
            case 4:                     // mute
            case 7: case 8: case 9:     // headphone volume and mute
            case 10: case 11:           // monitor mix
            case 12:                    // dial mode
                return true;

            default:
                return false;
        }
    }

    void fail (std::string* why, const char* text)
    {
        if (why != nullptr)
            *why = text;
    }
}

std::optional<bool> clipGuardOf (const ConfigBlock& block)
{
    const auto value = block[kClipGuardOffset];
    return value <= 1 ? std::optional<bool> (value == 1) : std::nullopt;
}

std::optional<double> gainDbOf (const ConfigBlock& block)
{
    const int raw = readInt16 (block, kGainOffset);
    return raw >= 0 && raw <= kMaxGainRaw ? std::optional<double> ((double) raw / 256.0) : std::nullopt;
}

std::optional<bool> gainLockOf (const ConfigBlock& block)
{
    const auto value = block[kGainLockOffset];
    return value <= 1 ? std::optional<bool> (value == 1) : std::nullopt;
}

bool looksLikeConfig (const ConfigBlock& block, std::string* why)
{
    if (! gainDbOf (block).has_value())
    {
        fail (why, "the gain field is outside 0 to 40 dB");
        return false;
    }

    if (block[kMuteOffset] > 1)
    {
        fail (why, "the mute byte is not 0 or 1");
        return false;
    }

    if (! clipGuardOf (block).has_value())
    {
        fail (why, "the ClipGuard byte is not 0 or 1");
        return false;
    }

    if (block[kDialModeOffset] < 1 || block[kDialModeOffset] > 3)
    {
        fail (why, "the dial mode is not 1, 2 or 3");
        return false;
    }

    return true;
}

ConfigBlock withClipGuard (ConfigBlock block, bool on)
{
    block[kClipGuardOffset] = on ? 1 : 0;
    return block;
}

bool writeWasKept (const ConfigBlock& written, const ConfigBlock& readBack)
{
    for (size_t i = 0; i < kConfigSize; ++i)
        if (readBack[i] != written[i] && ! movedByPhysicalControls (i))
            return false;

    return true;
}

std::string toHex (const ConfigBlock& block)
{
    std::string text;

    for (size_t i = 0; i < kConfigSize; ++i)
    {
        char byte[4];
        std::snprintf (byte, sizeof (byte), i == 0 ? "%02x" : " %02x", (unsigned) block[i]);
        text += byte;
    }

    return text;
}

std::optional<ConfigBlock> fromHex (const std::string& text)
{
    ConfigBlock block {};
    size_t at = 0;

    for (size_t i = 0; i < kConfigSize; ++i)
    {
        if (i > 0)
        {
            if (at >= text.size() || text[at] != ' ')
                return std::nullopt;

            ++at;
        }

        int value = 0;

        for (int digit = 0; digit < 2; ++digit, ++at)
        {
            if (at >= text.size())
                return std::nullopt;

            const char c = text[at];
            const int nibble = c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1));

            if (nibble < 0)
                return std::nullopt;

            value = value * 16 + nibble;
        }

        block[i] = (uint8_t) value;
    }

    return at == text.size() ? std::optional<ConfigBlock> (block) : std::nullopt;
}

std::optional<DeviceIds> parseDeviceIds (const uint8_t* descriptors, size_t size)
{
    constexpr size_t kDeviceDescriptorSize = 18;
    constexpr uint8_t kDeviceDescriptorType = 1;

    if (descriptors == nullptr || size < kDeviceDescriptorSize || descriptors[0] != kDeviceDescriptorSize
        || descriptors[1] != kDeviceDescriptorType)
        return std::nullopt;

    DeviceIds ids;
    ids.vendor = (uint16_t) (descriptors[8] | (descriptors[9] << 8));
    ids.product = (uint16_t) (descriptors[10] | (descriptors[11] << 8));
    return ids;
}

std::optional<uint8_t> interfaceClass (const uint8_t* descriptors, size_t size, uint8_t interfaceNumber)
{
    constexpr size_t kDeviceDescriptorSize = 18;
    constexpr uint8_t kInterfaceDescriptorType = 4;
    constexpr size_t kInterfaceDescriptorSize = 9;

    if (descriptors == nullptr || size < kDeviceDescriptorSize)
        return std::nullopt;

    // Every descriptor starts with its length and type, so the rest can be walked without knowing them all.
    for (size_t at = kDeviceDescriptorSize; at + 2 <= size;)
    {
        const size_t length = descriptors[at];

        if (length < 2 || at + length > size)
            break;

        if (descriptors[at + 1] == kInterfaceDescriptorType && length >= kInterfaceDescriptorSize
            && descriptors[at + 2] == interfaceNumber && descriptors[at + 3] == 0)
            return descriptors[at + 5];

        at += length;
    }

    return std::nullopt;
}
}
