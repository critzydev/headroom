// WaveUsb: reads and sets the Wave:3's ClipGuard over USB control transfers. Linux only in practice; on
// other platforms every call reports "unsupported". No libusb: the Linux transport is the kernel's usbfs ioctl.
//
// Everything that is sent goes through WaveUsb, and WaveUsb only ever sends what WaveProtocol.h allows: class requests
// to block 0, always the whole 16-byte block. A write is: read the block, change the ClipGuard byte, write the block,
// read it back and check it. If the device is not exactly 0fd9:0070, or the block does not look like the documented
// layout, nothing is written. Writes are at least 250 ms apart. None of this is for the real-time thread.
#pragma once

#include "backend/wave/WaveProtocol.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace hr::wave
{
enum class Presence
{
    Unsupported,        // not Linux
    Absent,             // no Wave:3 on the USB bus
    Ambiguous,          // more than one: Headroom cannot know which one is meant, so it does not touch any
    NoPermission,       // there, but this user may not open it (the udev rule is missing)
    Ready
};

enum class Error
{
    None,
    Unsupported,
    NotFound,
    NoPermission,
    Busy,               // someone else holds the vendor interface
    WrongDevice,        // not a 0fd9:0070 with a vendor interface 3
    UnknownLayout,      // the block is not the 16-byte layout we know
    Io,                 // the transfer failed or timed out
    NotKept             // the write went through but the read-back does not show it
};

// Plain words for a person; the log gets the detail.
std::string describe (Error);

// ---- The device, as seen by WaveUsb ----------------------------------------------------------------------
// One opened device node with the vendor interface claimed. Closing it releases the interface.
class UsbDevice
{
public:
    virtual ~UsbDevice() = default;

    // The raw descriptors of the device that was really opened: device descriptor first, then the configuration.
    virtual std::vector<uint8_t> readDescriptors() = 0;

    // A control transfer on endpoint 0. Returns the number of bytes moved, or -errno.
    virtual int controlTransfer (uint8_t requestType, uint8_t request, uint16_t value, uint16_t index,
                                 uint8_t* data, uint16_t length, int timeoutMs) = 0;
};

class UsbBus
{
public:
    virtual ~UsbBus() = default;

    virtual Presence probe() = 0;                                   // looks only, opens nothing

    // Opens the Wave:3's node read-write and claims interface 3. nullptr, with `error` and `detail` set, if it cannot.
    virtual std::unique_ptr<UsbDevice> open (Error& error, std::string& detail) = 0;
};

// The real bus: usbfs on Linux, a bus that says "unsupported" elsewhere.
std::unique_ptr<UsbBus> createSystemBus();

// ---- Finding the device in sysfs (pure enough to test with a folder of files) ------------------------------
// Every /sys/bus/usb/devices/<x> whose idVendor / idProduct are exactly 0fd9 / 0070, as /dev/bus/usb/BBB/DDD.
std::vector<std::string> findWave3Nodes (const std::filesystem::path& sysfsUsbDevices);
std::string usbNodePath (int bus, int device);

// The same for any exact vendor / product id (findWave3Nodes is this with 0fd9 / 0070). Used by the experimental Wave XLR path.
std::vector<std::string> findUsbNodes (const std::filesystem::path& sysfsUsbDevices, uint16_t vendor, uint16_t product);

// What looking at /sys and the node's permissions says about a device found by findUsbNodes (no USB traffic): nothing found,
// more than one, one that this user may not open, one that may be opened. Unsupported when this is not Linux.
Presence presenceOfNodes (const std::vector<std::string>& nodes);

// Opens one such node read-write and claims interface 3, the vendor interface (class 0xFF, no endpoints, no kernel driver);
// snd-usb-audio's own interfaces are never touched. nullptr, with `error` and `detail` set, if it cannot. Linux only.
std::unique_ptr<UsbDevice> openUsbNode (const std::string& nodePath, Error& error, std::string& detail);

// ---- WaveUsb ---------------------------------------------------------------------------------------------
class WaveUsb
{
public:
    struct Clock
    {
        std::function<int64_t()> nowMs;                             // steady milliseconds
        std::function<void (int)> sleepMs;
    };

    static Clock systemClock();

    explicit WaveUsb (std::unique_ptr<UsbBus> bus = createSystemBus(), Clock clock = systemClock());

    Presence probe();
    bool isAvailable();                                             // a Wave:3 is on the bus, whoever may open it
    bool hasPermission();                                           // ... and this user may

    bool readConfig (ConfigBlock& out);                             // false: see lastError()
    std::optional<bool> getClipGuard();
    bool setClipGuard (bool on);                                    // true also when it already was that way

    Error lastError() const;
    std::string lastErrorText() const;                              // plain words
    std::string lastErrorDetail() const;                            // for the log

private:
    bool openVerified (std::unique_ptr<UsbDevice>& device);
    bool readBlock (UsbDevice& device, ConfigBlock& out);
    bool writeBlock (UsbDevice& device, const ConfigBlock& block);
    void setError (Error error, std::string detail);
    void waitForWriteGap();

    std::unique_ptr<UsbBus> bus;
    Clock clock;

    mutable std::mutex mutex;
    Error error = Error::None;
    std::string errorDetail;
    std::optional<int64_t> lastWriteMs;
};
}
