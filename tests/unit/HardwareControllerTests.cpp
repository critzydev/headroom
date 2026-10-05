// Unit tests for the Wave:3 hardware side of the app: the new settings and their defaults, the demo backend's permission flow,
// ClipGuard reaching the backend (also at start-up), and Safe gain lowering the mic and adding the difference back in the engine.
#include "Juce.h"

#include "app/AppController.h"
#include "app/Paths.h"
#include "backend/NullBackend.h"
#include "backend/wave/WaveGain.h"

#include <array>
#include <cmath>
#include <vector>

// The tests only need to let async callbacks run (see PluginTests.cpp for why this is not runDispatchLoopUntil).
namespace juce::detail
{
bool dispatchNextMessageOnSystemQueue (bool returnIfNoPendingMessages);
}

namespace hr
{
namespace
{
    template <typename Condition>
    bool waitUntil (Condition&& condition, int timeoutMs)
    {
        const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;

        while (! condition())
        {
            if (juce::Time::getMillisecondCounter() > end)
                return false;

            const auto until = juce::Time::getMillisecondCounter() + 10;

            while (juce::Time::getMillisecondCounter() < until)
                if (! juce::detail::dispatchNextMessageOnSystemQueue (true))
                    juce::Thread::sleep (1);
        }

        return true;
    }

    // Lets the backend's queued notifications reach the controller.
    void pump()
    {
        const auto until = juce::Time::getMillisecondCounter() + 60;

        while (juce::Time::getMillisecondCounter() < until)
            if (! juce::detail::dispatchNextMessageOnSystemQueue (true))
                juce::Thread::sleep (2);
    }

    struct TempDir
    {
        TempDir() : dir (juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("headroom-hwtest", "", false))
        {
            dir.createDirectory();
        }

        ~TempDir()      { dir.deleteRecursively(); }

        juce::File dir;
    };

    // Points Paths at a throw-away root, so a controller with a real state file never meets the user's own files.
    struct ScopedRoot
    {
        explicit ScopedRoot (const juce::File& root)    { Paths::setRootOverride (root); }
        ~ScopedRoot()                                   { Paths::setRootOverride ({}); }
    };

    AppController::Options demoOptions()
    {
        AppController::Options options;
        options.demo = true;            // NullBackend, never started: no threads
        options.persist = false;
        options.startBackend = false;
        options.safeGainRetryMs = 0;    // Safe gain does not wait between its tries here
        return options;
    }

    // What the engine's mic input meter shows for a DC level fed into the mic: post-trim, before any effect.
    float micInputLevel (AppController& controller, float dc)
    {
        std::vector<float> in (2048, dc), a (2048), b (2048), c (2048), d (2048);

        EngineIO io;
        io.numFrames = 2048;
        io.sampleRate = 48000.0;
        io.micIn[0] = in.data();
        io.monitorOut[0] = a.data();  io.monitorOut[1] = b.data();
        io.streamOut[0] = c.data();   io.streamOut[1] = d.data();

        auto& engine = controller.getEngine();

        for (int i = 0; i < 4; ++i)     // the trim is smoothed over 20 ms
            engine.process (io);

        engine.readChannelLevels (kMicChannel);
        engine.process (io);
        return engine.readChannelLevels (kMicChannel).input.l;
    }

    NullBackend& nullBackend (AppController& controller)
    {
        auto* backend = dynamic_cast<NullBackend*> (&controller.getBackend());
        jassert (backend != nullptr);
        return *backend;
    }
}

class HardwareControllerTests final : public juce::UnitTest
{
public:
    HardwareControllerTests() : juce::UnitTest ("Wave:3 hardware settings", "headroom") {}

    void runTest() override
    {
        settingsTests();
        permissionFlowTests();
        clipGuardTests();
        safeGainTests();
        restoreMicGainTests();
    }

private:
    void settingsTests()
    {
        beginTest ("new settings: defaults, and a state.xml from before them gets the defaults");
        {
            AppState fresh;
            expectEquals ((int) fresh.getSetting (ids::clipGuard), -1);             // never chosen: the mic is left alone
            expectEquals ((int) fresh.getSetting (ids::restoreClipGuard), 1);       // re-apply at start-up: on
            expectEquals ((int) fresh.getSetting (ids::safeGain), 0);
            expectWithinAbsoluteError ((double) fresh.getSetting (ids::safeGainBase), 40.0, 1.0e-9);

            const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("headroom-hwstate", ".xml", false);
            file.replaceWithText ("<HEADROOM version=\"1\"><SETTINGS latency=\"128\" micGain=\"0.75\"/></HEADROOM>");

            AppState old (file);
            expect (old.load() == AppState::LoadResult::Loaded);
            expectEquals ((int) old.getSetting (ids::latency), 128);
            expectEquals ((int) old.getSetting (ids::clipGuard), -1);
            expectEquals ((int) old.getSetting (ids::restoreClipGuard), 1);
            expectEquals ((int) old.getSetting (ids::safeGain), 0);

            old.setSetting (ids::clipGuard, 1);
            old.setSetting (ids::safeGain, 1);
            old.setSetting (ids::safeGainBase, 30.5);
            old.saveNow();

            AppState again (file);
            again.load();
            expectEquals ((int) again.getSetting (ids::clipGuard), 1);
            expectEquals ((int) again.getSetting (ids::safeGain), 1);
            expectWithinAbsoluteError ((double) again.getSetting (ids::safeGainBase), 30.5, 1.0e-9);

            file.deleteFile();
        }
    }

    void permissionFlowTests()
    {
        beginTest ("demo backend: no permission until it is asked for, then ClipGuard reads off and can be set");
        {
            AppController controller (demoOptions());
            controller.start();
            auto& backend = nullBackend (controller);

            auto hw = controller.getHardwareControls();
            expect (hw.usbAvailable);                       // the fake Wave:3 is the input
            expect (! hw.usbPermission);
            expect (! hw.clipGuard.has_value());

            backend.setClipGuard (true);                    // no permission: it waits, and nothing shows
            expect (! controller.getHardwareControls().clipGuard.has_value());

            controller.requestHardwarePermission();
            hw = controller.getHardwareControls();
            expect (hw.usbPermission);
            expect (hw.clipGuard == std::optional<bool> (true));        // what was waiting was applied

            backend.setClipGuard (false);
            expect (controller.getHardwareControls().clipGuard == std::optional<bool> (false));
            backend.setClipGuard (true);
            expect (controller.getHardwareControls().clipGuard == std::optional<bool> (true));

            // A fresh demo Wave:3 reads ClipGuard off.
            AppController other (demoOptions());
            other.start();
            other.requestHardwarePermission();
            expect (other.getHardwareControls().clipGuard == std::optional<bool> (false));
        }

        beginTest ("demo backend: it is only the Wave:3 when the Wave:3 is the input");
        {
            AppController controller (demoOptions());
            controller.start();
            expect (controller.getHardwareControls().usbAvailable);

            controller.setInputDevice ("alsa_input.usb-Generic_USB_Webcam_DEMO-02.analog-stereo");
            expect (! controller.getHardwareControls().usbAvailable);

            controller.setInputDevice ("auto");
            expect (controller.getHardwareControls().usbAvailable);
        }
    }

    void clipGuardTests()
    {
        beginTest ("ClipGuard: the toggle is remembered and reaches the backend");
        {
            AppController controller (demoOptions());
            controller.start();
            controller.requestHardwarePermission();
            auto& state = controller.getState();

            controller.setClipGuard (true);
            expectEquals ((int) state.getSetting (ids::clipGuard), 1);
            expect (controller.getHardwareControls().clipGuard == std::optional<bool> (true));

            controller.setClipGuard (false);
            expectEquals ((int) state.getSetting (ids::clipGuard), 0);
            expect (controller.getHardwareControls().clipGuard == std::optional<bool> (false));

            // The model changed straight on the tree (the way a loaded state.xml does) is picked up too.
            state.setSetting (ids::clipGuard, 1);
            expect (controller.getHardwareControls().clipGuard == std::optional<bool> (true));

            // Pressing it when the setting already says so still tells the backend: the mic may have forgotten.
            nullBackend (controller).setClipGuard (false);
            expect (controller.getHardwareControls().clipGuard == std::optional<bool> (false));
            controller.setClipGuard (true);
            expect (controller.getHardwareControls().clipGuard == std::optional<bool> (true));
        }

        beginTest ("ClipGuard at start-up: put back the way it was, only if asked and only if it was ever chosen");
        {
            // The saved settings come from a state.xml, as they do in real life: loaded before anything listens.
            const auto started = [] (int saved, int restore)
            {
                TempDir root;
                ScopedRoot scope (root.dir);

                const auto file = root.dir.getChildFile ("state.xml");
                file.replaceWithText ("<HEADROOM version=\"1\"><SETTINGS clipGuard=\"" + juce::String (saved)
                                      + "\" restoreClipGuard=\"" + juce::String (restore) + "\"/></HEADROOM>");

                auto options = demoOptions();
                options.persist = true;
                options.stateFile = file;

                AppController controller (options);
                controller.start();
                controller.requestHardwarePermission();       // the permission arrives after start, as it does in real life
                return controller.getHardwareControls().clipGuard;
            };

            expect (started (1, 1) == std::optional<bool> (true));      // saved on, re-apply on: restored
            expect (started (1, 0) == std::optional<bool> (false));     // re-apply off: the mic keeps what it says (demo: off)
            expect (started (-1, 1) == std::optional<bool> (false));    // never chosen: nothing to restore
            expect (started (0, 1) == std::optional<bool> (false));

            // Switching "re-apply" on later pushes the saved state at once.
            TempDir root;
            ScopedRoot scope (root.dir);
            const auto file = root.dir.getChildFile ("state.xml");
            file.replaceWithText ("<HEADROOM version=\"1\"><SETTINGS clipGuard=\"1\" restoreClipGuard=\"0\"/></HEADROOM>");

            auto options = demoOptions();
            options.persist = true;
            options.stateFile = file;

            AppController controller (options);
            controller.start();
            controller.requestHardwarePermission();
            expect (controller.getHardwareControls().clipGuard == std::optional<bool> (false));
            controller.setRestoreClipGuard (true);
            expect (controller.getHardwareControls().clipGuard == std::optional<bool> (true));
        }
    }

    // A controller that loaded `settings` from a state.xml (so nothing was listening while they loaded, as at a real start).
    static std::unique_ptr<AppController> controllerWith (const juce::File& file, const juce::String& settings, int permissionGraceMs = 5000)
    {
        file.replaceWithText ("<HEADROOM version=\"1\"><SETTINGS " + settings + "/></HEADROOM>");

        auto options = demoOptions();
        options.persist = true;
        options.stateFile = file;
        options.safeGainPermissionGraceMs = permissionGraceMs;
        return std::make_unique<AppController> (options);
    }

    void safeGainTests()
    {
        beginTest ("Safe gain: it does not start without the USB permission");
        {
            AppController controller (demoOptions());
            controller.start();
            auto& backend = nullBackend (controller);

            controller.setHardwareMicGain (1.0f);
            pump();
            expect (! controller.getHardwareControls().usbPermission);

            controller.setSafeGain (true);
            pump();

            expectEquals ((int) controller.getState().getSetting (ids::safeGain), 0);
            expect (! controller.getSafeGainInfo().on);
            expectWithinAbsoluteError (backend.getStatus().hardwareMicGain, 1.0f, 1.0e-6f);        // the mic was not touched
            expectWithinAbsoluteError (controller.getSafeGainInfo().makeupDb, 0.0f, 1.0e-6f);

            // With the permission it does.
            controller.requestHardwarePermission();
            controller.setSafeGain (true);
            expectEquals ((int) controller.getState().getSetting (ids::safeGain), 1);
            expectWithinAbsoluteError (backend.getStatus().hardwareMicGain, 0.5412f, 1.0e-3f);
        }

        beginTest ("Safe gain: the mic goes to +24 dB and Headroom adds the +16 dB back");
        {
            AppController controller (demoOptions());
            controller.start();
            controller.requestHardwarePermission();
            auto& backend = nullBackend (controller);
            auto& state = controller.getState();

            controller.setHardwareMicGain (1.0f);           // the mic at its maximum, +40 dB
            pump();
            expectWithinAbsoluteError (micInputLevel (controller, 0.1f), 0.1f, 1.0e-4f);

            controller.setSafeGain (true);
            expectWithinAbsoluteError ((double) state.getSetting (ids::safeGainBase), 40.0, 1.0e-9);

            // The mic was told to go to +24 dB (54 % on the cubic slider) ...
            expectWithinAbsoluteError (backend.getStatus().hardwareMicGain, 0.5412f, 1.0e-3f);
            expectWithinAbsoluteError (wave::sliderToHardwareDb ((double) backend.getStatus().hardwareMicGain), 24.0, 0.02);

            // ... and once the mic itself has said so over USB, the engine adds the difference on the mic channel.
            expect (waitUntil ([&] { return controller.getSafeGainInfo().makeupDb > 15.0f; }, 2000));

            const auto info = controller.getSafeGainInfo();
            expect (info.available && info.on && info.hardwareKnown);
            expectWithinAbsoluteError (info.hardwareDb, 24.0f, 0.05f);
            expectWithinAbsoluteError (info.makeupDb, 16.0f, 0.05f);
            expect (info.problem.isEmpty());

            expectWithinAbsoluteError (micInputLevel (controller, 0.1f), 0.1f * std::pow (10.0f, 16.0f / 20.0f), 0.005f);

            // The slider's stored value is a memory now, not a command: moving it does not touch the mic.
            controller.setHardwareMicGain (0.9f);
            pump();
            expectWithinAbsoluteError (backend.getStatus().hardwareMicGain, 0.5412f, 1.0e-3f);
            expectWithinAbsoluteError ((double) state.getSetting (ids::micGain), 0.9, 1.0e-6);

            // Off: the mic goes back to where it was, the makeup goes.
            controller.setSafeGain (false);
            expectWithinAbsoluteError (backend.getStatus().hardwareMicGain, 1.0f, 1.0e-4f);
            expect (waitUntil ([&] { return controller.getSafeGainInfo().makeupDb < 0.05f; }, 2000));
            expectWithinAbsoluteError (micInputLevel (controller, 0.1f), 0.1f, 1.0e-4f);
            expect (! controller.getSafeGainInfo().on);
        }

        beginTest ("Safe gain: it puts the mic back to where it was, not to a fixed level");
        {
            AppController controller (demoOptions());
            controller.start();
            controller.requestHardwarePermission();
            auto& backend = nullBackend (controller);

            backend.setHardwareMicGain ((float) wave::hardwareDbToSlider (30.0));       // the mic at +30 dB
            pump();
            controller.setSafeGain (true);
            expectWithinAbsoluteError ((double) controller.getState().getSetting (ids::safeGainBase), 30.0, 1.0e-9);

            expect (waitUntil ([&] { return controller.getSafeGainInfo().makeupDb > 5.0f; }, 2000));
            expectWithinAbsoluteError (controller.getSafeGainInfo().makeupDb, 6.0f, 0.05f);      // 30 - 24

            controller.setSafeGain (false);
            expectWithinAbsoluteError (wave::sliderToHardwareDb ((double) backend.getStatus().hardwareMicGain), 30.0, 0.02);
        }

        beginTest ("Safe gain: a mic that is already at a lower gain is not raised, and gets no makeup");
        {
            AppController controller (demoOptions());
            controller.start();
            controller.requestHardwarePermission();
            auto& backend = nullBackend (controller);

            backend.setHardwareMicGain ((float) wave::hardwareDbToSlider (18.0));
            pump();
            controller.setSafeGain (true);
            pump();

            expectWithinAbsoluteError (wave::sliderToHardwareDb ((double) backend.getStatus().hardwareMicGain), 18.0, 0.02);
            expectWithinAbsoluteError (controller.getSafeGainInfo().makeupDb, 0.0f, 1.0e-6f);
            expectWithinAbsoluteError (micInputLevel (controller, 0.1f), 0.1f, 1.0e-4f);
        }

        beginTest ("Safe gain: the mic's own word over USB decides, so a locked mic gets no makeup");
        {
            AppController controller (demoOptions());
            controller.start();
            controller.requestHardwarePermission();
            auto& backend = nullBackend (controller);

            controller.setHardwareMicGain (1.0f);
            backend.setDemoUsbGain (40.0f, true);           // it says +40 whatever we ask, and that it ignores us
            pump();
            controller.setSafeGain (true);
            pump();

            // The kernel's copy says +24 (that is what was asked), the mic says +40: no makeup on top of a mic that did not move.
            expectWithinAbsoluteError (wave::sliderToHardwareDb ((double) backend.getStatus().hardwareMicGain), 24.0, 0.02);
            expectWithinAbsoluteError (controller.getSafeGainInfo().makeupDb, 0.0f, 1.0e-6f);
            expectWithinAbsoluteError (micInputLevel (controller, 0.1f), 0.1f, 1.0e-4f);

            // After a few tries it says why, in words; still no makeup.
            for (int i = 0; i < 5; ++i)
            {
                backend.setDemoUsbGain (40.0f, true);
                pump();
            }

            const auto info = controller.getSafeGainInfo();
            expect (info.problem.contains ("ignore gain changes"), info.problem);
            expectWithinAbsoluteError (info.makeupDb, 0.0f, 1.0e-6f);
            expectWithinAbsoluteError (micInputLevel (controller, 0.1f), 0.1f, 1.0e-4f);

            // Same, but the mic is not locked and simply did not take it.
            backend.setDemoUsbGain (40.0f, false);
            pump();
            expect (controller.getSafeGainInfo().problem.contains ("did not take"));
        }

        beginTest ("Safe gain: no makeup without the mic's confirmation, whatever the kernel says");
        {
            TempDir root;
            ScopedRoot scope (root.dir);

            // Switched on in the saved settings, but the permission is not there (yet): the mic is at +24 as far as the kernel
            // knows, and nothing confirms it.
            auto controller = controllerWith (root.dir.getChildFile ("state.xml"), "safeGain=\"1\" safeGainBase=\"40\"", 60000);
            controller->start();
            auto& backend = nullBackend (*controller);

            backend.setHardwareMicGain ((float) wave::hardwareDbToSlider (24.0));
            pump();

            expectWithinAbsoluteError (controller->getSafeGainInfo().makeupDb, 0.0f, 1.0e-6f);
            expectWithinAbsoluteError (micInputLevel (*controller, 0.1f), 0.1f, 1.0e-4f);
            expectEquals ((int) controller->getState().getSetting (ids::safeGain), 1);      // still waiting: not switched off yet

            // The moment the mic can be asked, the makeup follows.
            controller->requestHardwarePermission();
            expect (waitUntil ([&] { return std::abs (controller->getSafeGainInfo().makeupDb - 16.0f) < 0.05f; }, 2000));
        }

        beginTest ("Safe gain: when the permission goes away it switches itself off, cleanly, and says so once");
        {
            AppController::Options options = demoOptions();
            options.safeGainPermissionGraceMs = 0;
            AppController controller (options);
            controller.start();
            controller.requestHardwarePermission();
            auto& backend = nullBackend (controller);

            controller.setHardwareMicGain (1.0f);
            pump();
            controller.setSafeGain (true);
            expect (waitUntil ([&] { return std::abs (controller.getSafeGainInfo().makeupDb - 16.0f) < 0.05f; }, 2000));
            expect (controller.getNotices().empty());

            backend.revokeHardwarePermission();             // the udev rule was removed
            expect (waitUntil ([&] { return ! controller.getSafeGainInfo().on; }, 2000));

            expectEquals ((int) controller.getState().getSetting (ids::safeGain), 0);
            expectWithinAbsoluteError (controller.getSafeGainInfo().makeupDb, 0.0f, 1.0e-6f);
            expectWithinAbsoluteError (micInputLevel (controller, 0.1f), 0.1f, 1.0e-4f);                       // trim back to 0
            expectWithinAbsoluteError (wave::sliderToHardwareDb ((double) backend.getStatus().hardwareMicGain), 24.0, 0.02);   // the mic's gain is left alone

            int notices = 0;

            for (const auto& notice : controller.getNotices())
                if (notice.id == "safe-gain-permission")
                {
                    ++notices;
                    expect (notice.text.contains ("Safe gain was switched off"));
                }

            expectEquals (notices, 1);

            // Said once: more changes do not say it again, and getting the permission back does not switch it on by itself.
            controller.dismissNotice ("safe-gain-permission");
            pump();
            backend.setDemoUsbGain (24.0f, false);
            controller.requestHardwarePermission();
            pump();
            expect (controller.getNotices().empty());
            expectEquals ((int) controller.getState().getSetting (ids::safeGain), 0);
            expectWithinAbsoluteError (controller.getSafeGainInfo().makeupDb, 0.0f, 1.0e-6f);
        }

        beginTest ("Safe gain: a permission that is missing only for a moment (a replugged mic) does not switch it off");
        {
            AppController controller (demoOptions());       // the default grace period
            controller.start();
            controller.requestHardwarePermission();
            auto& backend = nullBackend (controller);

            controller.setHardwareMicGain (1.0f);
            pump();
            controller.setSafeGain (true);
            expect (waitUntil ([&] { return controller.getSafeGainInfo().makeupDb > 15.0f; }, 2000));

            backend.revokeHardwarePermission();
            pump();
            expectEquals ((int) controller.getState().getSetting (ids::safeGain), 1);           // waiting
            expectWithinAbsoluteError (controller.getSafeGainInfo().makeupDb, 0.0f, 1.0e-6f);   // but nothing is added on faith

            controller.requestHardwarePermission();
            expect (waitUntil ([&] { return controller.getSafeGainInfo().makeupDb > 15.0f; }, 2000));
            expect (controller.getNotices().empty());
        }

        beginTest ("Safe gain: when the mic reports the lower gain over USB, the makeup follows it, and follows the dial");
        {
            AppController controller (demoOptions());
            controller.start();
            controller.requestHardwarePermission();
            auto& backend = nullBackend (controller);

            controller.setHardwareMicGain (1.0f);
            backend.setDemoUsbGain (40.0f, false);
            pump();
            controller.setSafeGain (true);
            backend.setDemoUsbGain (24.0f, false);          // the mic took it
            expect (waitUntil ([&] { return std::abs (controller.getSafeGainInfo().makeupDb - 16.0f) < 0.05f; }, 2000));
            expect (controller.getSafeGainInfo().problem.isEmpty());

            backend.setDemoUsbGain (30.0f, false);          // somebody turned the dial up
            expect (waitUntil ([&] { return std::abs (controller.getSafeGainInfo().makeupDb - 10.0f) < 0.05f; }, 2000));

            backend.setDemoUsbGain (std::nullopt, std::nullopt);    // and the mic follows what it was asked again (+24)
            expect (waitUntil ([&] { return std::abs (controller.getSafeGainInfo().makeupDb - 16.0f) < 0.05f; }, 2000));
        }

        beginTest ("Safe gain: another mic than the Wave:3 gets no makeup and no changes to its gain");
        {
            AppController controller (demoOptions());
            controller.start();
            controller.requestHardwarePermission();
            auto& backend = nullBackend (controller);

            controller.setHardwareMicGain (1.0f);
            pump();
            controller.setInputDevice ("alsa_input.usb-Generic_USB_Webcam_DEMO-02.analog-stereo");
            controller.setSafeGain (true);
            pump();

            expect (! controller.getSafeGainInfo().available);
            expectWithinAbsoluteError (controller.getSafeGainInfo().makeupDb, 0.0f, 1.0e-6f);
            expectWithinAbsoluteError (backend.getStatus().hardwareMicGain, 1.0f, 1.0e-6f);
            expectWithinAbsoluteError (micInputLevel (controller, 0.1f), 0.1f, 1.0e-4f);

            // Back on the Wave:3, it starts.
            controller.setInputDevice ("auto");
            expect (waitUntil ([&] { return controller.getSafeGainInfo().makeupDb > 15.0f; }, 2000));
        }

        beginTest ("Safe gain: switched on in the saved settings, it starts with the app");
        {
            TempDir root;
            ScopedRoot scope (root.dir);

            auto controller = controllerWith (root.dir.getChildFile ("state.xml"), "safeGain=\"1\" safeGainBase=\"40\"");
            controller->start();
            controller->requestHardwarePermission();
            auto& backend = nullBackend (*controller);

            // The mic has just powered up at its maximum (not the slider: while Safe gain is on that is only a memory).
            backend.setHardwareMicGain (1.0f);
            expect (waitUntil ([&] { return std::abs (wave::sliderToHardwareDb ((double) backend.getStatus().hardwareMicGain) - 24.0) < 0.05; }, 2000));
            expect (waitUntil ([&] { return std::abs (controller->getSafeGainInfo().makeupDb - 16.0f) < 0.05f; }, 2000));
        }
    }

    void restoreMicGainTests()
    {
        beginTest ("Restore mic gain: at start, the stored gain is put back through the ordinary path");
        {
            TempDir root;
            ScopedRoot scope (root.dir);

            auto controller = controllerWith (root.dir.getChildFile ("state.xml"), "restoreMicGain=\"1\" micGain=\"0.8\"");
            auto& backend = nullBackend (*controller);
            expectWithinAbsoluteError (backend.getStatus().hardwareMicGain, 0.5f, 1.0e-6f);      // the demo mic starts at 50 %

            controller->start();
            expectWithinAbsoluteError (backend.getStatus().hardwareMicGain, 0.8f, 1.0e-6f);
            expectWithinAbsoluteError ((double) controller->getState().getSetting (ids::micGain), 0.8, 1.0e-6);
        }

        beginTest ("Restore mic gain: off means the mic keeps what it has");
        {
            TempDir root;
            ScopedRoot scope (root.dir);

            auto controller = controllerWith (root.dir.getChildFile ("state.xml"), "restoreMicGain=\"0\" micGain=\"0.8\"");
            controller->start();
            expectWithinAbsoluteError (nullBackend (*controller).getStatus().hardwareMicGain, 0.5f, 1.0e-6f);
        }

        beginTest ("Restore mic gain: Safe gain wins");
        {
            TempDir root;
            ScopedRoot scope (root.dir);

            auto controller = controllerWith (root.dir.getChildFile ("state.xml"), "restoreMicGain=\"1\" micGain=\"0.8\" safeGain=\"1\" safeGainBase=\"40\"", 60000);
            controller->start();
            auto& backend = nullBackend (*controller);

            expectWithinAbsoluteError (backend.getStatus().hardwareMicGain, 0.5f, 1.0e-6f);       // not put back to 80 %
            expectWithinAbsoluteError ((double) controller->getState().getSetting (ids::micGain), 0.8, 1.0e-6);
        }

        beginTest ("Restore mic gain: once per appearance of the mic; a replug puts it back again, with the latest slider value");
        {
            TempDir root;
            ScopedRoot scope (root.dir);

            auto controller = controllerWith (root.dir.getChildFile ("state.xml"), "restoreMicGain=\"1\" micGain=\"0.8\"");
            controller->start();
            auto& backend = nullBackend (*controller);
            expectWithinAbsoluteError (backend.getStatus().hardwareMicGain, 0.8f, 1.0e-6f);

            // The user moves the slider: the stored value follows, and the mic.
            controller->setHardwareMicGain (0.6f);
            expectWithinAbsoluteError ((double) controller->getState().getSetting (ids::micGain), 0.6, 1.0e-6);
            expectWithinAbsoluteError (backend.getStatus().hardwareMicGain, 0.6f, 1.0e-6f);

            // Other changes, and the dial being turned, do not bring the stored value back: it was done for this appearance.
            backend.setHardwareMicGain (0.3f);
            controller->setLatency (128);
            pump();
            expectWithinAbsoluteError (backend.getStatus().hardwareMicGain, 0.3f, 1.0e-6f);

            // Unplugged, plugged in again: it powers up at its maximum, and the stored gain (0.6) is put back.
            backend.setDemoMicPresent (false);
            pump();
            expectWithinAbsoluteError (backend.getStatus().hardwareMicGain, -1.0f, 1.0e-6f);
            backend.setDemoMicPresent (true, 1.0f);
            expect (waitUntil ([&] { return std::abs (backend.getStatus().hardwareMicGain - 0.6f) < 1.0e-4f; }, 2000));
        }

        beginTest ("Restore mic gain: switching it on keeps the mic the way it is now");
        {
            AppController controller (demoOptions());
            controller.start();
            auto& backend = nullBackend (controller);

            // The stored value is the slider's first position; the mic is somewhere else (powered up, or the dial).
            backend.setHardwareMicGain (0.7f);
            pump();
            expectWithinAbsoluteError ((double) controller.getState().getSetting (ids::micGain), 0.5, 1.0e-6);

            controller.setRestoreMicGain (true);
            expectWithinAbsoluteError ((double) controller.getState().getSetting (ids::micGain), 0.7, 1.0e-6);
            expectWithinAbsoluteError (backend.getStatus().hardwareMicGain, 0.7f, 1.0e-6f);
        }

        beginTest ("Restore mic gain: it is off by default, and the first time the user moves the gain slider it switches itself on");
        {
            AppController controller (demoOptions());
            controller.start();
            auto& backend = nullBackend (controller);
            auto& state = controller.getState();

            expectEquals ((int) state.getSetting (ids::restoreMicGain), 0);
            expectEquals ((int) state.getSetting (ids::restoreMicGainChosen), 0);

            controller.userSetHardwareMicGain (0.6f);
            expectEquals ((int) state.getSetting (ids::restoreMicGain), 1);
            expectEquals ((int) state.getSetting (ids::restoreMicGainChosen), 1);
            expectWithinAbsoluteError ((double) state.getSetting (ids::micGain), 0.6, 1.0e-6);     // the user's value, not the mic's old level
            expectWithinAbsoluteError (backend.getStatus().hardwareMicGain, 0.6f, 1.0e-6f);

            controller.userSetHardwareMicGain (0.4f);                   // the next moves just move the gain
            expectEquals ((int) state.getSetting (ids::restoreMicGain), 1);
            expectWithinAbsoluteError ((double) state.getSetting (ids::micGain), 0.4, 1.0e-6);
            expectWithinAbsoluteError (backend.getStatus().hardwareMicGain, 0.4f, 1.0e-6f);
        }

        beginTest ("Restore mic gain: when the user switches it off afterwards, it stays off");
        {
            AppController controller (demoOptions());
            controller.start();
            auto& state = controller.getState();

            controller.userSetHardwareMicGain (0.6f);
            expectEquals ((int) state.getSetting (ids::restoreMicGain), 1);

            controller.setRestoreMicGain (false);
            controller.userSetHardwareMicGain (0.3f);
            controller.userSetHardwareMicGain (0.8f);
            expectEquals ((int) state.getSetting (ids::restoreMicGain), 0);
            expectWithinAbsoluteError ((double) state.getSetting (ids::micGain), 0.8, 1.0e-6);
        }

        beginTest ("Restore mic gain: a choice made before the user moved the slider (on or off) is respected");
        {
            AppController off (demoOptions());
            off.start();
            off.setRestoreMicGain (false);                              // chosen: off
            off.userSetHardwareMicGain (0.6f);
            expectEquals ((int) off.getState().getSetting (ids::restoreMicGain), 0);

            AppController on (demoOptions());
            on.start();
            on.setRestoreMicGain (true);
            on.setRestoreMicGain (false);
            on.setRestoreMicGain (true);
            on.userSetHardwareMicGain (0.6f);
            expectEquals ((int) on.getState().getSetting (ids::restoreMicGain), 1);
        }

        beginTest ("Restore mic gain: only the user's slider switches it on, not a programmatic change");
        {
            AppController controller (demoOptions());
            controller.start();

            controller.setHardwareMicGain (0.7f);                       // the screenshot runner, Safe gain's own writes, ...
            nullBackend (controller).setHardwareMicGain (0.2f);         // the dial on the mic
            pump();

            expectEquals ((int) controller.getState().getSetting (ids::restoreMicGain), 0);
            expectEquals ((int) controller.getState().getSetting (ids::restoreMicGainChosen), 0);
        }

        beginTest ("Restore mic gain: Safe gain wins, the slider does not switch it on while Safe gain is on");
        {
            TempDir root;
            ScopedRoot scope (root.dir);

            auto controller = controllerWith (root.dir.getChildFile ("state.xml"), "safeGain=\"1\" safeGainBase=\"40\"", 60000);
            controller->start();
            controller->userSetHardwareMicGain (0.6f);

            expectEquals ((int) controller->getState().getSetting (ids::restoreMicGain), 0);
            expectEquals ((int) controller->getState().getSetting (ids::restoreMicGainChosen), 0);
            expectWithinAbsoluteError ((double) controller->getState().getSetting (ids::micGain), 0.6, 1.0e-6);      // only a memory
        }

        beginTest ("Restore mic gain: the automatic switch-on is saved, and the next start puts the gain back");
        {
            TempDir root;
            ScopedRoot scope (root.dir);
            const auto file = root.dir.getChildFile ("state.xml");

            {
                auto controller = controllerWith (file, "latency=\"256\"");
                controller->start();
                controller->userSetHardwareMicGain (0.6f);
            }                                                           // saved when the controller goes (a normal quit)

            const auto saved = file.loadFileAsString();
            expect (saved.contains ("restoreMicGain=\"1\""));
            expect (saved.contains ("restoreMicGainChosen=\"1\""));

            auto options = demoOptions();
            options.persist = true;
            options.stateFile = file;
            AppController next (options);
            auto& backend = nullBackend (next);
            expectWithinAbsoluteError (backend.getStatus().hardwareMicGain, 0.5f, 1.0e-6f);      // the demo mic starts at 50 %
            next.start();
            expectWithinAbsoluteError (backend.getStatus().hardwareMicGain, 0.6f, 1.0e-6f);
        }

        beginTest ("Restore mic gain: a state.xml from before the flag gets it (0) and loads");
        {
            const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("headroom-hwstate2", ".xml", false);
            file.replaceWithText ("<HEADROOM version=\"1\"><SETTINGS restoreMicGain=\"1\" micGain=\"0.75\"/></HEADROOM>");

            AppState old (file);
            expect (old.load() == AppState::LoadResult::Loaded);
            expectEquals ((int) old.getSetting (ids::restoreMicGain), 1);
            expectEquals ((int) old.getSetting (ids::restoreMicGainChosen), 0);
            file.deleteFile();
        }
    }
};

static HardwareControllerTests hardwareControllerTests;
}
