// A fake Wave XLR for the USB tests: a 34-byte config block and a device info block behind the XlrBus / UsbDevice interfaces,
// a clock that only moves when someone sleeps, and a record of every control transfer. Like FakeWaveUsb.h it checks that
// nothing outside the documented protocol is ever asked of it: each such request is counted in `forbidden`, and the tests
// insist on zero.
#pragma once

#include "backend/wave/WaveXlrUsb.h"

#include <cerrno>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hr::test
{
struct FakeXlr
{
    // A plausible block, with distinctive values in the bytes nobody documents and with 48 V phantom power and low impedance
    // switched ON (the two things a careless write must never touch): gain 40 dB, unmuted, headphones at -12 dB.
    wave::xlr::ConfigBlock block { 0x00, 0x28, 0x11, 0x22, 0x00, 0x33, 0x01, 0x44, 0x55, 0x00, 0xf4, 0x66, 0x77, 0x88, 0x02, 0x99,
                                   0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x12, 0x23, 0x34, 0x45, 0x56, 0x67, 0x78, 0x89, 0x9a, 0xab,
                                   0xbc, 0x01 };

    wave::xlr::VersionBlock version = makeVersion (1, 4);

    uint16_t vendor = 0x0fd9, product = 0x007d;
    uint8_t interface3Class = 0xFF;
    bool hasInterface3 = true;
    wave::Presence presence = wave::Presence::Ready;

    // Misbehaviour on demand.
    bool ignoreWrites = false;                              // accepts a write and does nothing with it
    int configReturns = -1;                                 // >= 0: a config read answers with that many bytes
    int versionReturns = -1;                                // >= 0: a device info read answers with that many bytes
    int failWithErrno = 0;                                  // != 0: every transfer fails with this
    std::function<void (wave::xlr::ConfigBlock&)> afterWrite;    // runs after a write was applied (the dial turns, the unit falls back)

    // What happened.
    int configReads = 0, versionReads = 0, writes = 0, forbidden = 0, opens = 0, probes = 0;
    std::vector<int64_t> writeTimesMs;
    int64_t nowMs = 100000;                                 // the fake clock
    std::vector<int> sleeps;

    static wave::xlr::VersionBlock makeVersion (uint8_t major, uint8_t minor)
    {
        wave::xlr::VersionBlock v {};
        v[0] = major;
        v[1] = minor;
        return v;
    }

    wave::WaveUsb::Clock clock()
    {
        wave::WaveUsb::Clock c;
        c.nowMs = [this] { return nowMs; };
        c.sleepMs = [this] (int ms) { sleeps.push_back (ms); nowMs += ms; };
        return c;
    }
};

class FakeXlrDevice final : public wave::UsbDevice
{
public:
    explicit FakeXlrDevice (FakeXlr& x) : xlr (x) {}

    std::vector<uint8_t> readDescriptors() override
    {
        std::vector<uint8_t> d { 18, 1, 0x00, 0x02, 0, 0, 0, 64, (uint8_t) (xlr.vendor & 0xff), (uint8_t) (xlr.vendor >> 8),
                                 (uint8_t) (xlr.product & 0xff), (uint8_t) (xlr.product >> 8), 0, 1, 1, 2, 3, 1 };

        const int interfaces = xlr.hasInterface3 ? 4 : 3;
        const uint16_t total = (uint16_t) (9 + 9 * interfaces);
        d.insert (d.end(), { 9, 2, (uint8_t) (total & 0xff), (uint8_t) (total >> 8), (uint8_t) interfaces, 1, 0, 0x80, 50 });

        for (int i = 0; i < interfaces; ++i)
        {
            const uint8_t kind = i < 3 ? 0x01 : xlr.interface3Class;      // 0 to 2: audio, 3: the vendor interface
            d.insert (d.end(), { 9, 4, (uint8_t) i, 0, 0, kind, (uint8_t) (i == 3 ? 0xF0 : 0x01), 0, 0 });
        }

        return d;
    }

    int controlTransfer (uint8_t requestType, uint8_t request, uint16_t value, uint16_t index,
                         uint8_t* data, uint16_t length, int) override
    {
        const bool isRead = requestType == wave::kRequestTypeIn && request == wave::kRequestGet;
        const bool isWrite = requestType == wave::kRequestTypeOut && request == wave::kRequestSet;
        const bool configRead = isRead && value == wave::kConfigBlock && length == wave::xlr::kConfigSize;
        const bool versionRead = isRead && value == wave::xlr::kVersionBlock && length == wave::xlr::kVersionSize;
        const bool configWrite = isWrite && value == wave::kConfigBlock && length == wave::xlr::kConfigSize;

        if (index != wave::kIndex || ! (configRead || versionRead || configWrite))
        {
            ++xlr.forbidden;
            return -EPIPE;
        }

        if (xlr.failWithErrno != 0)
            return -xlr.failWithErrno;

        if (versionRead)
        {
            ++xlr.versionReads;
            const int count = xlr.versionReturns >= 0 ? xlr.versionReturns : (int) wave::xlr::kVersionSize;

            for (int i = 0; i < count && i < (int) wave::xlr::kVersionSize; ++i)
                data[i] = xlr.version[(size_t) i];

            return count;
        }

        if (configRead)
        {
            ++xlr.configReads;
            const int count = xlr.configReturns >= 0 ? xlr.configReturns : (int) wave::xlr::kConfigSize;

            for (int i = 0; i < count && i < (int) wave::xlr::kConfigSize; ++i)
                data[i] = xlr.block[(size_t) i];

            return count;
        }

        ++xlr.writes;
        xlr.writeTimesMs.push_back (xlr.nowMs);

        if (! xlr.ignoreWrites)
        {
            for (size_t i = 0; i < wave::xlr::kConfigSize; ++i)
                xlr.block[i] = data[i];

            if (xlr.afterWrite)
                xlr.afterWrite (xlr.block);
        }

        return (int) wave::xlr::kConfigSize;
    }

private:
    FakeXlr& xlr;
};

class FakeXlrBus final : public wave::xlr::XlrBus
{
public:
    explicit FakeXlrBus (FakeXlr& x) : xlr (x) {}

    wave::Presence probe() override                     { ++xlr.probes; return xlr.presence; }

    std::unique_ptr<wave::UsbDevice> open (wave::Error& error, std::string& detail) override
    {
        switch (xlr.presence)
        {
            case wave::Presence::Ready:         ++xlr.opens; return std::make_unique<FakeXlrDevice> (xlr);
            case wave::Presence::NoPermission:  error = wave::Error::NoPermission; break;
            case wave::Presence::Ambiguous:     error = wave::Error::WrongDevice; break;
            case wave::Presence::Unsupported:   error = wave::Error::Unsupported; break;
            case wave::Presence::Absent:        error = wave::Error::NotFound; break;
        }

        detail = "fake bus";
        return nullptr;
    }

private:
    FakeXlr& xlr;
};
}
