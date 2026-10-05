// Unit tests for the Wave:3 USB side: the protocol helpers, the guarded read-modify-write against a fake device, finding
// the device in sysfs, the udev rule and its helper's argument rules, and the worker that keeps ClipGuard where the user
// put it. None of it touches a real USB device.
#include "Juce.h"

#include "FakeWaveUsb.h"
#include "backend/wave/WaveGain.h"
#include "backend/wave/WaveHardware.h"
#include "backend/wave/WavePermission.h"
#include "backend/wave/WaveProtocol.h"
#include "backend/wave/WaveUsb.h"

#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace hr
{
namespace
{
    using wave::ConfigBlock;

    struct TempDir
    {
        TempDir() : dir (juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("headroom-wavetest", "", false))
        {
            dir.createDirectory();
        }

        ~TempDir()      { dir.deleteRecursively(); }

        std::filesystem::path path() const      { return std::filesystem::path (dir.getFullPathName().toStdString()); }

        juce::File dir;
    };

    // The live dump published by Kailas Mahavarkar: gain 40 dB, ClipGuard on, low-cut on, gain lock on, dial on the mic.
    const ConfigBlock kLiveDump { 0x00, 0x28, 0x00, 0x00, 0x00, 0x01, 0x01, 0x80, 0xe8, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01 };

    void makeSysfsDevice (const juce::File& root, const juce::String& name, const char* vendor, const char* product, int bus, int device)
    {
        auto dir = root.getChildFile (name);
        dir.createDirectory();
        dir.getChildFile ("idVendor").replaceWithText (juce::String (vendor) + "\n");
        dir.getChildFile ("idProduct").replaceWithText (juce::String (product) + "\n");
        dir.getChildFile ("busnum").replaceWithText (juce::String (bus) + "\n");
        dir.getChildFile ("devnum").replaceWithText (juce::String (device) + "\n");
    }

    std::unique_ptr<wave::WaveUsb> makeUsb (test::FakeWave& fake)
    {
        return std::make_unique<wave::WaveUsb> (std::make_unique<test::FakeBus> (fake), fake.clock());
    }
}

class WaveUsbTests final : public juce::UnitTest
{
public:
    WaveUsbTests() : juce::UnitTest ("Wave:3 USB", "headroom") {}

    void runTest() override
    {
        protocolTests();
        descriptorTests();
        sysfsTests();
        readModifyWriteTests();
        safetyTests();
        permissionTests();
        workerTests();
        gainMathTests();
    }

private:
    // ---- The block ---------------------------------------------------------------------------------------------
    void protocolTests()
    {
        beginTest ("the documented request: class, block 0, wIndex 0x3303, 16 bytes");
        {
            expectEquals ((int) wave::kRequestTypeIn, 0xA1);
            expectEquals ((int) wave::kRequestTypeOut, 0x21);
            expectEquals ((int) wave::kRequestGet, 0x85);
            expectEquals ((int) wave::kRequestSet, 0x05);
            expectEquals ((int) wave::kConfigBlock, 0);
            expectEquals ((int) wave::kIndex, 0x3303);
            expectEquals ((int) wave::kConfigSize, 16);
            expectEquals ((int) wave::kClipGuardOffset, 5);
            expectEquals ((int) wave::kVendorId, 0x0fd9);
            expectEquals ((int) wave::kProductId, 0x0070);
            expect (wave::kMinWriteGapMs >= 250);
        }

        beginTest ("a block parses into its fields");
        {
            expect (wave::clipGuardOf (kLiveDump) == std::optional<bool> (true));
            expect (wave::gainLockOf (kLiveDump) == std::optional<bool> (true));
            expectWithinAbsoluteError (*wave::gainDbOf (kLiveDump), 40.0, 1.0e-9);
            expect (wave::looksLikeConfig (kLiveDump));

            auto lower = kLiveDump;
            lower[0] = 0x00;  lower[1] = 0x18;      // 0x1800 = 24.0 dB
            expectWithinAbsoluteError (*wave::gainDbOf (lower), 24.0, 1.0e-9);
            lower[0] = 0x40;                        // 24.25 dB: the 0.25 dB steps of the UAC path
            expectWithinAbsoluteError (*wave::gainDbOf (lower), 24.25, 1.0e-9);

            auto off = kLiveDump;
            off[5] = 0;
            expect (wave::clipGuardOf (off) == std::optional<bool> (false));
        }

        beginTest ("a block that does not look like the layout is recognised");
        {
            std::string why;

            auto odd = kLiveDump;
            odd[5] = 2;
            expect (! wave::clipGuardOf (odd).has_value());
            expect (! wave::looksLikeConfig (odd, &why));
            expect (why.find ("ClipGuard") != std::string::npos);

            odd = kLiveDump;
            odd[1] = 0x29;                          // above 40 dB
            expect (! wave::gainDbOf (odd).has_value());
            expect (! wave::looksLikeConfig (odd));

            odd = kLiveDump;
            odd[1] = 0x80;                          // negative
            expect (! wave::looksLikeConfig (odd));

            odd = kLiveDump;
            odd[4] = 5;                             // mute byte
            expect (! wave::looksLikeConfig (odd));

            odd = kLiveDump;
            odd[12] = 0;                            // dial mode
            expect (! wave::looksLikeConfig (odd));
            odd[12] = 4;
            expect (! wave::looksLikeConfig (odd));
        }

        beginTest ("setting ClipGuard changes that one byte and no other");
        {
            juce::Random random (7);

            for (int round = 0; round < 50; ++round)
            {
                ConfigBlock block = kLiveDump;

                for (auto& byte : block)
                    byte = (uint8_t) random.nextInt (256);

                block[wave::kClipGuardOffset] = (uint8_t) random.nextInt (2);
                block[wave::kGainOffset + 1] = (uint8_t) random.nextInt (0x28);
                block[wave::kMuteOffset] = (uint8_t) random.nextInt (2);
                block[wave::kDialModeOffset] = (uint8_t) (1 + random.nextInt (3));

                for (const bool on : { false, true })
                {
                    const auto changed = wave::withClipGuard (block, on);

                    for (size_t i = 0; i < wave::kConfigSize; ++i)
                        if (i != wave::kClipGuardOffset && changed[i] != block[i])
                            expect (false, "byte " + juce::String ((int) i) + " changed");

                    expect (wave::clipGuardOf (changed) == std::optional<bool> (on));
                }
            }
        }

        beginTest ("a written block that is read back: what must match and what may move");
        {
            const auto written = wave::withClipGuard (kLiveDump, false);
            expect (wave::writeWasKept (written, written));

            // The dial, the mute pad and the headphone volume move by themselves.
            auto moved = written;
            moved[0] = 0x40;  moved[4] = 1;  moved[7] = 0x00;  moved[8] = 0xf0;  moved[9] = 1;  moved[10] = 5;  moved[11] = 9;  moved[12] = 2;
            expect (wave::writeWasKept (written, moved));

            // Anything that only software writes, or a ClipGuard that did not take, is a failure.
            for (const size_t offset : { (size_t) 2, (size_t) 3, (size_t) 5, (size_t) 6, (size_t) 13, (size_t) 14, (size_t) 15 })
            {
                auto wrong = written;
                wrong[offset] = (uint8_t) (wrong[offset] ^ 0x01);
                expect (! wave::writeWasKept (written, wrong), "offset " + juce::String ((int) offset));
            }

            expectEquals (juce::String (wave::toHex (kLiveDump)), juce::String ("00 28 00 00 00 01 01 80 e8 00 00 00 01 00 00 01"));
        }

        beginTest ("a block survives being read into fields and put back, and being written out as hex and read again");
        {
            juce::Random random (11);

            for (int round = 0; round < 100; ++round)
            {
                ConfigBlock block {};

                for (auto& byte : block)
                    byte = (uint8_t) random.nextInt (256);

                block[wave::kClipGuardOffset] = (uint8_t) random.nextInt (2);

                // Fields out, ClipGuard put back as read: the same block.
                expect (wave::withClipGuard (block, *wave::clipGuardOf (block)) == block);

                // Hex out and in.
                const auto again = wave::fromHex (wave::toHex (block));
                expect (again.has_value() && *again == block);
            }

            expect (*wave::fromHex ("00 28 00 00 00 01 01 80 e8 00 00 00 01 00 00 01") == kLiveDump);
            expect (*wave::fromHex ("00 28 00 00 00 01 01 80 E8 00 00 00 01 00 00 01") == kLiveDump);
            expect (! wave::fromHex ("").has_value());
            expect (! wave::fromHex ("00 28 00 00 00 01 01 80 e8 00 00 00 01 00 00").has_value());          // 15 bytes
            expect (! wave::fromHex ("00 28 00 00 00 01 01 80 e8 00 00 00 01 00 00 01 00").has_value());    // 17 bytes
            expect (! wave::fromHex ("00 28 00 00 00 01 01 80 e8 00 00 00 01 00 00 0g").has_value());
            expect (! wave::fromHex ("0028 00 00 00 01 01 80 e8 00 00 00 01 00 00 01 0").has_value());
        }
    }

    // ---- Who is it -----------------------------------------------------------------------------------------------
    void descriptorTests()
    {
        beginTest ("the opened device is identified from its descriptors");
        {
            test::FakeWave fake;
            test::FakeDevice device (fake);
            auto d = device.readDescriptors();

            const auto ids = wave::parseDeviceIds (d.data(), d.size());
            expect (ids.has_value() && ids->isWave3());
            expect (wave::interfaceClass (d.data(), d.size(), 3) == std::optional<uint8_t> ((uint8_t) 0xFF));
            expect (wave::interfaceClass (d.data(), d.size(), 0) == std::optional<uint8_t> ((uint8_t) 0x01));
            expect (! wave::interfaceClass (d.data(), d.size(), 4).has_value());

            fake.product = 0x0071;      // firmware-update mode
            auto dfu = test::FakeDevice (fake).readDescriptors();
            expect (! wave::parseDeviceIds (dfu.data(), dfu.size())->isWave3());

            fake.product = 0x0070;
            fake.vendor = 0x1234;
            auto other = test::FakeDevice (fake).readDescriptors();
            expect (! wave::parseDeviceIds (other.data(), other.size())->isWave3());

            expect (! wave::parseDeviceIds (d.data(), 10).has_value());
            expect (! wave::parseDeviceIds (nullptr, 0).has_value());

            auto garbage = d;
            garbage[1] = 5;             // not a device descriptor
            expect (! wave::parseDeviceIds (garbage.data(), garbage.size()).has_value());

            // A truncated or nonsense descriptor list never reads past its end.
            auto broken = d;
            broken[18 + 9 + 1] = 4;
            broken[18 + 9] = 250;       // a descriptor that claims to be longer than the dump
            expect (! wave::interfaceClass (broken.data(), broken.size(), 3).has_value());
        }
    }

    void sysfsTests()
    {
        beginTest ("the device is found in sysfs by its exact ids");
        {
            TempDir temp;
            makeSysfsDevice (temp.dir, "1-2", "0fd9", "0070", 1, 5);            // the Wave:3
            makeSysfsDevice (temp.dir, "1-3", "0fd9", "0071", 1, 6);            // its firmware-update mode
            makeSysfsDevice (temp.dir, "1-4", "0fd9", "006e", 1, 7);            // a Wave:1
            makeSysfsDevice (temp.dir, "2-1", "1234", "0070", 2, 9);            // somebody else's 0070
            makeSysfsDevice (temp.dir, "3-1", "0FD9", "0070", 3, 12);           // upper case hex is the same id
            temp.dir.getChildFile ("1-2:1.0").createDirectory();                // an interface
            temp.dir.getChildFile ("usb1").createDirectory();                   // a hub with no ids

            const auto nodes = wave::findWave3Nodes (temp.path());
            expectEquals ((int) nodes.size(), 2);

            if (nodes.size() == 2)
            {
                expectEquals (juce::String (nodes[0]), juce::String ("/dev/bus/usb/001/005"));
                expectEquals (juce::String (nodes[1]), juce::String ("/dev/bus/usb/003/012"));
            }
        }

        beginTest ("no sysfs, an empty one, and damaged entries all mean: not there");
        {
            TempDir temp;
            expect (wave::findWave3Nodes (temp.path() / "missing").empty());
            expect (wave::findWave3Nodes (temp.path()).empty());

            makeSysfsDevice (temp.dir, "1-2", "0fd9", "0070", 1, 5);
            temp.dir.getChildFile ("1-2").getChildFile ("busnum").replaceWithText ("one\n");
            expect (wave::findWave3Nodes (temp.path()).empty());

            expectEquals (juce::String (wave::usbNodePath (12, 3)), juce::String ("/dev/bus/usb/012/003"));
        }
    }

    // ---- The guarded write ---------------------------------------------------------------------------------------
    void readModifyWriteTests()
    {
        beginTest ("reading ClipGuard and the whole block");
        {
            test::FakeWave fake;
            auto usb = makeUsb (fake);

            ConfigBlock block {};
            expect (usb->readConfig (block));
            expect (block == fake.block);
            expect (usb->getClipGuard() == std::optional<bool> (true));
            expectEquals (fake.writes, 0);
            expectEquals (fake.forbidden, 0);
        }

        beginTest ("a change is a read-modify-write of the whole block that touches only the ClipGuard byte");
        {
            test::FakeWave fake;
            const auto before = fake.block;
            auto usb = makeUsb (fake);

            expect (usb->setClipGuard (false));
            expectEquals (fake.writes, 1);
            expect (fake.reads >= 2);                       // the read before, and the read-back after
            expect (fake.block == wave::withClipGuard (before, false));

            for (size_t i = 0; i < wave::kConfigSize; ++i)
                if (i != wave::kClipGuardOffset)
                    expectEquals ((int) fake.block[i], (int) before[i]);

            expect (usb->getClipGuard() == std::optional<bool> (false));

            expect (usb->setClipGuard (true));
            expect (fake.block == before);                  // and back: nothing else moved on the way
            expectEquals (fake.forbidden, 0);
        }

        beginTest ("it is already so: no write at all");
        {
            test::FakeWave fake;
            auto usb = makeUsb (fake);

            expect (usb->setClipGuard (true));
            expectEquals (fake.writes, 0);
            expectEquals (fake.forbidden, 0);
        }

        beginTest ("a dial turned between the write and the read-back is not a failure");
        {
            test::FakeWave fake;
            fake.afterWrite = [] (ConfigBlock& block) { block[0] = 0x80; block[1] = 0x15; block[12] = 3; };
            auto usb = makeUsb (fake);

            expect (usb->setClipGuard (false));
            expectEquals (usb->lastErrorText(), std::string());
        }

        beginTest ("writes are at least 250 ms apart");
        {
            test::FakeWave fake;
            auto usb = makeUsb (fake);

            expect (usb->setClipGuard (false));
            expect (usb->setClipGuard (true));
            expect (usb->setClipGuard (false));
            expectEquals (fake.writes, 3);

            for (size_t i = 1; i < fake.writeTimesMs.size(); ++i)
                expect (fake.writeTimesMs[i] - fake.writeTimesMs[i - 1] >= 250, "gap " + juce::String ((int) i));

            // Long enough after the last write, nothing needs to wait.
            fake.sleeps.clear();
            fake.nowMs += 1000;
            expect (usb->setClipGuard (true));
            expect (fake.sleeps.empty());
        }
    }

    void safetyTests()
    {
        beginTest ("anything that is not exactly a 0fd9:0070 is left alone: not one transfer");
        {
            using Ids = std::pair<uint16_t, uint16_t>;
            const std::array<Ids, 4> lookalikes { Ids { (uint16_t) 0x0fd9, (uint16_t) 0x0071 },        // firmware-update mode
                                                  Ids { (uint16_t) 0x0fd9, (uint16_t) 0x006e },        // Wave:1
                                                  Ids { (uint16_t) 0x0fd9, (uint16_t) 0x007d },        // Wave XLR
                                                  Ids { (uint16_t) 0x1234, (uint16_t) 0x0070 } };
            for (const auto& ids : lookalikes)
            {
                test::FakeWave fake;
                fake.vendor = ids.first;
                fake.product = ids.second;
                auto usb = makeUsb (fake);

                ConfigBlock block {};
                expect (! usb->readConfig (block));
                expect (usb->lastError() == wave::Error::WrongDevice);
                expect (! usb->setClipGuard (false));
                expect (! usb->getClipGuard().has_value());
                expectEquals (fake.reads + fake.writes + fake.forbidden, 0);
            }
        }

        beginTest ("a Wave:3 without a vendor interface 3 is left alone");
        {
            test::FakeWave fake;
            fake.hasInterface3 = false;
            auto usb = makeUsb (fake);
            expect (! usb->setClipGuard (false));
            expect (usb->lastError() == wave::Error::WrongDevice);
            expectEquals (fake.reads + fake.writes, 0);

            fake.hasInterface3 = true;
            fake.interface3Class = 0x01;        // interface 3 is audio here: wIndex 0x3303 would point at the wrong thing
            expect (! usb->setClipGuard (false));
            expectEquals (fake.reads + fake.writes, 0);
        }

        beginTest ("a block that is not the known layout is not written");
        {
            test::FakeWave fake;
            fake.block[wave::kClipGuardOffset] = 7;
            auto usb = makeUsb (fake);
            expect (! usb->setClipGuard (false));
            expect (usb->lastError() == wave::Error::UnknownLayout);
            expectEquals (fake.writes, 0);
            expect (! usb->getClipGuard().has_value());

            test::FakeWave shortBlock;              // API 5.2 has a 14-byte block
            shortBlock.readReturns = 14;
            auto usb2 = makeUsb (shortBlock);
            expect (! usb2->setClipGuard (false));
            expect (usb2->lastError() == wave::Error::UnknownLayout);
            expectEquals (shortBlock.writes, 0);
        }

        beginTest ("a device that ignores the write is reported, not believed");
        {
            test::FakeWave fake;
            fake.ignoreWrites = true;
            auto usb = makeUsb (fake);

            expect (! usb->setClipGuard (false));
            expect (usb->lastError() == wave::Error::NotKept);
            expectEquals (fake.writes, 1);
            expect (! usb->lastErrorText().empty());
            expect (usb->lastErrorDetail().find ("read back") != std::string::npos);
        }

        beginTest ("no device, no permission, a failing device and a bus that cannot do it");
        {
            test::FakeWave fake;
            fake.presence = wave::Presence::Absent;
            auto usb = makeUsb (fake);
            ConfigBlock block {};
            expect (! usb->isAvailable());
            expect (! usb->hasPermission());
            expect (! usb->readConfig (block));
            expect (usb->lastError() == wave::Error::NotFound);

            fake.presence = wave::Presence::NoPermission;
            expect (usb->isAvailable());
            expect (! usb->hasPermission());
            expect (! usb->setClipGuard (true));
            expect (usb->lastError() == wave::Error::NoPermission);

            fake.presence = wave::Presence::Ambiguous;
            expect (! usb->isAvailable());          // two Wave:3s: which one is meant is not known

            fake.presence = wave::Presence::Ready;
            fake.failWithErrno = EBUSY;
            expect (! usb->setClipGuard (true));
            expect (usb->lastError() == wave::Error::Busy);
            expect (! fake.sleeps.empty());         // it backed off and tried again before giving up
            fake.sleeps.clear();

            fake.failWithErrno = EIO;
            expect (! usb->setClipGuard (true));
            expect (usb->lastError() == wave::Error::Io);
            expect (fake.sleeps.empty());           // a real failure is not retried
            expectEquals (fake.writes, 0);

#if ! JUCE_LINUX
            wave::WaveUsb system;
            expect (system.probe() == wave::Presence::Unsupported);
            expect (! system.setClipGuard (true));
            expect (system.lastError() == wave::Error::Unsupported);
#endif
        }
    }

    // ---- Allowing hardware controls --------------------------------------------------------------------------------
    void permissionTests()
    {
        // What the person at the computer is asked to approve is exactly this text, and nothing is put into it.
        const std::string expectedScript =
            "umask 022 || exit 10\n"
            "d=/etc/udev/rules.d\n"
            "t=$d/.70-headroom-wave.rules.tmp\n"
            "mkdir -p \"$d\" || exit 11\n"
            "printf '%s\\n' '# Written by Headroom when you press \"Allow hardware controls\": it lets you, at this computer, control the'"
            " '# Elgato Wave:3 (ClipGuard). The Headroom uninstall.sh removes it.'"
            " 'SUBSYSTEM==\"usb\", ATTR{idVendor}==\"0fd9\", ATTR{idProduct}==\"0070\", TAG+=\"uaccess\"' > \"$t\" || { rm -f \"$t\"; exit 12; }\n"
            "mv -f \"$t\" \"$d/70-headroom-wave.rules\" || { rm -f \"$t\"; exit 13; }\n"
            "for u in /usr/bin/udevadm /bin/udevadm /usr/sbin/udevadm /sbin/udevadm; do [ -x \"$u\" ] && break; done\n"
            "\"$u\" control --reload || echo \"udevadm control --reload did not succeed\" >&2\n"
            "\"$u\" trigger --action=add --attr-match=idVendor=0fd9 --attr-match=idProduct=0070 || echo \"udevadm trigger did not succeed\" >&2\n"
            "\"$u\" settle --timeout=5 || echo \"udevadm settle did not succeed\" >&2\n"
            "exit 0\n";

        beginTest ("the udev rule is exactly the Wave:3, for the user at the seat");
        {
            const std::string expected =
                "# Written by Headroom when you press \"Allow hardware controls\": it lets you, at this computer, control the\n"
                "# Elgato Wave:3 (ClipGuard). The Headroom uninstall.sh removes it.\n"
                "SUBSYSTEM==\"usb\", ATTR{idVendor}==\"0fd9\", ATTR{idProduct}==\"0070\", TAG+=\"uaccess\"\n";

            expectEquals (juce::String (wave::permission::ruleText()), juce::String (expected));
            expectEquals (juce::String (wave::permission::kRuleFileName), juce::String ("70-headroom-wave.rules"));
            expectEquals (juce::String (wave::permission::kRulesDirectory), juce::String ("/etc/udev/rules.d"));

            // One rule line, and only the one device.
            const auto text = juce::String (wave::permission::ruleText());
            int ruleLines = 0;

            for (const auto& line : juce::StringArray::fromLines (text))
                if (line.isNotEmpty() && ! line.startsWithChar ('#'))
                    ++ruleLines;

            expectEquals (ruleLines, 1);
            expect (! text.contains ("MODE"));
            expect (! text.contains ("GROUP"));
            expect (! text.contains ("'"), "the script quotes each rule line in single quotes");
        }

        beginTest ("the install script is a constant: exactly this text, with nothing put into it");
        {
            expectEquals (juce::String (wave::permission::installScript()), juce::String (expectedScript));

            // Every call gives the same text (nothing in it comes from the user, the environment or the time).
            expect (wave::permission::installScript() == wave::permission::installScript());

            // It writes the rule text that ruleText() says, atomically (a hidden temp name, then mv -f), for exactly the Wave:3.
            const auto script = wave::permission::installScript();
            expect (script.find (wave::permission::ruleText().substr (0, wave::permission::ruleText().find ('\n'))) != std::string::npos);
            expect (script.find ("ATTR{idProduct}==\"0070\"") != std::string::npos);
            expect (script.find ("--attr-match=idVendor=0fd9 --attr-match=idProduct=0070") != std::string::npos);
            expect (script.find ("mv -f \"$t\" \"$d/70-headroom-wave.rules\"") != std::string::npos);

            // udevadm is found among fixed system paths, never through PATH; and the script only uses absolute paths for it.
            expect (script.find ("/usr/bin/udevadm /bin/udevadm /usr/sbin/udevadm /sbin/udevadm") != std::string::npos);
            expect (script.find ("PATH") == std::string::npos);
            expect (script.find ("$1") == std::string::npos && script.find ("$@") == std::string::npos && script.find ("$*") == std::string::npos);
            expect (script.find ("$HOME") == std::string::npos && script.find ("$USER") == std::string::npos);

            // It never exits with the codes pkexec keeps for itself.
            expect (script.find ("exit 126") == std::string::npos && script.find ("exit 127") == std::string::npos);
        }

        beginTest ("pkexec is given a shell, -c and the constant script: three arguments and no others");
        {
            const auto arguments = wave::permission::pkexecArguments ("/bin/sh");
            expectEquals ((int) arguments.size(), 3);

            if (arguments.size() == 3)
            {
                expectEquals (juce::String (arguments[0]), juce::String ("/bin/sh"));
                expectEquals (juce::String (arguments[1]), juce::String ("-c"));
                expectEquals (juce::String (arguments[2]), juce::String (expectedScript));
            }

            // Headroom's own binary is not part of it: there is no --install-udev-rule mode, and the script never names one.
            expect (wave::permission::installScript().find ("--install-udev-rule") == std::string::npos);
        }

#if JUCE_LINUX
        beginTest ("the script is valid shell, and writes the rule atomically (run against a scratch folder)");
        {
            const auto script = wave::permission::installScript();

            // Syntax only: sh -n reads the whole script and runs nothing.
            {
                juce::ChildProcess check;
                expect (check.start (juce::StringArray { "/bin/sh", "-n", "-c", juce::String (script) }));
                expect (check.waitForProcessToFinish (10000));
                expectEquals ((int) check.getExitCode(), 0);
            }

            // Run only the part that writes the file (everything before the udevadm lines), in a scratch folder instead of
            // /etc/udev/rules.d: the same text, the same commands, a different `d=`.
            TempDir temp;
            const auto rulesDir = temp.dir.getChildFile ("rules.d");
            const auto cut = script.find ("for u in");
            expect (cut != std::string::npos);

            auto writePart = script.substr (0, cut);
            const std::string realDir = "d=/etc/udev/rules.d";
            const auto at = writePart.find (realDir);
            expect (at != std::string::npos);
            writePart.replace (at, realDir.size(), "d=" + rulesDir.getFullPathName().toStdString());
            writePart += "exit 0\n";

            const auto run = [&]
            {
                juce::ChildProcess process;
                expect (process.start (juce::StringArray { "/bin/sh", "-c", juce::String (writePart) }));
                expect (process.waitForProcessToFinish (10000));
                return (int) process.getExitCode();
            };

            expectEquals (run(), 0);
            const auto rule = rulesDir.getChildFile ("70-headroom-wave.rules");
            expect (rule.existsAsFile());
            expectEquals (rule.loadFileAsString().toStdString(), wave::permission::ruleText());
            expectEquals (rulesDir.getNumberOfChildFiles (juce::File::findFiles | juce::File::findFilesAndDirectories, "*"), 1);
            expect (! rulesDir.getChildFile (".70-headroom-wave.rules.tmp").exists());

            // Readable by everyone, writable only by the owner (umask 022).
            expect (rule.hasReadAccess());

            // A stale or damaged file is replaced, and running it again changes nothing.
            rule.replaceWithText ("SUBSYSTEM==\"usb\", MODE=\"0666\"\n");
            expectEquals (run(), 0);
            expectEquals (rule.loadFileAsString().toStdString(), wave::permission::ruleText());
            expectEquals (run(), 0);
            expectEquals (rule.loadFileAsString().toStdString(), wave::permission::ruleText());

            // A folder that cannot be made is a failure with the script's own code, not 126 or 127.
            const auto blocker = temp.dir.getChildFile ("blocker");
            blocker.replaceWithText ("a file");
            auto failing = script.substr (0, cut);
            failing.replace (failing.find (realDir), realDir.size(), "d=" + blocker.getFullPathName().toStdString() + "/rules.d");
            failing += "exit 0\n";

            juce::ChildProcess process;
            expect (process.start (juce::StringArray { "/bin/sh", "-c", juce::String (failing) }));
            expect (process.waitForProcessToFinish (10000));
            expectEquals ((int) process.getExitCode(), 11);
        }
#endif

        beginTest ("what a person is told when the password box does not lead anywhere");
        {
            using O = wave::permission::InstallResult::Outcome;
            expect (wave::permission::describe (O::Done).empty());

            for (const auto outcome : { O::Cancelled, O::NotAuthorised, O::NoPkexec, O::Failed })
                expect (! wave::permission::describe (outcome).empty());
        }
    }

    // ---- The worker ----------------------------------------------------------------------------------------------
    struct Rig
    {
        Rig()
        {
            WaveHardware::Options options;
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
            hardware = std::make_unique<WaveHardware> (std::move (options));
        }

        void run (int ticks, int stepMs = 600)
        {
            for (int i = 0; i < ticks; ++i)
            {
                hardware->tick();
                fake.nowMs += stepMs;
            }
        }

        test::FakeWave fake;
        wave::permission::InstallResult installResult { wave::permission::InstallResult::Outcome::Done, "test" };
        std::unique_ptr<WaveHardware> hardware;
        int changes = 0, installs = 0;
    };

    void workerTests()
    {
        beginTest ("worker: no Wave:3, nothing to say");
        {
            Rig rig;
            rig.fake.presence = wave::Presence::Absent;
            rig.hardware->setClipGuard (true);
            rig.run (5);

            const auto state = rig.hardware->get();
            expect (! state.usbAvailable);
            expect (! state.usbPermission);
            expect (! state.clipGuard.has_value());
            expect (state.error.isEmpty());
            expectEquals (rig.fake.reads + rig.fake.writes + rig.fake.opens, 0);
        }

        beginTest ("worker: without permission it only looks, until the user allows it");
        {
            Rig rig;
            rig.fake.presence = wave::Presence::NoPermission;
            rig.hardware->setClipGuard (false);         // what the start-up restore does: waits for the permission
            rig.run (3);

            auto state = rig.hardware->get();
            expect (state.usbAvailable);
            expect (! state.usbPermission);
            expect (! state.clipGuard.has_value());
            expectEquals (rig.fake.opens, 0);           // never even opened

            rig.hardware->requestPermission();
            rig.run (3);

            state = rig.hardware->get();
            expectEquals (rig.installs, 1);
            expect (state.usbPermission);
            expect (state.error.isEmpty());
            expect (! state.busy);
            expect (state.clipGuard == std::optional<bool> (false));        // and the saved state was applied: it was on
            expect (rig.fake.block[wave::kClipGuardOffset] == 0);
            expectEquals (rig.fake.writes, 1);
            expectEquals (rig.fake.forbidden, 0);
        }

        beginTest ("worker: a cancelled password box says so in plain words and changes nothing");
        {
            Rig rig;
            rig.fake.presence = wave::Presence::NoPermission;
            rig.installResult.outcome = wave::permission::InstallResult::Outcome::Cancelled;
            rig.run (2);
            rig.hardware->requestPermission();
            rig.run (2);

            const auto state = rig.hardware->get();
            expect (! state.usbPermission);
            expect (state.error.contains ("cancelled"));
            expect (! state.busy);
            expectEquals (rig.fake.opens, 0);
        }

        beginTest ("worker: it reads first and writes only when the mic is not already as wanted");
        {
            Rig rig;
            rig.hardware->setClipGuard (true);          // the mic says on already
            rig.run (4);
            expectEquals (rig.fake.writes, 0);
            expect (rig.hardware->get().clipGuard == std::optional<bool> (true));
            expect (rig.hardware->get().gainDb.has_value() && std::abs (*rig.hardware->get().gainDb - 40.0f) < 0.01f);
            expect (rig.hardware->get().gainLocked == std::optional<bool> (true));

            rig.hardware->setClipGuard (false);
            rig.run (2);
            expectEquals (rig.fake.writes, 1);
            expect (rig.hardware->get().clipGuard == std::optional<bool> (false));
            expect (rig.hardware->get().error.isEmpty());
            expect (rig.changes > 0);
        }

        beginTest ("worker: the mic is left alone until the user has said what they want");
        {
            Rig rig;
            rig.run (6, 6000);
            expectEquals (rig.fake.writes, 0);
            expect (rig.hardware->get().clipGuard == std::optional<bool> (true));      // it just reports
        }

        beginTest ("worker: a mic that lost power and came back is put back the way it was");
        {
            Rig rig;
            rig.hardware->setClipGuard (true);
            rig.run (3);
            expectEquals (rig.fake.writes, 0);

            // Unplugged, plugged back in, and it forgot: ClipGuard is off again.
            rig.fake.presence = wave::Presence::Absent;
            rig.run (2);
            expect (! rig.hardware->get().usbAvailable);
            expect (! rig.hardware->get().clipGuard.has_value());

            rig.fake.block[wave::kClipGuardOffset] = 0;
            rig.fake.presence = wave::Presence::Ready;
            rig.run (3);
            expectEquals (rig.fake.writes, 1);
            expect (rig.fake.block[wave::kClipGuardOffset] == 1);
            expect (rig.hardware->get().clipGuard == std::optional<bool> (true));
        }

        beginTest ("worker: a mic that keeps falling back is left alone after a few tries");
        {
            Rig rig;
            rig.fake.block[wave::kClipGuardOffset] = 0;
            rig.fake.afterWrite = [] (ConfigBlock& block) { block[wave::kClipGuardOffset] = 0; };     // takes it, then forgets
            rig.hardware->setClipGuard (true);
            rig.run (40);

            expect (rig.fake.writes <= 4, "writes " + juce::String (rig.fake.writes));
            expect (rig.fake.writes >= 1);
            expect (rig.hardware->get().error.contains ("keeps switching back"));

            // Asking again is a new budget.
            const int before = rig.fake.writes;
            rig.hardware->setClipGuard (true);
            rig.run (4);
            expect (rig.fake.writes > before);
        }

        beginTest ("worker: a change the mic did not keep shows an error and the real state");
        {
            Rig rig;
            rig.fake.ignoreWrites = true;
            rig.hardware->setClipGuard (false);
            rig.run (3);

            const auto state = rig.hardware->get();
            expect (state.error.isNotEmpty());
            expect (state.clipGuard == std::optional<bool> (true));             // what the mic really says
            expect (! state.busy);
        }

        beginTest ("worker: asking for a read soon refreshes the gain the mic reports");
        {
            Rig rig;
            rig.run (2);
            expect (std::abs (*rig.hardware->get().gainDb - 40.0f) < 0.01f);

            rig.fake.block[0] = 0x00;  rig.fake.block[1] = 0x18;                // the dial says 24 dB now
            rig.hardware->readSoon (0);
            rig.run (1);
            expect (std::abs (*rig.hardware->get().gainDb - 24.0f) < 0.01f);
        }

        beginTest ("worker: the real thread starts and stops cleanly");
        {
            Rig rig;
            rig.hardware->start();
            rig.hardware->setClipGuard (false);

            for (int i = 0; i < 50 && rig.hardware->get().clipGuard != std::optional<bool> (false); ++i)
                juce::Thread::sleep (20);

            rig.hardware->stop();
            expect (rig.hardware->get().clipGuard == std::optional<bool> (false));
            rig.hardware->stop();       // twice is fine
        }
    }

    // ---- Gain maths ----------------------------------------------------------------------------------------------
    void gainMathTests()
    {
        beginTest ("the cubic slider maps the mic's 0 to +40 dB onto its top");
        {
            expectWithinAbsoluteError (wave::hardwareDbToSlider (40.0), 1.0, 1.0e-12);
            expectWithinAbsoluteError (wave::hardwareDbToSlider (24.0), 0.5412, 1.0e-4);
            expectWithinAbsoluteError (wave::hardwareDbToSlider (0.0), 0.2154, 1.0e-4);
            expectWithinAbsoluteError (wave::sliderToHardwareDb (1.0), 40.0, 1.0e-9);
            expectWithinAbsoluteError (wave::sliderToHardwareDb (wave::hardwareDbToSlider (24.0)), 24.0, 1.0e-9);
            expectWithinAbsoluteError (wave::sliderToHardwareDb (0.0), 0.0, 1.0e-12);

            // The 0.25 dB steps of the mic survive the round trip through float.
            for (double db = 0.0; db <= 40.0; db += 0.25)
                expectWithinAbsoluteError ((double) wave::sliderToHardwareDb ((double) (float) wave::hardwareDbToSlider (db)), db, 1.0e-3);

            // It is the same cube DeviceControl uses for the route volume: slider^3 is the linear amplitude, +24 dB is -16 dB of it.
            expectWithinAbsoluteError (wave::sliderToVolume (0.5f), 0.125f, 1.0e-7f);
            expectWithinAbsoluteError (wave::volumeToSlider (0.125f), 0.5f, 1.0e-6f);
            expectWithinAbsoluteError (20.0 * std::log10 ((double) wave::sliderToVolume ((float) wave::hardwareDbToSlider (24.0))), -16.0, 1.0e-3);
        }

        beginTest ("Safe gain: the mic at +24, Headroom adds the +16 that was given up");
        {
            auto plan = wave::planSafeGain (40.0, 24.0);
            expectWithinAbsoluteError (plan.targetDb, 24.0, 1.0e-12);
            expectWithinAbsoluteError (plan.makeupDb, 16.0, 1.0e-9);
            expect (! plan.hardwareTooHigh);

            plan = wave::planSafeGain (40.0, 24.25);            // the mic rounded up by a step: the makeup takes it off
            expectWithinAbsoluteError (plan.makeupDb, 15.8, 1.0e-9);
            expect (! plan.hardwareTooHigh);

            plan = wave::planSafeGain (40.0, 23.75);            // rounded down: the target still holds, so does the makeup
            expectWithinAbsoluteError (plan.makeupDb, 16.0, 1.0e-9);
        }

        beginTest ("Safe gain: a mic that did not take the lower gain gets no makeup and is asked again");
        {
            const auto plan = wave::planSafeGain (40.0, 40.0);
            expect (plan.hardwareTooHigh);
            expectWithinAbsoluteError (plan.makeupDb, 0.0, 1.0e-12);
            expectWithinAbsoluteError (plan.targetDb, 24.0, 1.0e-12);

            expect (wave::planSafeGain (40.0, 24.7).hardwareTooHigh == false);          // within a step
            expect (wave::planSafeGain (40.0, 24.8).hardwareTooHigh == true);
        }

        beginTest ("Safe gain: nothing is added until the mic has been read");
        {
            const auto plan = wave::planSafeGain (40.0, std::nullopt);
            expectWithinAbsoluteError (plan.makeupDb, 0.0, 1.0e-12);
            expectWithinAbsoluteError (plan.targetDb, 24.0, 1.0e-12);
            expect (! plan.hardwareTooHigh);
        }

        beginTest ("Safe gain: a turned dial keeps the level, and going lower is still quieter");
        {
            expectWithinAbsoluteError (wave::planSafeGain (40.0, 30.0).makeupDb, 10.0, 1.0e-9);    // dial up: the makeup shrinks
            expectWithinAbsoluteError (wave::planSafeGain (40.0, 40.0).makeupDb, 0.0, 1.0e-9);
            expectWithinAbsoluteError (wave::planSafeGain (40.0, 12.0).makeupDb, 16.0, 1.0e-9);    // dial down: the makeup stays
            expectWithinAbsoluteError (wave::planSafeGain (40.0, 0.0).makeupDb, 16.0, 1.0e-9);
        }

        beginTest ("Safe gain: a mic that was already lower than the safe level is not raised");
        {
            const auto plan = wave::planSafeGain (20.0, 20.0);
            expectWithinAbsoluteError (plan.targetDb, 20.0, 1.0e-12);
            expectWithinAbsoluteError (plan.makeupDb, 0.0, 1.0e-12);
            expect (! plan.hardwareTooHigh);

            expectWithinAbsoluteError (wave::planSafeGain (30.0, 24.0).makeupDb, 6.0, 1.0e-9);     // was at 30: gives back 6
            expectWithinAbsoluteError (wave::planSafeGain (100.0, 40.0).targetDb, 24.0, 1.0e-12);  // nonsense baselines are clamped
            expectWithinAbsoluteError (wave::planSafeGain (-5.0, 0.0).targetDb, 0.0, 1.0e-12);
            expect (wave::planSafeGain (40.0, 24.0).makeupDb <= wave::kMaxMakeupDb);
        }
    }
};

static WaveUsbTests waveUsbTests;
}
