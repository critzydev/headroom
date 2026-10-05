// XlrUsb: reads and sets the Wave XLR's microphone gain over USB control transfers. EXPERIMENTAL, and only ever
// used when the user has switched it on. The same guards as the Wave:3's WaveUsb, for a different block:
//   * only class requests to two block ids (the 34-byte config block and the read-only 51-byte device info block), nothing else;
//   * the opened device is identified from its own descriptors: exactly 0fd9:007d with a vendor-class interface 3, else nothing is sent;
//   * the firmware API must be 1.3 or 1.4 (the ones whose config block is these 34 bytes), else nothing is written;
//   * a change is: read the block, check it looks like the documented layout, change the gain bytes and nothing else, write
//     the whole block, read it back and check it (48 V phantom power and low impedance must come back exactly as they were);
//   * an already-correct gain is never written; writes are at least 250 ms apart; none of it is for the real-time thread.
#pragma once

#include "backend/wave/WaveUsb.h"
#include "backend/wave/WaveXlrProtocol.h"

#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace hr::wave::xlr
{
// Plain words for a person; the log gets the detail.
std::string describe (Error);

// Opens the Wave XLR's node and claims interface 3.
class XlrBus
{
public:
    virtual ~XlrBus() = default;

    virtual Presence probe() = 0;                                   // looks only, opens nothing
    virtual std::unique_ptr<UsbDevice> open (Error& error, std::string& detail) = 0;
};

// The real bus: usbfs on Linux (the helpers in WaveUsb.h), a bus that says "unsupported" elsewhere.
std::unique_ptr<XlrBus> createSystemXlrBus();

class XlrUsb
{
public:
    explicit XlrUsb (std::unique_ptr<XlrBus> bus = createSystemXlrBus(), WaveUsb::Clock clock = WaveUsb::systemClock());

    Presence probe();

    bool readConfig (ConfigBlock& out);                             // false: see lastError()
    std::optional<double> getGainDb();
    bool setGainDb (double db);                                     // true also when it already was that way

    Error lastError() const;
    std::string lastErrorText() const;                              // plain words
    std::string lastErrorDetail() const;                            // for the log

private:
    bool openVerified (std::unique_ptr<UsbDevice>& device);
    bool checkApi (UsbDevice& device);
    bool readConfigBlock (UsbDevice& device, ConfigBlock& out);
    bool writeConfigBlock (UsbDevice& device, const ConfigBlock& block);
    int transfer (UsbDevice& device, bool write, uint16_t block, uint8_t* data, uint16_t length);
    void setError (Error error, std::string detail);
    void waitForWriteGap();

    std::unique_ptr<XlrBus> bus;
    WaveUsb::Clock clock;

    mutable std::mutex mutex;
    Error error = Error::None;
    std::string errorDetail;
    std::optional<int64_t> lastWriteMs;
    bool apiChecked = false;                                        // the device said API 1.3 / 1.4 since it was last seen
};
}
