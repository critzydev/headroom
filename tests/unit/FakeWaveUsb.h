// A fake Wave:3 for the USB tests: a 16-byte config block behind the UsbBus / UsbDevice interfaces, a clock that only moves
// when someone sleeps, and a record of every control transfer. It also checks that nothing outside the documented protocol
// is ever asked of it: each such request is counted in `forbidden`, and the tests insist on zero.
#pragma once

#include "backend/wave/WaveUsb.h"

#include <cerrno>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hr::test
{
struct FakeWave
{
    // What Elgato's own software reads from a Wave:3 that Wave Link had been using (Kailas Mahavarkar's live dump),
    // plus distinctive values in the bytes nobody documents, so that a careless write is noticed.
    wave::ConfigBlock block { 0x00, 0x28, 0x11, 0x22, 0x00, 0x01, 0x01, 0x80, 0xe8, 0x00, 0x00, 0x00, 0x01, 0x33, 0x44, 0x01 };

    uint16_t vendor = 0x0fd9, product = 0x0070;
    uint8_t interface3Class = 0xFF;
    bool hasInterface3 = true;
    wave::Presence presence = wave::Presence::Ready;

    // Misbehaviour on demand.
    bool ignoreWrites = false;                              // accepts a write and does nothing with it
    int readReturns = -1;                                   // >= 0: a read answers with that many bytes
    int failWithErrno = 0;                                  // != 0: every transfer fails with this
    std::function<void (wave::ConfigBlock&)> afterWrite;    // runs after a write was applied (the dial turns, the mic falls back)

    // What happened.
    int reads = 0, writes = 0, forbidden = 0, opens = 0;
    std::vector<int64_t> writeTimesMs;
    int64_t nowMs = 100000;                                 // the fake clock
    std::vector<int> sleeps;

    wave::WaveUsb::Clock clock()
    {
        wave::WaveUsb::Clock c;
        c.nowMs = [this] { return nowMs; };
        c.sleepMs = [this] (int ms) { sleeps.push_back (ms); nowMs += ms; };
        return c;
    }
};

class FakeDevice final : public wave::UsbDevice
{
public:
    explicit FakeDevice (FakeWave& w) : wave (w) {}

    std::vector<uint8_t> readDescriptors() override
    {
        std::vector<uint8_t> d { 18, 1, 0x00, 0x02, 0, 0, 0, 64, (uint8_t) (wave.vendor & 0xff), (uint8_t) (wave.vendor >> 8),
                                 (uint8_t) (wave.product & 0xff), (uint8_t) (wave.product >> 8), 0, 1, 1, 2, 3, 1 };

        const int interfaces = wave.hasInterface3 ? 4 : 3;
        const uint16_t total = (uint16_t) (9 + 9 * interfaces);
        d.insert (d.end(), { 9, 2, (uint8_t) (total & 0xff), (uint8_t) (total >> 8), (uint8_t) interfaces, 1, 0, 0x80, 50 });

        for (int i = 0; i < interfaces; ++i)
        {
            const uint8_t kind = i < 3 ? 0x01 : wave.interface3Class;      // 0 to 2: audio, 3: the vendor interface
            d.insert (d.end(), { 9, 4, (uint8_t) i, 0, 0, kind, (uint8_t) (i == 3 ? 0xF0 : 0x01), 0, 0 });
        }

        return d;
    }

    int controlTransfer (uint8_t requestType, uint8_t request, uint16_t value, uint16_t index,
                         uint8_t* data, uint16_t length, int) override
    {
        const bool isRead = requestType == wave::kRequestTypeIn && request == wave::kRequestGet;
        const bool isWrite = requestType == wave::kRequestTypeOut && request == wave::kRequestSet;

        if ((! isRead && ! isWrite) || value != wave::kConfigBlock || index != wave::kIndex || length != wave::kConfigSize)
        {
            ++wave.forbidden;
            return -EPIPE;
        }

        if (wave.failWithErrno != 0)
            return -wave.failWithErrno;

        if (isRead)
        {
            ++wave.reads;
            const int count = wave.readReturns >= 0 ? wave.readReturns : (int) wave::kConfigSize;

            for (int i = 0; i < count && i < (int) wave::kConfigSize; ++i)
                data[i] = wave.block[(size_t) i];

            return count;
        }

        ++wave.writes;
        wave.writeTimesMs.push_back (wave.nowMs);

        if (! wave.ignoreWrites)
        {
            for (size_t i = 0; i < wave::kConfigSize; ++i)
                wave.block[i] = data[i];

            if (wave.afterWrite)
                wave.afterWrite (wave.block);
        }

        return (int) wave::kConfigSize;
    }

private:
    FakeWave& wave;
};

class FakeBus final : public wave::UsbBus
{
public:
    explicit FakeBus (FakeWave& w) : wave (w) {}

    wave::Presence probe() override                     { return wave.presence; }

    std::unique_ptr<wave::UsbDevice> open (wave::Error& error, std::string& detail) override
    {
        switch (wave.presence)
        {
            case wave::Presence::Ready:         ++wave.opens; return std::make_unique<FakeDevice> (wave);
            case wave::Presence::NoPermission:  error = wave::Error::NoPermission; break;
            case wave::Presence::Ambiguous:     error = wave::Error::WrongDevice; break;
            case wave::Presence::Unsupported:   error = wave::Error::Unsupported; break;
            case wave::Presence::Absent:        error = wave::Error::NotFound; break;
        }

        detail = "fake bus";
        return nullptr;
    }

private:
    FakeWave& wave;
};
}
