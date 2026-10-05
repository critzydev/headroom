// XlrUsb: the guarded read / read-modify-write of the Wave XLR config block (see WaveXlrUsb.h).
#include "backend/wave/WaveXlrUsb.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hr::wave::xlr
{
std::string describe (Error error)
{
    switch (error)
    {
        case Error::None:           return {};
        case Error::Unsupported:    return "Hardware controls only work on Linux.";
        case Error::NotFound:       return "The Wave XLR is not connected.";
        case Error::NoPermission:   return "Headroom is not allowed to control the Wave XLR yet.";
        case Error::Busy:           return "Another program is using the Wave XLR's controls. Close it and try again.";
        case Error::WrongDevice:    return "That does not look like a Wave XLR, so Headroom left it alone.";
        case Error::UnknownLayout:  return "The Wave XLR answered in a way Headroom does not know, so it left it alone.";
        case Error::Io:             return "The Wave XLR did not answer. Unplug it, plug it back in and try again.";
        case Error::NotKept:        return "The Wave XLR did not keep the change.";
    }

    return {};
}

namespace
{
    constexpr const char* kSysfs = "/sys/bus/usb/devices";

    // The errno of a failed transfer, as the kind of trouble it means.
    Error errorForErrno (int code)
    {
        switch (code)
        {
            case EBUSY:
            case EAGAIN:    return Error::Busy;
            case EACCES:
            case EPERM:     return Error::NoPermission;
            case ENODEV:
            case ENOENT:    return Error::NotFound;
            default:        return Error::Io;
        }
    }

    std::string errnoText (int code)
    {
        return std::string (std::strerror (code)) + " (errno " + std::to_string (code) + ")";
    }

    class SystemXlrBus final : public XlrBus
    {
    public:
        Presence probe() override
        {
            return presenceOfNodes (findUsbNodes (kSysfs, kVendorId, kProductId));
        }

        std::unique_ptr<UsbDevice> open (Error& error, std::string& detail) override
        {
            if (probe() == Presence::Unsupported)
            {
                error = Error::Unsupported;
                detail = "no USB access on this platform";
                return nullptr;
            }

            const auto nodes = findUsbNodes (kSysfs, kVendorId, kProductId);

            if (nodes.empty())
            {
                error = Error::NotFound;
                detail = "no 0fd9:007d in " + std::string (kSysfs);
                return nullptr;
            }

            if (nodes.size() > 1)
            {
                error = Error::WrongDevice;
                detail = "more than one 0fd9:007d is connected";
                return nullptr;
            }

            return openUsbNode (nodes.front(), error, detail);
        }
    };
}

std::unique_ptr<XlrBus> createSystemXlrBus()
{
    return std::make_unique<SystemXlrBus>();
}

// ---------------------------------------------------------------------------------------------------
// XlrUsb
// ---------------------------------------------------------------------------------------------------

XlrUsb::XlrUsb (std::unique_ptr<XlrBus> busToUse, WaveUsb::Clock clockToUse) : bus (std::move (busToUse)), clock (std::move (clockToUse)) {}

Presence XlrUsb::probe()
{
    const std::lock_guard<std::mutex> lock (mutex);
    const auto presence = bus->probe();

    if (presence != Presence::Ready)
        apiChecked = false;         // the device went away or may be a different one when it is back: ask again

    return presence;
}

Error XlrUsb::lastError() const
{
    const std::lock_guard<std::mutex> lock (mutex);
    return error;
}

std::string XlrUsb::lastErrorText() const
{
    const std::lock_guard<std::mutex> lock (mutex);
    return xlr::describe (error);
}

std::string XlrUsb::lastErrorDetail() const
{
    const std::lock_guard<std::mutex> lock (mutex);
    return errorDetail;
}

void XlrUsb::setError (Error newError, std::string detail)
{
    error = newError;
    errorDetail = std::move (detail);
}

// Opens the node and checks, from the descriptors of the device that really got opened (not from what sysfs said a moment
// ago), that it is exactly a Wave XLR with a vendor-specific interface 3 (the one wIndex 0x3303 points at).
bool XlrUsb::openVerified (std::unique_ptr<UsbDevice>& device)
{
    Error openError = Error::None;
    std::string detail;
    device = bus->open (openError, detail);

    if (device == nullptr)
    {
        setError (openError, detail);
        return false;
    }

    const auto descriptors = device->readDescriptors();
    const auto ids = parseDeviceIds (descriptors.data(), descriptors.size());

    if (! ids.has_value() || ids->vendor != kVendorId || ids->product != kProductId)
    {
        char text[80];
        std::snprintf (text, sizeof (text), "the opened device is %04x:%04x, not 0fd9:007d", ids ? ids->vendor : 0, ids ? ids->product : 0);
        setError (Error::WrongDevice, text);
        device.reset();
        return false;
    }

    const auto interfaceKind = interfaceClass (descriptors.data(), descriptors.size(), wave::kInterface);

    if (! interfaceKind.has_value() || *interfaceKind != kVendorSpecificClass)
    {
        char text[96];
        std::snprintf (text, sizeof (text), "interface 3 is %s (class %02x), not a vendor interface",
                       interfaceKind ? "there" : "missing", interfaceKind ? (unsigned) *interfaceKind : 0u);
        setError (Error::WrongDevice, text);
        device.reset();
        return false;
    }

    return true;
}

int XlrUsb::transfer (UsbDevice& device, bool write, uint16_t block, uint8_t* data, uint16_t length)
{
    int result = 0;

    // EBUSY / EAGAIN mean "not now": nothing reached the device, so trying again is safe.
    for (const int backoffMs : { 10, 20, 40, 80, 0 })
    {
        result = device.controlTransfer (write ? kRequestTypeOut : kRequestTypeIn, write ? kRequestSet : kRequestGet, block,
                                         kIndex, data, length, kTimeoutMs);

        if (result != -EBUSY && result != -EAGAIN)
            break;

        if (backoffMs > 0)
            clock.sleepMs (backoffMs);
    }

    return result;
}

// The device-info block (read only) says which firmware API the device speaks. Only API 1.3 / 1.4 has the 34-byte config
// block that this protocol layer knows; any other is left alone, so that a longer block is never half written.
bool XlrUsb::checkApi (UsbDevice& device)
{
    if (apiChecked)
        return true;

    VersionBlock version {};
    const int result = transfer (device, false, kVersionBlock, version.data(), (uint16_t) version.size());

    if (result < 0)
    {
        setError (errorForErrno (-result), "reading the device info block failed: " + errnoText (-result));
        return false;
    }

    if (result < 2)
    {
        setError (Error::UnknownLayout, "the device info block is " + std::to_string (result) + " bytes");
        return false;
    }

    if (! apiIsKnown (version))
    {
        setError (Error::UnknownLayout, "the firmware API is " + apiText (version) + ", not 1.3 or 1.4");
        return false;
    }

    apiChecked = true;
    return true;
}

bool XlrUsb::readConfigBlock (UsbDevice& device, ConfigBlock& out)
{
    ConfigBlock buffer {};
    const int result = transfer (device, false, kConfigBlock, buffer.data(), (uint16_t) buffer.size());

    if (result < 0)
    {
        setError (errorForErrno (-result), "reading the config block failed: " + errnoText (-result));
        return false;
    }

    if ((size_t) result != kConfigSize)
    {
        setError (Error::UnknownLayout, "the config block is " + std::to_string (result) + " bytes, not 34");
        return false;
    }

    out = buffer;
    return true;
}

bool XlrUsb::writeConfigBlock (UsbDevice& device, const ConfigBlock& block)
{
    ConfigBlock buffer = block;
    const int result = transfer (device, true, kConfigBlock, buffer.data(), (uint16_t) buffer.size());

    if (result < 0)
    {
        setError (errorForErrno (-result), "writing the config block failed: " + errnoText (-result));
        return false;
    }

    if ((size_t) result != kConfigSize)
    {
        setError (Error::Io, "the device took " + std::to_string (result) + " of 34 bytes");
        return false;
    }

    return true;
}

void XlrUsb::waitForWriteGap()
{
    if (! lastWriteMs.has_value())
        return;

    const int64_t wait = kMinWriteGapMs - (clock.nowMs() - *lastWriteMs);

    if (wait > 0)
        clock.sleepMs ((int) wait);
}

bool XlrUsb::readConfig (ConfigBlock& out)
{
    const std::lock_guard<std::mutex> lock (mutex);
    setError (Error::None, {});

    std::unique_ptr<UsbDevice> device;

    if (! openVerified (device) || ! checkApi (*device) || ! readConfigBlock (*device, out))
        return false;

    std::string why;

    if (! looksLikeConfig (out, &why))
    {
        setError (Error::UnknownLayout, why + "; block: " + toHex (out));
        return false;
    }

    return true;
}

std::optional<double> XlrUsb::getGainDb()
{
    ConfigBlock block {};

    if (! readConfig (block))
        return std::nullopt;

    return gainDbOf (block);        // looksLikeConfig has checked that it is there
}

bool XlrUsb::setGainDb (double db)
{
    const std::lock_guard<std::mutex> lock (mutex);
    setError (Error::None, {});

    // The gap comes first, so the read and the write that follow it are back to back: the block we write is as fresh as it
    // can be, and a dial turned in between is lost only for a few milliseconds.
    waitForWriteGap();

    std::unique_ptr<UsbDevice> device;

    if (! openVerified (device) || ! checkApi (*device))
        return false;

    ConfigBlock before {};

    if (! readConfigBlock (*device, before))
        return false;

    std::string why;

    if (! looksLikeConfig (before, &why))
    {
        setError (Error::UnknownLayout, why + "; block: " + toHex (before));
        return false;
    }

    const auto current = gainDbOf (before);
    const double wanted = quantiseGainDb (db);

    if (current.has_value() && std::abs (*current - wanted) < kGainStepDb / 2.0)
        return true;        // already so: no write at all

    const ConfigBlock after = withGainDb (before, wanted);
    const bool written = writeConfigBlock (*device, after);
    lastWriteMs = clock.nowMs();

    if (! written)
        return false;

    // Read it back at once, and once more after a moment in case the firmware needs a beat to settle.
    ConfigBlock readBack {};

    for (int attempt = 0; attempt < 2; ++attempt)
    {
        if (attempt > 0)
            clock.sleepMs (100);

        if (! readConfigBlock (*device, readBack))
            return false;

        if (writeWasKept (after, readBack))
            return true;
    }

    setError (Error::NotKept, "wrote " + toHex (after) + " but read back " + toHex (readBack));
    return false;
}
}
