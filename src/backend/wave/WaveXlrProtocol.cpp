// Wave XLR config block helpers (see WaveXlrProtocol.h).
#include "backend/wave/WaveXlrProtocol.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace hr::wave::xlr
{
namespace
{
    int16_t readInt16 (const ConfigBlock& block, size_t offset)
    {
        return (int16_t) (uint16_t) (block[offset] | (block[offset + 1] << 8));
    }

    void writeUint16 (ConfigBlock& block, size_t offset, uint16_t value)
    {
        block[offset] = (uint8_t) (value & 0xff);
        block[offset + 1] = (uint8_t) (value >> 8);
    }

    // Bytes that the unit's own controls move while we are talking to it: the gain dial, the mute pad, the headphone volume and
    // the dial's mode. A difference there between the block we wrote and the one we read back is not our write going wrong.
    bool movedByPhysicalControls (size_t offset)
    {
        switch (offset)
        {
            case 0: case 1:             // gain (checked on its own)
            case 4:                     // mute
            case 9: case 10:            // headphone volume
            case 14:                    // what the dial controls
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

std::optional<double> gainDbOf (const ConfigBlock& block)
{
    const int raw = readInt16 (block, kGainOffset);
    return raw >= 0 && raw <= kMaxGainRaw ? std::optional<double> ((double) raw / 256.0) : std::nullopt;
}

std::optional<bool> muteOf (const ConfigBlock& block)
{
    const auto value = block[kMuteOffset];
    return value <= 1 ? std::optional<bool> (value == 1) : std::nullopt;
}

bool looksLikeConfig (const ConfigBlock& block, std::string* why)
{
    if (! gainDbOf (block).has_value())
    {
        fail (why, "the gain field is outside 0 to 75 dB");
        return false;
    }

    if (block[kMuteOffset] > 1)
    {
        fail (why, "the mute byte is not 0 or 1");
        return false;
    }

    if (block[kPhantomOffset] > 1)
    {
        fail (why, "the phantom power byte is not 0 or 1");
        return false;
    }

    if (block[kLowImpedanceOffset] > 1)
    {
        fail (why, "the low impedance byte is not 0 or 1");
        return false;
    }

    if (readInt16 (block, kHeadphoneVolumeOffset) > 0)
    {
        fail (why, "the headphone volume is above 0 dB");
        return false;
    }

    return true;
}

bool apiIsKnown (const VersionBlock& version)
{
    return version[0] == 1 && (version[1] == 3 || version[1] == 4);
}

std::string apiText (const VersionBlock& version)
{
    char text[24];
    std::snprintf (text, sizeof (text), "%u.%u", (unsigned) version[0], (unsigned) version[1]);
    return text;
}

double quantiseGainDb (double db)
{
    return std::clamp (std::round (db / kGainStepDb) * kGainStepDb, 0.0, kMaxGainDb);
}

ConfigBlock withGainDb (ConfigBlock block, double db)
{
    writeUint16 (block, kGainOffset, (uint16_t) std::lround (quantiseGainDb (db) * 256.0));
    return block;
}

bool writeWasKept (const ConfigBlock& written, const ConfigBlock& readBack)
{
    const auto wantedGain = gainDbOf (written);
    const auto gotGain = gainDbOf (readBack);

    if (! wantedGain.has_value() || ! gotGain.has_value() || std::abs (*wantedGain - *gotGain) > kGainStepDb + 0.01)
        return false;

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
}
