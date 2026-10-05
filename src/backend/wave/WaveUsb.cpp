// WaveUsb: the guarded read / read-modify-write of the Wave:3 config block, and the Linux usbfs transport (see WaveUsb.h).
#include "backend/wave/WaveUsb.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>

#if defined (__linux__)
 #include <fcntl.h>
 #include <linux/usbdevice_fs.h>
 #include <sys/ioctl.h>
 #include <unistd.h>
#endif

namespace hr::wave
{
std::string describe (Error error)
{
    switch (error)
    {
        case Error::None:           return {};
        case Error::Unsupported:    return "Hardware controls only work on Linux.";
        case Error::NotFound:       return "The Wave:3 is not connected.";
        case Error::NoPermission:   return "Headroom is not allowed to control the Wave:3 yet.";
        case Error::Busy:           return "Another program is using the Wave:3's controls. Close it and try again.";
        case Error::WrongDevice:    return "That does not look like a Wave:3, so Headroom left it alone.";
        case Error::UnknownLayout:  return "The Wave:3 answered in a way Headroom does not know, so it left it alone.";
        case Error::Io:             return "The Wave:3 did not answer. Unplug it, plug it back in and try again.";
        case Error::NotKept:        return "The Wave:3 did not keep the change.";
    }

    return {};
}

// ---------------------------------------------------------------------------------------------------
// Finding the device
// ---------------------------------------------------------------------------------------------------

namespace
{
    std::string readTrimmed (const std::filesystem::path& file)
    {
        std::ifstream in (file);
        std::string line;
        std::getline (in, line);

        while (! line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
            line.pop_back();

        return line;
    }

    std::string lowerCase (std::string text)
    {
        std::transform (text.begin(), text.end(), text.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
        return text;
    }

    std::optional<int> parseDecimal (const std::string& text)
    {
        if (text.empty() || text.size() > 6)
            return std::nullopt;

        int value = 0;

        for (const char c : text)
        {
            if (c < '0' || c > '9')
                return std::nullopt;

            value = value * 10 + (c - '0');
        }

        return value;
    }

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
}

std::string usbNodePath (int bus, int device)
{
    char path[48];
    std::snprintf (path, sizeof (path), "/dev/bus/usb/%03d/%03d", bus, device);
    return path;
}

std::vector<std::string> findWave3Nodes (const std::filesystem::path& sysfsUsbDevices)
{
    return findUsbNodes (sysfsUsbDevices, 0x0fd9, 0x0070);
}

std::vector<std::string> findUsbNodes (const std::filesystem::path& sysfsUsbDevices, uint16_t vendor, uint16_t product)
{
    char wantedVendor[8], wantedProduct[8];
    std::snprintf (wantedVendor, sizeof (wantedVendor), "%04x", (unsigned) vendor);
    std::snprintf (wantedProduct, sizeof (wantedProduct), "%04x", (unsigned) product);

    std::vector<std::string> nodes;
    std::error_code ec;

    for (std::filesystem::directory_iterator it (sysfsUsbDevices, ec), end; ! ec && it != end; it.increment (ec))
    {
        const auto& dir = it->path();

        if (dir.filename().string().find (':') != std::string::npos)
            continue;       // an interface ("1-2:1.0"), not a device

        // Exactly the one product (the Wave:3: not its firmware-update mode 0071, not the other Waves, not a look-alike).
        if (lowerCase (readTrimmed (dir / "idVendor")) != wantedVendor || lowerCase (readTrimmed (dir / "idProduct")) != wantedProduct)
            continue;

        const auto bus = parseDecimal (readTrimmed (dir / "busnum"));
        const auto device = parseDecimal (readTrimmed (dir / "devnum"));

        if (bus.has_value() && device.has_value())
            nodes.push_back (usbNodePath (*bus, *device));
    }

    std::sort (nodes.begin(), nodes.end());
    return nodes;
}

// ---------------------------------------------------------------------------------------------------
// The system bus
// ---------------------------------------------------------------------------------------------------

namespace
{
#if defined (__linux__)
    class LinuxUsbDevice final : public UsbDevice
    {
    public:
        explicit LinuxUsbDevice (int fileDescriptor) : fd (fileDescriptor) {}

        ~LinuxUsbDevice() override
        {
            if (claimed)
            {
                unsigned int number = kInterface;
                ::ioctl (fd, USBDEVFS_RELEASEINTERFACE, &number);
            }

            ::close (fd);
        }

        // Interface 3 is the vendor interface with no driver; claiming it ourselves keeps the kernel from logging
        // "did not claim interface before use", and tells us if another program has it. snd-usb-audio's interfaces
        // (0 to 2) are never touched.
        bool claim (int& errorCode)
        {
            unsigned int number = kInterface;

            if (::ioctl (fd, USBDEVFS_CLAIMINTERFACE, &number) < 0)
            {
                errorCode = errno;
                return false;
            }

            claimed = true;
            return true;
        }

        std::vector<uint8_t> readDescriptors() override
        {
            // Reading a usbfs node returns its descriptors: the device descriptor, then the configuration.
            std::vector<uint8_t> data (4096);
            ::lseek (fd, 0, SEEK_SET);
            const auto count = ::read (fd, data.data(), data.size());
            data.resize (count > 0 ? (size_t) count : 0);
            return data;
        }

        int controlTransfer (uint8_t requestType, uint8_t request, uint16_t value, uint16_t index,
                             uint8_t* data, uint16_t length, int timeoutMs) override
        {
            usbdevfs_ctrltransfer transfer {};
            transfer.bRequestType = requestType;
            transfer.bRequest = request;
            transfer.wValue = value;
            transfer.wIndex = index;
            transfer.wLength = length;
            transfer.timeout = (uint32_t) timeoutMs;
            transfer.data = data;

            for (int attempt = 0; attempt < 5; ++attempt)
            {
                const int result = ::ioctl (fd, USBDEVFS_CONTROL, &transfer);

                if (result >= 0)
                    return result;

                if (errno != EINTR)
                    return -errno;
            }

            return -EINTR;
        }

    private:
        int fd;
        bool claimed = false;
    };

    class LinuxUsbBus final : public UsbBus
    {
    public:
        Presence probe() override
        {
            const auto nodes = findWave3Nodes (kSysfs);

            if (nodes.empty())
                return Presence::Absent;

            if (nodes.size() > 1)
                return Presence::Ambiguous;

            return ::access (nodes.front().c_str(), R_OK | W_OK) == 0 ? Presence::Ready : Presence::NoPermission;
        }

        std::unique_ptr<UsbDevice> open (Error& error, std::string& detail) override
        {
            const auto nodes = findWave3Nodes (kSysfs);

            if (nodes.empty())
            {
                error = Error::NotFound;
                detail = "no 0fd9:0070 in " + std::string (kSysfs);
                return nullptr;
            }

            if (nodes.size() > 1)
            {
                error = Error::WrongDevice;
                detail = "more than one 0fd9:0070 is connected";
                return nullptr;
            }

            const int fd = ::open (nodes.front().c_str(), O_RDWR | O_CLOEXEC);

            if (fd < 0)
            {
                const int code = errno;
                error = errorForErrno (code);
                detail = "open " + nodes.front() + ": " + errnoText (code);
                return nullptr;
            }

            auto device = std::make_unique<LinuxUsbDevice> (fd);
            int code = 0;

            if (! device->claim (code))
            {
                error = errorForErrno (code);
                detail = "claim interface 3 of " + nodes.front() + ": " + errnoText (code);
                return nullptr;
            }

            return device;
        }

    private:
        static constexpr const char* kSysfs = "/sys/bus/usb/devices";
    };
#else
    class UnsupportedBus final : public UsbBus
    {
    public:
        Presence probe() override                                                   { return Presence::Unsupported; }

        std::unique_ptr<UsbDevice> open (Error& error, std::string& detail) override
        {
            error = Error::Unsupported;
            detail = "no USB access on this platform";
            return nullptr;
        }
    };
#endif
}

Presence presenceOfNodes (const std::vector<std::string>& nodes)
{
   #if defined (__linux__)
    if (nodes.empty())
        return Presence::Absent;

    if (nodes.size() > 1)
        return Presence::Ambiguous;

    return ::access (nodes.front().c_str(), R_OK | W_OK) == 0 ? Presence::Ready : Presence::NoPermission;
   #else
    (void) nodes;
    return Presence::Unsupported;
   #endif
}

std::unique_ptr<UsbDevice> openUsbNode (const std::string& nodePath, Error& error, std::string& detail)
{
   #if defined (__linux__)
    const int fd = ::open (nodePath.c_str(), O_RDWR | O_CLOEXEC);

    if (fd < 0)
    {
        const int code = errno;
        error = errorForErrno (code);
        detail = "open " + nodePath + ": " + errnoText (code);
        return nullptr;
    }

    auto device = std::make_unique<LinuxUsbDevice> (fd);
    int code = 0;

    if (! device->claim (code))
    {
        error = errorForErrno (code);
        detail = "claim interface 3 of " + nodePath + ": " + errnoText (code);
        return nullptr;
    }

    return device;
   #else
    (void) nodePath;
    error = Error::Unsupported;
    detail = "no USB access on this platform";
    return nullptr;
   #endif
}

std::unique_ptr<UsbBus> createSystemBus()
{
   #if defined (__linux__)
    return std::make_unique<LinuxUsbBus>();
   #else
    return std::make_unique<UnsupportedBus>();
   #endif
}

// ---------------------------------------------------------------------------------------------------
// WaveUsb
// ---------------------------------------------------------------------------------------------------

WaveUsb::Clock WaveUsb::systemClock()
{
    Clock clock;
    clock.nowMs = []
    {
        using namespace std::chrono;
        return (int64_t) duration_cast<milliseconds> (steady_clock::now().time_since_epoch()).count();
    };
    clock.sleepMs = [] (int ms) { std::this_thread::sleep_for (std::chrono::milliseconds (ms)); };
    return clock;
}

WaveUsb::WaveUsb (std::unique_ptr<UsbBus> busToUse, Clock clockToUse) : bus (std::move (busToUse)), clock (std::move (clockToUse)) {}

Presence WaveUsb::probe()
{
    const std::lock_guard<std::mutex> lock (mutex);
    return bus->probe();
}

bool WaveUsb::isAvailable()
{
    const auto presence = probe();
    return presence == Presence::NoPermission || presence == Presence::Ready;
}

bool WaveUsb::hasPermission()
{
    return probe() == Presence::Ready;
}

Error WaveUsb::lastError() const
{
    const std::lock_guard<std::mutex> lock (mutex);
    return error;
}

std::string WaveUsb::lastErrorText() const
{
    const std::lock_guard<std::mutex> lock (mutex);
    return describe (error);
}

std::string WaveUsb::lastErrorDetail() const
{
    const std::lock_guard<std::mutex> lock (mutex);
    return errorDetail;
}

void WaveUsb::setError (Error newError, std::string detail)
{
    error = newError;
    errorDetail = std::move (detail);
}

// Opens the node and checks, from the descriptors of the device that really got opened (not from what sysfs said a
// moment ago), that it is exactly a Wave:3 with a vendor-specific interface 3 (the one wIndex 0x3303 points at).
bool WaveUsb::openVerified (std::unique_ptr<UsbDevice>& device)
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

    if (! ids.has_value() || ! ids->isWave3())
    {
        char text[80];
        std::snprintf (text, sizeof (text), "the opened device is %04x:%04x, not 0fd9:0070", ids ? ids->vendor : 0, ids ? ids->product : 0);
        setError (Error::WrongDevice, text);
        device.reset();
        return false;
    }

    const auto interfaceKind = interfaceClass (descriptors.data(), descriptors.size(), kInterface);

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

bool WaveUsb::readBlock (UsbDevice& device, ConfigBlock& out)
{
    ConfigBlock buffer {};
    int result = 0;

    // EBUSY / EAGAIN mean "not now": nothing reached the device, so trying again is safe.
    for (const int backoffMs : { 10, 20, 40, 80, 0 })
    {
        result = device.controlTransfer (kRequestTypeIn, kRequestGet, kConfigBlock, kIndex, buffer.data(),
                                         (uint16_t) buffer.size(), kTimeoutMs);

        if (result != -EBUSY && result != -EAGAIN)
            break;

        if (backoffMs > 0)
            clock.sleepMs (backoffMs);
    }

    if (result < 0)
    {
        setError (errorForErrno (-result), "reading the config block failed: " + errnoText (-result));
        return false;
    }

    if ((size_t) result != kConfigSize)
    {
        setError (Error::UnknownLayout, "the config block is " + std::to_string (result) + " bytes, not 16");
        return false;
    }

    out = buffer;
    return true;
}

bool WaveUsb::writeBlock (UsbDevice& device, const ConfigBlock& block)
{
    ConfigBlock buffer = block;
    int result = 0;

    for (const int backoffMs : { 10, 20, 40, 80, 0 })
    {
        result = device.controlTransfer (kRequestTypeOut, kRequestSet, kConfigBlock, kIndex, buffer.data(),
                                         (uint16_t) buffer.size(), kTimeoutMs);

        if (result != -EBUSY && result != -EAGAIN)
            break;

        if (backoffMs > 0)
            clock.sleepMs (backoffMs);
    }

    if (result < 0)
    {
        setError (errorForErrno (-result), "writing the config block failed: " + errnoText (-result));
        return false;
    }

    if ((size_t) result != kConfigSize)
    {
        setError (Error::Io, "the device took " + std::to_string (result) + " of 16 bytes");
        return false;
    }

    return true;
}

void WaveUsb::waitForWriteGap()
{
    if (! lastWriteMs.has_value())
        return;

    const int64_t wait = kMinWriteGapMs - (clock.nowMs() - *lastWriteMs);

    if (wait > 0)
        clock.sleepMs ((int) wait);
}

bool WaveUsb::readConfig (ConfigBlock& out)
{
    const std::lock_guard<std::mutex> lock (mutex);
    setError (Error::None, {});

    std::unique_ptr<UsbDevice> device;
    return openVerified (device) && readBlock (*device, out);
}

std::optional<bool> WaveUsb::getClipGuard()
{
    ConfigBlock block {};

    if (! readConfig (block))
        return std::nullopt;

    const auto value = clipGuardOf (block);

    if (! value.has_value())
    {
        const std::lock_guard<std::mutex> lock (mutex);
        setError (Error::UnknownLayout, "the ClipGuard byte is " + std::to_string ((int) block[kClipGuardOffset]) + ", not 0 or 1; block: " + toHex (block));
    }

    return value;
}

bool WaveUsb::setClipGuard (bool on)
{
    const std::lock_guard<std::mutex> lock (mutex);
    setError (Error::None, {});

    // The gap comes first, so the read and the write that follow it are back to back: the block we write is as fresh
    // as it can be, and a dial turned in between is lost only for a few milliseconds.
    waitForWriteGap();

    std::unique_ptr<UsbDevice> device;

    if (! openVerified (device))
        return false;

    ConfigBlock before {};

    if (! readBlock (*device, before))
        return false;

    std::string why;

    if (! looksLikeConfig (before, &why))
    {
        setError (Error::UnknownLayout, why + "; block: " + toHex (before));
        return false;
    }

    if (clipGuardOf (before) == std::optional<bool> (on))
        return true;        // already so: no write at all

    const ConfigBlock after = withClipGuard (before, on);
    const bool written = writeBlock (*device, after);
    lastWriteMs = clock.nowMs();

    if (! written)
        return false;

    // Read it back at once, and once more after a moment in case the firmware needs a beat to settle.
    ConfigBlock readBack {};

    for (int attempt = 0; attempt < 2; ++attempt)
    {
        if (attempt > 0)
            clock.sleepMs (100);

        if (! readBlock (*device, readBack))
            return false;

        if (writeWasKept (after, readBack))
            return true;
    }

    setError (Error::NotKept, "wrote " + toHex (after) + " but read back " + toHex (readBack));
    return false;
}
}
