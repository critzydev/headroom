// Unit tests for the experimental Wave XLR USB side: the protocol helpers, the guarded read-modify-write against a
// fake device, the experimental udev rule, the worker that is off until the user opts in, and the gain slider's maths. The
// safety rules are the point: nothing outside the documented requests, nothing written to a layout we do not know, and 48 V
// phantom power and low impedance left exactly as they were. None of it touches a real USB device.
#include "Juce.h"

#include "FakeWaveXlr.h"
#include "backend/ElgatoModels.h"
#include "backend/wave/WaveGain.h"
#include "backend/wave/WavePermission.h"
#include "backend/wave/WaveXlrHardware.h"
#include "backend/wave/WaveXlrProtocol.h"
#include "backend/wave/WaveXlrUsb.h"

#include <cmath>
#include <functional>
#include <string>
#include <utility>

namespace hr
{
namespace
{
    namespace xlr = wave::xlr;

    std::unique_ptr<xlr::XlrUsb> makeUsb (test::FakeXlr& fake)
    {
        return std::make_unique<xlr::XlrUsb> (std::make_unique<test::FakeXlrBus> (fake), fake.clock());
    }

    constexpr size_t kPhantom = xlr::kPhantomOffset, kLowZ = xlr::kLowImpedanceOffset;
}

class WaveXlrTests final : public juce::UnitTest
{
public:
    WaveXlrTests() : juce::UnitTest ("Wave XLR USB (experimental)", "headroom") {}

    void runTest() override
    {
        protocolTests();
        safetyTests();
        readModifyWriteTests();
        permissionTests();
        workerTests();
        gainMathTests();
    }

private:
    // ---- The block ---------------------------------------------------------------------------------------------
    void protocolTests()
    {
        beginTest ("the documented request: the Wave:3's class dialect, wIndex 0x3303, two block ids, 34 and 51 bytes");
        {
            expectEquals ((int) wave::kRequestTypeIn, 0xA1);
            expectEquals ((int) wave::kRequestTypeOut, 0x21);
            expectEquals ((int) wave::kRequestGet, 0x85);
            expectEquals ((int) wave::kRequestSet, 0x05);
            expectEquals ((int) wave::kIndex, 0x3303);
            expectEquals ((int) wave::kConfigBlock, 0);
            expectEquals ((int) xlr::kVersionBlock, 0x000A);
            expectEquals ((int) xlr::kConfigSize, 34);
            expectEquals ((int) xlr::kVersionSize, 51);
            expectEquals ((int) xlr::kProductId, 0x007d);
            expectEquals ((int) xlr::kProductId, (int) elgato::pidsWith (elgato::Hardware::Experimental).front());
        }

        beginTest ("a block parses into its fields");
        {
            test::FakeXlr fake;
            expectEquals (*xlr::gainDbOf (fake.block), 40.0);
            expect (xlr::muteOf (fake.block) == std::optional<bool> (false));
            expect (xlr::looksLikeConfig (fake.block));

            auto block = fake.block;
            block[0] = 0x80;                // 40.5 dB
            expectEquals (*xlr::gainDbOf (block), 40.5);
            block[0] = 0x00;
            block[1] = 0x4B;                // 75 dB, the top
            expectEquals (*xlr::gainDbOf (block), 75.0);
            block[1] = 0x4C;                // above it
            expect (! xlr::gainDbOf (block).has_value());
            block[1] = 0xFF;                // negative
            expect (! xlr::gainDbOf (block).has_value());
        }

        beginTest ("a block that does not look like the layout is recognised");
        {
            test::FakeXlr fake;
            std::string why;

            const auto bad = [&] (const std::function<void (xlr::ConfigBlock&)>& change, const char* expectedWhy)
            {
                auto block = fake.block;
                change (block);
                why.clear();
                expect (! xlr::looksLikeConfig (block, &why), expectedWhy);
                expect (why.find (expectedWhy) != std::string::npos, why);
            };

            bad ([] (xlr::ConfigBlock& b) { b[1] = 0x50; }, "gain");                      // 80 dB: Elgato's limit is 75
            bad ([] (xlr::ConfigBlock& b) { b[xlr::kMuteOffset] = 2; }, "mute");
            bad ([] (xlr::ConfigBlock& b) { b[kPhantom] = 2; }, "phantom");
            bad ([] (xlr::ConfigBlock& b) { b[kLowZ] = 5; }, "low impedance");
            bad ([] (xlr::ConfigBlock& b) { b[xlr::kHeadphoneVolumeOffset] = 0x80; b[xlr::kHeadphoneVolumeOffset + 1] = 0x01; }, "headphone");   // +1.5 dB
        }

        beginTest ("only firmware API 1.3 and 1.4 are known: the ones whose config block is these 34 bytes");
        {
            expect (xlr::apiIsKnown (test::FakeXlr::makeVersion (1, 3)));
            expect (xlr::apiIsKnown (test::FakeXlr::makeVersion (1, 4)));

            const int unknown[][2] = { { 1, 2 }, { 1, 7 }, { 1, 0 }, { 2, 3 }, { 0, 4 }, { 255, 255 } };

            for (const auto& version : unknown)
                expect (! xlr::apiIsKnown (test::FakeXlr::makeVersion ((uint8_t) version[0], (uint8_t) version[1])),
                        std::to_string (version[0]) + "." + std::to_string (version[1]));

            expectEquals (juce::String (xlr::apiText (test::FakeXlr::makeVersion (1, 4))), juce::String ("1.4"));
        }

        beginTest ("the gain is quantised to 0.5 dB and kept between 0 and 75, and nothing but the two gain bytes changes");
        {
            test::FakeXlr fake;
            const auto before = fake.block;

            expectEquals (xlr::quantiseGainDb (40.3), 40.5);
            expectEquals (xlr::quantiseGainDb (40.2), 40.0);
            expectEquals (xlr::quantiseGainDb (-5.0), 0.0);
            expectEquals (xlr::quantiseGainDb (90.0), 75.0);

            for (const double db : { 0.0, 12.5, 40.0, 63.5, 75.0, 90.0, -3.0 })
            {
                const auto after = xlr::withGainDb (before, db);
                expectEquals (*xlr::gainDbOf (after), xlr::quantiseGainDb (db));

                for (size_t i = 2; i < xlr::kConfigSize; ++i)
                    expectEquals ((int) after[i], (int) before[i], "byte " + juce::String ((int) i));

                expect (xlr::looksLikeConfig (after));
            }
        }

        beginTest ("a written block that is read back: what must match and what may move");
        {
            test::FakeXlr fake;
            const auto written = xlr::withGainDb (fake.block, 30.0);

            expect (xlr::writeWasKept (written, written));

            // The unit's own controls: the dial moved the gain by a step, the mute pad was touched, the headphone knob turned.
            auto moved = written;
            moved[0] = 0x80;
            moved[1] = 0x1E;                                    // 30.5 dB: one step
            moved[xlr::kMuteOffset] = 1;
            moved[xlr::kHeadphoneVolumeOffset] = 0x00;
            moved[xlr::kHeadphoneVolumeOffset + 1] = 0xe8;
            moved[14] = 3;
            expect (xlr::writeWasKept (written, moved));

            auto farGain = written;
            farGain[1] = 0x28;                                  // the unit still says 40 dB
            expect (! xlr::writeWasKept (written, farGain));

            for (const size_t offset : { (size_t) 2, (size_t) 5, kPhantom, (size_t) 7, (size_t) 12, (size_t) 20, kLowZ })
            {
                auto changed = written;
                changed[offset] ^= 0x01;
                expect (! xlr::writeWasKept (written, changed), "byte " + juce::String ((int) offset));
            }
        }
    }

    // ---- Safety ------------------------------------------------------------------------------------------------
    void safetyTests()
    {
        beginTest ("anything that is not exactly a 0fd9:007d is left alone: not one transfer");
        {
            const int lookalikes[][2] = { { 0x0fd9, 0x0070 },         // Wave:3
                                          { 0x0fd9, 0x006e },         // Wave:1
                                          { 0x0fd9, 0x00a6 },         // XLR Dock
                                          { 0x0fd9, 0x00b6 },         // Wave XLR MK.2
                                          { 0x0fd9, 0x00b4 },         // Wave XLR Pro
                                          { 0x1234, 0x007d } };

            for (const auto& ids : lookalikes)
            {
                test::FakeXlr fake;
                fake.vendor = (uint16_t) ids[0];
                fake.product = (uint16_t) ids[1];
                auto usb = makeUsb (fake);

                xlr::ConfigBlock block {};
                expect (! usb->readConfig (block));
                expect (usb->lastError() == wave::Error::WrongDevice);
                expect (! usb->setGainDb (30.0));
                expect (! usb->getGainDb().has_value());
                expectEquals (fake.configReads + fake.versionReads + fake.writes + fake.forbidden, 0);
            }
        }

        beginTest ("a Wave XLR without a vendor interface 3 is left alone");
        {
            test::FakeXlr fake;
            fake.hasInterface3 = false;
            auto usb = makeUsb (fake);
            expect (! usb->setGainDb (30.0));
            expect (usb->lastError() == wave::Error::WrongDevice);
            expectEquals (fake.configReads + fake.versionReads + fake.writes, 0);

            fake.hasInterface3 = true;
            fake.interface3Class = 0x01;        // interface 3 is audio here: wIndex 0x3303 would point at the wrong thing
            expect (! usb->setGainDb (30.0));
            expectEquals (fake.configReads + fake.versionReads + fake.writes, 0);
        }

        beginTest ("a firmware API that is not 1.3 or 1.4 is never written to, and its config block is not even read");
        {
            const int unknown[][2] = { { 1, 2 }, { 1, 7 }, { 2, 0 } };

            for (const auto& version : unknown)
            {
                test::FakeXlr fake;
                fake.version = test::FakeXlr::makeVersion ((uint8_t) version[0], (uint8_t) version[1]);
                auto usb = makeUsb (fake);

                xlr::ConfigBlock block {};
                expect (! usb->readConfig (block));
                expect (! usb->setGainDb (30.0));
                expect (usb->lastError() == wave::Error::UnknownLayout);
                expect (usb->lastErrorDetail().find ("API") != std::string::npos, usb->lastErrorDetail());
                expectEquals (fake.writes, 0);
                expectEquals (fake.configReads, 0);
                expectEquals (fake.forbidden, 0);
            }

            test::FakeXlr tiny;
            tiny.versionReturns = 1;                // not even the two bytes that name the API
            auto usb = makeUsb (tiny);
            expect (! usb->setGainDb (30.0));
            expectEquals (tiny.writes, 0);
        }

        beginTest ("a block that is not the known layout is not written");
        {
            test::FakeXlr fake;
            fake.block[kPhantom] = 7;
            auto usb = makeUsb (fake);
            expect (! usb->setGainDb (30.0));
            expect (usb->lastError() == wave::Error::UnknownLayout);
            expectEquals (fake.writes, 0);
            expect (! usb->getGainDb().has_value());

            test::FakeXlr shortBlock;               // API 1.2 has a 29-byte block
            shortBlock.configReturns = 29;
            auto usb2 = makeUsb (shortBlock);
            expect (! usb2->setGainDb (30.0));
            expect (usb2->lastError() == wave::Error::UnknownLayout);
            expectEquals (shortBlock.writes, 0);
        }

        beginTest ("a device that ignores the write is reported, not believed");
        {
            test::FakeXlr fake;
            fake.ignoreWrites = true;
            auto usb = makeUsb (fake);

            expect (! usb->setGainDb (30.0));
            expect (usb->lastError() == wave::Error::NotKept);
            expectEquals (fake.writes, 1);
            expect (! usb->lastErrorText().empty());
            expect (usb->lastErrorDetail().find ("read back") != std::string::npos);
        }

        beginTest ("a unit that switches 48 V phantom power or low impedance by itself after our write is reported");
        {
            for (const size_t offset : { kPhantom, kLowZ })
            {
                test::FakeXlr fake;
                fake.afterWrite = [offset] (xlr::ConfigBlock& block) { block[offset] ^= 0x01; };
                auto usb = makeUsb (fake);

                expect (! usb->setGainDb (30.0));
                expect (usb->lastError() == wave::Error::NotKept);
            }
        }

        beginTest ("no device, no permission, a failing device and a bus that cannot do it");
        {
            test::FakeXlr fake;
            fake.presence = wave::Presence::Absent;
            auto usb = makeUsb (fake);
            xlr::ConfigBlock block {};
            expect (usb->probe() == wave::Presence::Absent);
            expect (! usb->readConfig (block));
            expect (usb->lastError() == wave::Error::NotFound);

            fake.presence = wave::Presence::NoPermission;
            expect (! usb->setGainDb (30.0));
            expect (usb->lastError() == wave::Error::NoPermission);

            fake.presence = wave::Presence::Ambiguous;
            expect (! usb->setGainDb (30.0));
            expect (usb->lastError() == wave::Error::WrongDevice);      // two Wave XLRs: which one is meant is not known

            fake.presence = wave::Presence::Ready;
            fake.failWithErrno = EBUSY;
            expect (! usb->setGainDb (30.0));
            expect (usb->lastError() == wave::Error::Busy);
            expect (! fake.sleeps.empty());         // it backed off and tried again before giving up
            fake.sleeps.clear();

            fake.failWithErrno = EIO;
            expect (! usb->setGainDb (30.0));
            expect (usb->lastError() == wave::Error::Io);
            expect (fake.sleeps.empty());           // a real failure is not retried
            expectEquals (fake.writes, 0);

            for (const auto error : { wave::Error::Unsupported, wave::Error::NotFound, wave::Error::NoPermission, wave::Error::Busy,
                                      wave::Error::WrongDevice, wave::Error::UnknownLayout, wave::Error::Io, wave::Error::NotKept })
                expect (xlr::describe (error).find ("Wave XLR") != std::string::npos || error == wave::Error::Unsupported);

#if ! JUCE_LINUX
            xlr::XlrUsb system;
            expect (system.probe() == wave::Presence::Unsupported);
            expect (! system.setGainDb (30.0));
            expect (system.lastError() == wave::Error::Unsupported);
#endif
        }
    }

    // ---- The guarded write -------------------------------------------------------------------------------------
    void readModifyWriteTests()
    {
        beginTest ("reading the gain: the device info block first (once), then the config block, no write");
        {
            test::FakeXlr fake;
            auto usb = makeUsb (fake);

            xlr::ConfigBlock block {};
            expect (usb->readConfig (block));
            expect (block == fake.block);
            expectEquals (fake.versionReads, 1);
            expectEquals (fake.configReads, 1);
            expect (usb->getGainDb() == std::optional<double> (40.0));
            expectEquals (fake.versionReads, 1);        // the API was said once and is remembered until the unit goes away
            expectEquals (fake.writes, 0);
            expectEquals (fake.forbidden, 0);

            fake.presence = wave::Presence::Absent;
            expect (usb->probe() == wave::Presence::Absent);
            fake.presence = wave::Presence::Ready;
            expect (usb->getGainDb().has_value());
            expectEquals (fake.versionReads, 2);        // a unit that came back is asked again
        }

        beginTest ("a change is a read-modify-write of the whole block that touches only the two gain bytes");
        {
            test::FakeXlr fake;
            const auto before = fake.block;
            auto usb = makeUsb (fake);

            expect (usb->setGainDb (30.0));
            expectEquals (fake.writes, 1);
            expect (fake.configReads >= 2);                 // the read before, and the read-back after
            expect (fake.block == xlr::withGainDb (before, 30.0));
            expect (usb->getGainDb() == std::optional<double> (30.0));

            for (size_t i = 2; i < xlr::kConfigSize; ++i)
                expectEquals ((int) fake.block[i], (int) before[i], "byte " + juce::String ((int) i));

            // 48 V phantom power and low impedance were on, and still are.
            expectEquals ((int) fake.block[kPhantom], 1);
            expectEquals ((int) fake.block[kLowZ], 1);

            expect (usb->setGainDb (40.0));
            expect (fake.block == before);                  // and back: nothing else moved on the way
            expectEquals (fake.forbidden, 0);
        }

        beginTest ("it is already so: no write at all");
        {
            test::FakeXlr fake;
            auto usb = makeUsb (fake);

            expect (usb->setGainDb (40.0));
            expect (usb->setGainDb (40.1));                 // quantises to the same step
            expectEquals (fake.writes, 0);
            expectEquals (fake.forbidden, 0);
        }

        beginTest ("the gain is clamped and quantised before it is written");
        {
            test::FakeXlr fake;
            auto usb = makeUsb (fake);

            expect (usb->setGainDb (99.0));
            expect (usb->getGainDb() == std::optional<double> (75.0));
            expect (usb->setGainDb (-4.0));
            expect (usb->getGainDb() == std::optional<double> (0.0));
            expect (usb->setGainDb (20.3));
            expect (usb->getGainDb() == std::optional<double> (20.5));
        }

        beginTest ("a dial turned between the write and the read-back is not a failure");
        {
            test::FakeXlr fake;
            fake.afterWrite = [] (xlr::ConfigBlock& block)
            {
                block[0] = 0x80;
                block[1] = 0x1E;                // the dial moved it by a step, to 30.5 dB
                block[xlr::kMuteOffset] = 1;
                block[14] = 3;
            };
            auto usb = makeUsb (fake);

            expect (usb->setGainDb (30.0));
            expectEquals (usb->lastErrorText(), std::string());
        }

        beginTest ("writes are at least 250 ms apart");
        {
            test::FakeXlr fake;
            auto usb = makeUsb (fake);

            expect (usb->setGainDb (10.0));
            expect (usb->setGainDb (20.0));
            expect (usb->setGainDb (30.0));
            expectEquals (fake.writes, 3);

            for (size_t i = 1; i < fake.writeTimesMs.size(); ++i)
                expect (fake.writeTimesMs[i] - fake.writeTimesMs[i - 1] >= 250, "gap " + juce::String ((int) i));

            fake.sleeps.clear();
            fake.nowMs += 1000;
            expect (usb->setGainDb (35.0));
            expect (fake.sleeps.empty());
        }
    }

    // ---- Allowing hardware controls ----------------------------------------------------------------------------
    void permissionTests()
    {
        beginTest ("the udev rule is the Wave:3 alone unless the user switched the experimental option on, then it adds the Wave XLR");
        {
            const auto plain = wave::permission::ruleText();
            expect (plain == wave::permission::ruleText (false));
            expect (plain.find ("0070") != std::string::npos && plain.find ("007d") == std::string::npos);

            const auto both = wave::permission::ruleText (true);
            expect (both.find ("ATTR{idProduct}==\"0070\"") != std::string::npos);
            expect (both.find ("ATTR{idProduct}==\"007d\"") != std::string::npos);
            expect (both.find ("Wave XLR") != std::string::npos);
            expect (both.find ("'") == std::string::npos, "no apostrophes: the script quotes each line");
            expect (both.find ("MODE") == std::string::npos && both.find ("GROUP") == std::string::npos);

            int ruleLines = 0;

            for (const auto& line : juce::StringArray::fromLines (juce::String (both)))
                if (line.isNotEmpty() && ! line.startsWithChar ('#'))
                    ++ruleLines;

            expectEquals (ruleLines, 2);        // exactly the tested model and the experimental one, nothing else of Elgato's
        }

        beginTest ("the experimental install script is a constant too, and tells udev about each product in the rule");
        {
            const auto plain = wave::permission::installScript();
            expect (plain == wave::permission::installScript (false));
            expect (plain.find ("idProduct=007d") == std::string::npos);

            const auto both = wave::permission::installScript (true);
            expect (both == wave::permission::installScript (true));
            expect (both.find ("--attr-match=idVendor=0fd9 --attr-match=idProduct=0070") != std::string::npos);
            expect (both.find ("--attr-match=idVendor=0fd9 --attr-match=idProduct=007d") != std::string::npos);
            expect (both.find (wave::permission::ruleText (true).substr (0, wave::permission::ruleText (true).find ('\n'))) != std::string::npos);
            expect (both.find ("mv -f \"$t\" \"$d/70-headroom-wave.rules\"") != std::string::npos);
            expect (both.find ("PATH") == std::string::npos);
            expect (both.find ("$1") == std::string::npos && both.find ("$@") == std::string::npos);
            expect (both.find ("exit 126") == std::string::npos && both.find ("exit 127") == std::string::npos);

            const auto arguments = wave::permission::pkexecArguments ("/bin/sh", true);
            expectEquals ((int) arguments.size(), 3);

            if (arguments.size() == 3)
                expect (arguments[2] == both);

#if JUCE_LINUX
            juce::ChildProcess check;
            expect (check.start (juce::StringArray { "/bin/sh", "-n", "-c", juce::String (both) }));
            expect (check.waitForProcessToFinish (10000));
            expectEquals ((int) check.getExitCode(), 0);
#endif
        }
    }

    // ---- The worker --------------------------------------------------------------------------------------------
    struct Rig
    {
        Rig()
        {
            WaveXlrHardware::Options options;
            options.usb = makeUsb (fake);
            options.nowMs = [this] { return fake.nowMs; };
            options.sleepMs = [this] (int ms) { fake.nowMs += ms; };
            options.probeIntervalMs = 500;
            options.readIntervalMs = 5000;
            options.onChanged = [this] { ++changes; };
            options.installPermission = [this] (const std::function<bool()>&)
            {
                ++installs;
                fake.presence = installResult.outcome == wave::permission::InstallResult::Outcome::Done ? wave::Presence::Ready : wave::Presence::NoPermission;
                return installResult;
            };
            hardware = std::make_unique<WaveXlrHardware> (std::move (options));
        }

        void run (int ticks, int stepMs = 600)
        {
            for (int i = 0; i < ticks; ++i)
            {
                hardware->tick();
                fake.nowMs += stepMs;
            }
        }

        test::FakeXlr fake;
        wave::permission::InstallResult installResult { wave::permission::InstallResult::Outcome::Done, "test" };
        std::unique_ptr<WaveXlrHardware> hardware;
        int changes = 0, installs = 0;
    };

    void workerTests()
    {
        beginTest ("worker: off until the user opts in: it does not even look at the bus, whatever it is asked");
        {
            Rig rig;
            expect (! rig.hardware->isEnabled());
            rig.hardware->setGainDb (30.0);             // ignored
            rig.hardware->requestPermission();          // ignored
            rig.run (10);

            expectEquals (rig.fake.probes, 0);
            expectEquals (rig.fake.opens, 0);
            expectEquals (rig.fake.configReads + rig.fake.versionReads + rig.fake.writes + rig.fake.forbidden, 0);
            expectEquals (rig.installs, 0);

            const auto state = rig.hardware->get();
            expect (! state.on && ! state.available && ! state.permission && ! state.gainDb.has_value() && state.error.isEmpty());
        }

        beginTest ("worker: opted in, it finds the unit, reads the gain and says so, and writes nothing");
        {
            Rig rig;
            rig.hardware->setEnabled (true);
            rig.run (4);

            const auto state = rig.hardware->get();
            expect (state.on && state.available && state.permission);
            expect (state.gainDb == std::optional<float> (40.0f));
            expect (state.error.isEmpty());
            expectEquals (rig.fake.writes, 0);
            expectEquals (rig.fake.forbidden, 0);
            expect (rig.changes > 0);
        }

        beginTest ("worker: without the permission it only looks, until the user allows it");
        {
            Rig rig;
            rig.fake.presence = wave::Presence::NoPermission;
            rig.hardware->setEnabled (true);
            rig.run (4);

            auto state = rig.hardware->get();
            expect (state.available && ! state.permission && ! state.gainDb.has_value());
            expectEquals (rig.fake.configReads + rig.fake.versionReads + rig.fake.writes, 0);

            rig.hardware->setGainDb (30.0);
            rig.run (4);
            expectEquals (rig.fake.writes, 0);

            rig.hardware->requestPermission();
            rig.run (6);
            expectEquals (rig.installs, 1);
            state = rig.hardware->get();
            expect (state.permission && state.gainDb.has_value() && state.error.isEmpty());
        }

        beginTest ("worker: a cancelled password box says so in plain words and changes nothing");
        {
            Rig rig;
            rig.installResult.outcome = wave::permission::InstallResult::Outcome::Cancelled;
            rig.fake.presence = wave::Presence::NoPermission;
            rig.hardware->setEnabled (true);
            rig.run (3);
            rig.hardware->requestPermission();
            rig.run (3);

            const auto state = rig.hardware->get();
            expect (! state.permission && state.error.isNotEmpty() && ! state.busy);
        }

        beginTest ("worker: a gain that was asked for is applied once, after the mic has been read");
        {
            Rig rig;
            rig.hardware->setEnabled (true);
            rig.hardware->setGainDb (30.0);
            rig.run (6);

            expectEquals (rig.fake.writes, 1);
            expect (*xlr::gainDbOf (rig.fake.block) == 30.0);
            expect (rig.hardware->get().gainDb == std::optional<float> (30.0f));
            expectEquals (rig.fake.block[kPhantom], (uint8_t) 1);
            expectEquals (rig.fake.forbidden, 0);
        }

        beginTest ("worker: the wish is not held: a dial turned afterwards stays where the person put it");
        {
            Rig rig;
            rig.hardware->setEnabled (true);
            rig.hardware->setGainDb (30.0);
            rig.run (6);
            expectEquals (rig.fake.writes, 1);

            rig.fake.block = xlr::withGainDb (rig.fake.block, 55.0);        // someone turns the unit's dial
            rig.run (30);

            expectEquals (rig.fake.writes, 1);                              // not fought, not put back
            expect (rig.hardware->get().gainDb == std::optional<float> (55.0f));    // and the new value is read within the interval
        }

        beginTest ("worker: the latest wish wins");
        {
            Rig rig;
            rig.hardware->setEnabled (true);
            rig.run (3);
            rig.hardware->setGainDb (10.0);
            rig.hardware->setGainDb (20.0);
            rig.hardware->setGainDb (25.0);
            rig.run (4);

            expectEquals (rig.fake.writes, 1);
            expect (*xlr::gainDbOf (rig.fake.block) == 25.0);
        }

        beginTest ("worker: a mic that does not take the gain is tried twice, then reported, not hammered");
        {
            Rig rig;
            rig.fake.ignoreWrites = true;
            rig.hardware->setEnabled (true);
            rig.hardware->setGainDb (30.0);
            rig.run (30);

            expectEquals (rig.fake.writes, 2);
            expect (rig.hardware->get().error.isNotEmpty());

            // A new wish from the user starts over.
            rig.fake.ignoreWrites = false;
            rig.hardware->setGainDb (35.0);
            rig.run (6);
            expectEquals (rig.fake.writes, 3);
            expect (*xlr::gainDbOf (rig.fake.block) == 35.0);
            expect (rig.hardware->get().error.isEmpty());
        }

        beginTest ("worker: a firmware it does not know is left alone, and the person is told");
        {
            Rig rig;
            rig.fake.version = test::FakeXlr::makeVersion (1, 7);
            rig.hardware->setEnabled (true);
            rig.hardware->setGainDb (30.0);
            rig.run (10);

            expectEquals (rig.fake.writes, 0);
            expectEquals (rig.fake.configReads, 0);
            const auto state = rig.hardware->get();
            expect (state.available && state.permission && ! state.gainDb.has_value() && state.error.isNotEmpty());
        }

        beginTest ("worker: a unit that is unplugged and comes back is read again");
        {
            Rig rig;
            rig.hardware->setEnabled (true);
            rig.run (4);
            expect (rig.hardware->get().gainDb.has_value());

            rig.fake.presence = wave::Presence::Absent;
            rig.run (3);
            auto state = rig.hardware->get();
            expect (! state.available && ! state.gainDb.has_value());

            rig.fake.block = xlr::withGainDb (rig.fake.block, 75.0);        // it lost power: back at its maximum
            rig.fake.presence = wave::Presence::Ready;
            rig.run (4);
            state = rig.hardware->get();
            expect (state.available && state.gainDb == std::optional<float> (75.0f));
        }

        beginTest ("worker: switched off again, it forgets everything and sends nothing more");
        {
            Rig rig;
            rig.hardware->setEnabled (true);
            rig.hardware->setGainDb (30.0);
            rig.run (6);
            const int writesBefore = rig.fake.writes, readsBefore = rig.fake.configReads, probesBefore = rig.fake.probes;

            rig.hardware->setEnabled (false);
            rig.hardware->setGainDb (50.0);
            rig.run (20);

            expectEquals (rig.fake.writes, writesBefore);
            expectEquals (rig.fake.configReads, readsBefore);
            expectEquals (rig.fake.probes, probesBefore);

            const auto state = rig.hardware->get();
            expect (! state.on && ! state.available && ! state.permission && ! state.gainDb.has_value());
        }

        beginTest ("worker: the real thread starts and stops cleanly");
        {
            test::FakeXlr fake;
            WaveXlrHardware::Options options;
            options.usb = makeUsb (fake);
            options.probeIntervalMs = 20;
            options.readIntervalMs = 40;
            WaveXlrHardware hardware (std::move (options));

            hardware.start();
            hardware.setEnabled (true);
            hardware.setGainDb (35.0);

            for (int i = 0; i < 100 && ! (hardware.get().gainDb == std::optional<float> (35.0f)); ++i)
                juce::Thread::sleep (30);

            hardware.stop();
            expect (hardware.get().gainDb == std::optional<float> (35.0f));
            expectEquals (fake.forbidden, 0);
        }
    }

    // ---- The slider --------------------------------------------------------------------------------------------
    void gainMathTests()
    {
        beginTest ("the slider for a 0 to 75 dB mic: 75 dB is the top, each 60 dB below is a factor of 10, and it round-trips");
        {
            expectWithinAbsoluteError (wave::hardwareDbToSlider (75.0, 75.0), 1.0, 1e-9);
            expectWithinAbsoluteError (wave::hardwareDbToSlider (15.0, 75.0), 0.1, 1e-9);
            expectWithinAbsoluteError (wave::hardwareDbToSlider (0.0, 75.0), std::pow (10.0, -75.0 / 60.0), 1e-9);
            expectWithinAbsoluteError (wave::hardwareDbToSlider (99.0, 75.0), 1.0, 1e-9);       // clamped

            for (double db = 0.0; db <= 75.0; db += 2.5)
                expectWithinAbsoluteError (wave::sliderToHardwareDb (wave::hardwareDbToSlider (db, 75.0), 75.0), db, 1e-6);

            expectEquals (wave::sliderToHardwareDb (0.0, 75.0), 0.0);
            expectEquals (wave::sliderToHardwareDb (1.0, 75.0), 75.0);
            expectEquals (wave::sliderToHardwareDb (2.0, 75.0), 75.0);

            // The Wave:3's own mapping is untouched.
            expectWithinAbsoluteError (wave::hardwareDbToSlider (24.0), 0.5412, 1e-4);
            expectWithinAbsoluteError (wave::hardwareDbToSlider (24.0), wave::hardwareDbToSlider (24.0, 40.0), 1e-12);
        }
    }
};

static WaveXlrTests waveXlrTests;
}
