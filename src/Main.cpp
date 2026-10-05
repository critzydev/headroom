// Headroom entry point: command-line modes, the main window, the setup / update / uninstall flows and the
// screenshot / plugin-scanner helpers.
#include "Juce.h"

#include "app/AppController.h"
#include "app/Install.h"
#include "app/InstallManager.h"
#include "app/Log.h"
#include "app/Paths.h"
#include "app/QuitSignals.h"
#include "app/SingleInstance.h"
#include "plugins/CrashGuard.h"
#include "plugins/ScanWorker.h"
#include "ui/HeadroomLookAndFeel.h"
#include "ui/Icons.h"
#include "ui/MainComponent.h"
#include "ui/ScreenshotRunner.h"
#include "ui/Theme.h"
#include "ui/Toast.h"
#include "ui/UiScale.h"
#include "ui/setup/SetupSheets.h"

#include <csignal>
#include <cstdio>

namespace hr
{
namespace
{
    struct CommandLine
    {
        bool show = false;
        bool background = false;
        bool quit = false;
        bool demo = false;
        bool demoMicMuted = false;
        bool version = false;

        // --setup: the setup wizard (also what a first launch of an AppImage that is not installed shows).
        bool setup = false;

        // --setup-unattended [options] / --uninstall-unattended [options]: no window, for scripts and CI.
        bool setupUnattended = false, uninstallUnattended = false;
        juce::File source;                      // --source <AppImage>: what to install when $APPIMAGE is not set
        bool keepInPlace = false;               // --keep-in-place
        bool noLoginStart = false;              // --no-login-start
        bool noDefaultDevices = false;          // --no-default-devices (Headroom is not made the default device)
        bool noKeepDefault = false;             // --no-keep-default
        bool noStart = false;                   // --no-start (do not start the service at the end)
        bool deleteSettings = false;            // --delete-settings (uninstall)
        bool noPassword = false;                // --no-password (uninstall never runs pkexec)

        bool scanPlugin = false;
        juce::String scanFormat, scanId;

        bool screenshot = false;
        juce::File screenshotDir;

        // --dev-single-instance-child <name> [eventFile]: test helper, see runSingleInstanceChild().
        juce::String singleInstanceChildName;
        juce::File singleInstanceChildEvents;

        juce::StringArray unknown;

        static CommandLine parse (const juce::StringArray& args)
        {
            CommandLine cli;

            for (int i = 0; i < args.size(); ++i)
            {
                const auto& arg = args[i];

                if (arg == "--show")                    cli.show = true;
                else if (arg == "--background")         cli.background = true;
                else if (arg == "--quit")               cli.quit = true;
                else if (arg == "--demo")               cli.demo = true;
                else if (arg == "--demo-mic-muted")     cli.demoMicMuted = true;
                else if (arg == "--version")            cli.version = true;
                else if (arg == "--setup")              cli.setup = true;
                else if (arg == "--setup-unattended")   cli.setupUnattended = true;
                else if (arg == "--uninstall-unattended") cli.uninstallUnattended = true;
                else if (arg == "--source" && i + 1 < args.size())
                    cli.source = juce::File::getCurrentWorkingDirectory().getChildFile (args[++i]);
                else if (arg == "--keep-in-place")      cli.keepInPlace = true;
                else if (arg == "--no-login-start")     cli.noLoginStart = true;
                else if (arg == "--no-default-devices") cli.noDefaultDevices = true;
                else if (arg == "--no-keep-default")    cli.noKeepDefault = true;
                else if (arg == "--no-start")           cli.noStart = true;
                else if (arg == "--delete-settings")    cli.deleteSettings = true;
                else if (arg == "--no-password")        cli.noPassword = true;
                else if (arg == "--scan-plugin" && i + 2 < args.size())
                {
                    cli.scanPlugin = true;
                    cli.scanFormat = args[++i];
                    cli.scanId = args[++i];
                }
                else if (arg == "--screenshot" && i + 1 < args.size())
                {
                    cli.screenshot = true;
                    cli.screenshotDir = juce::File::getCurrentWorkingDirectory().getChildFile (args[++i]);
                }
                else if (arg == "--dev-single-instance-child" && i + 1 < args.size())
                {
                    cli.singleInstanceChildName = args[++i];

                    if (i + 1 < args.size())
                        cli.singleInstanceChildEvents = juce::File::getCurrentWorkingDirectory().getChildFile (args[++i]);
                }
                else
                {
                    cli.unknown.add (arg);
                }
            }

            return cli;
        }
    };

    // The main window. Closing it only hides it on Linux: the audio engine keeps running.
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        MainWindow (AppController& c, InstallManager* installManager)
            : juce::DocumentWindow ("Headroom", theme::bg0, juce::DocumentWindow::allButtons), controller (c)
        {
            setUsingNativeTitleBar (true);
            main = new MainComponent (controller, installManager);
            setContentOwned (main, true);
            setResizable (true, false);
            setResizeLimits (theme::kMinWidth, theme::kMinHeight, 8192, 8192);
            centreWithSize (getWidth(), getHeight());
        }

        MainComponent& getMain() noexcept   { return *main; }

        // The limits are for the whole window; the spec's minimum (860 x 540) is for the content, so add the frame.
        void applyMinimumSize()
        {
            if (auto* peer = getPeer())
            {
                const auto frame = peer->getFrameSize();
                setResizeLimits (theme::kMinWidth + frame.getLeftAndRight(), theme::kMinHeight + frame.getTopAndBottom(), 8192, 8192);
            }
        }

        // True while this process has nothing to keep running for: an AppImage that is not installed. Closing the window then ends it.
        std::function<bool()> closeEndsTheApp;

        void closeButtonPressed() override
        {
#if JUCE_LINUX
            if (closeEndsTheApp && closeEndsTheApp())
            {
                juce::JUCEApplication::getInstance()->systemRequestedQuit();
                return;
            }

            // Audio keeps running with the window gone. Say so the first time, in a toast that outlives the window.
            static const juce::Identifier closeToastShown ("closeToastShown");
            auto& state = controller.getState();

            if ((int) state.getSetting (closeToastShown) == 0)
            {
                state.setSetting (closeToastShown, 1);
                ToastWindow::show ("Headroom keeps running in the background. Open it again from your app menu.");
            }

            setVisible (false);
#else
            juce::JUCEApplication::getInstance()->systemRequestedQuit();     // Windows dev builds just quit
#endif
        }

    private:
        AppController& controller;
        MainComponent* main = nullptr;
        juce::TooltipWindow tooltips { nullptr, 600 };      // hover tips for every component in the app

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

    // A small window for the one question that comes before Headroom itself starts: "Update Headroom?"
    class UpdateWindow final : public juce::DocumentWindow
    {
    public:
        UpdateWindow (std::unique_ptr<UpdateSheet> sheet, std::function<void()> onClosed)
            : juce::DocumentWindow ("Headroom", theme::bg0, juce::DocumentWindow::closeButton), closed (std::move (onClosed))
        {
            setUsingNativeTitleBar (true);
            host = new SheetHost();
            setContentOwned (host, true);
            setResizable (false, false);
            centreWithSize (560, 300);
            host->show (std::move (sheet), UpdateSheet::kWidth, UpdateSheet::kHeight);
            setVisible (true);
            toFront (true);
        }

        void closeButtonPressed() override      { if (closed) closed(); }

    private:
        SheetHost* host = nullptr;
        std::function<void()> closed;
    };
}

class HeadroomApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override        { return "Headroom"; }
    const juce::String getApplicationVersion() override     { return install::currentVersion(); }

    // JUCE's anotherInstanceStarted never fires on Linux, so single-instance is done by SingleInstance instead.
    bool moreThanOneInstanceAllowed() override              { return true; }

    void initialise (const juce::String&) override
    {
        // The plugin scan worker (and the plugin dev tools) run as bare processes: nothing of the app may start
        // before them, above all not settings, logging or the crash guard.
        if (PluginCli::runIfRequested (getCommandLineParameterArray()))
            return;

        const auto cli = CommandLine::parse (getCommandLineParameterArray());

        if (cli.version)
        {
            std::fputs ((install::versionLine() + "\n").toRawUTF8(), stdout);
            std::fflush (stdout);
            setApplicationReturnValue (0);
            quit();
            return;
        }

        if (cli.scanPlugin)
        {
            runPluginScanner (cli);
            return;
        }

        if (cli.screenshot)
        {
            startScreenshotMode (cli);
            return;
        }

        // From here on the process either is the one Headroom of this user or hands over to it. The helper modes
        // above never take part, and neither do the dev tools that PluginCli handled.
        if (cli.singleInstanceChildName.isNotEmpty())
        {
            runSingleInstanceChild (cli);
            return;
        }

        if (cli.setupUnattended)
        {
            runUnattendedSetup (cli);
            return;
        }

        if (cli.uninstallUnattended)
        {
            runUnattendedUninstall (cli);
            return;
        }

        if (cli.quit)
        {
            requestRunningInstanceToQuit();
            return;
        }

        // What the screens know about installing. Only the modes that show a window use it.
        installManager = std::make_unique<InstallManager> (InstallManager::realSetup());
        installManager->onOpenInstalled = [this] (const juce::File& program) { handOverTo = program; quit(); };
        installManager->onUninstall = [this] (bool deleteSettings)
        {
            uninstallAfterShutdown = true;
            deleteSettingsWithUninstall = deleteSettings;
            quit();
        };

        // An AppImage newer than the installed Headroom offers to update it before anything else starts.
        if (! cli.background && ! cli.demo && ! cli.setup)
        {
            juce::String installedVersion;

            if (installManager->offersUpdate (installedVersion))
            {
                offerUpdate (cli, installedVersion);
                return;
            }
        }

        startApplication (cli);
    }

    void shutdown() override
    {
        // Stop answering right away, but keep the lock until everything is saved: a launch in the meantime waits for it
        // and then starts normally, instead of being told "ok" by a process that is about to exit.
        if (singleInstance != nullptr)
            singleInstance->stopListening();

        updateWindow.reset();
        window.reset();
        screenshotRunner.reset();
        controller.reset();

        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        lookAndFeel.reset();
        Icons::releaseAll();
        theme::releaseFonts();
        singleInstance.reset();                     // the lock is free: whatever starts next may be the primary

        // The installed Headroom takes over from the setup wizard (the service starts and shows its window), or Headroom is
        // taken off the computer now that everything is saved and the nodes are gone.
        if (uninstallAfterShutdown && installManager != nullptr)
            finishUninstall();
        else if (handOverTo != juce::File())
            startInstalled (handOverTo);

        Log::shutdown();
        installManager.reset();

        if (screenshotRoot != juce::File() && ! keepScreenshotRoot)
            screenshotRoot.deleteRecursively();
    }

    void systemRequestedQuit() override                     { quit(); }
    void anotherInstanceStarted (const juce::String&) override {}

private:
    enum class SetupMode { none, firstRun, rerun };

    void installLookAndFeel()
    {
        lookAndFeel = std::make_unique<HeadroomLookAndFeel>();
        juce::LookAndFeel::setDefaultLookAndFeel (lookAndFeel.get());
    }

    // The part of start-up that makes this process Headroom: election, log, controller, window.
    void startApplication (const CommandLine& cli)
    {
        // First launch of an AppImage that is not installed: the wizard; --setup: the wizard, set up again when installed.
        SetupMode setupMode = SetupMode::none;

        if (! cli.background && ! cli.demo)
        {
            if (installManager->needsSetup())
                setupMode = SetupMode::firstRun;
            else if (cli.setup)
                setupMode = installManager->canInstall() && ! installManager->isInstalled() ? SetupMode::firstRun : SetupMode::rerun;
        }

        singleInstance = std::make_unique<SingleInstance>();

        if (! claimPrimary (cli))
        {
            quit();
            return;
        }

        singleInstance->onShowRequested = [this] { showWindow(); };
        singleInstance->onQuitRequested = [] { juce::JUCEApplicationBase::quit(); };
        singleInstance->onSetupRequested = [this] { showWindow(); openSetup (SetupMode::rerun); };

        // Only the primary logs to the shared file and installs the crash guard.
        Log::init (Paths::getLogFile());

        for (const auto& arg : cli.unknown)
            Log::warn ("Ignoring unknown command-line argument: " + arg);

        CrashGuard::install (Paths::getCrashFile());

        // SIGTERM (what `systemctl --user stop` sends at logout, at shutdown and from install.sh) and SIGINT get the same orderly
        // quit as --quit: state saved, PipeWire nodes removed, status 0. JUCE handles neither on Linux.
        QuitSignals::install ([] (int signalNumber)
        {
            Log::info (juce::String ("Got ") + (signalNumber == SIGINT ? "SIGINT" : "SIGTERM") + ": saving and quitting");
            juce::JUCEApplicationBase::quit();
        });

        installLookAndFeel();

        AppController::Options options;
        options.demo = cli.demo;
        options.demoMicMuted = cli.demoMicMuted;
        options.holdDefaults = setupMode == SetupMode::firstRun;       // not the default device until the person has chosen

        controller = std::make_unique<AppController> (options);
        controller->start();

        // Test hook (tests/integration/run.sh, the SIGTERM check): HEADROOM_TEST_STORE_RULE_AFTER_MS=<ms> stores a routing rule
        // that long after start-up and says so in the log, so that the test has a change that is not saved yet to send SIGTERM after.
        if (const int storeRuleAfterMs = juce::SystemStats::getEnvironmentVariable ("HEADROOM_TEST_STORE_RULE_AFTER_MS", {}).getIntValue();
            storeRuleAfterMs > 0)
        {
            juce::Timer::callAfterDelay (storeRuleAfterMs, [this]
            {
                if (controller != nullptr)
                {
                    controller->getState().setRule ("hr-it-sigterm", "music");
                    Log::info ("test hook: stored the routing rule hr-it-sigterm");
                }
            });
        }

        // Test hook (tests/integration/run.sh, the "more than one output" scenarios): HEADROOM_TEST_OUTPUTS="<ms>,<headphones>,<stream>;..."
        // sets what the Headphones and Stream mixes play to, <ms> after start-up, once per entry, and says so in the log ("test hook 1: ...",
        // numbered in the order of the entries). The two lists are node.names joined with |. There is no other way to change a running
        // Headroom from a script.
        int stepNumber = 0;

        for (const auto& step : juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("HEADROOM_TEST_OUTPUTS", {}), ";", ""))
        {
            ++stepNumber;
            const auto afterMs = step.upToFirstOccurrenceOf (",", false, false).getIntValue();
            const auto lists = step.fromFirstOccurrenceOf (",", false, false);
            const auto headphones = lists.upToFirstOccurrenceOf (",", false, false);
            const auto stream = lists.fromFirstOccurrenceOf (",", false, false);

            if (afterMs <= 0)
                continue;

            juce::Timer::callAfterDelay (afterMs, [this, headphones, stream, stepNumber]
            {
                if (controller != nullptr)
                {
                    controller->setMixOutputs (Mix::Monitor, AppState::splitOutputs (headphones));
                    controller->setMixOutputs (Mix::Stream, AppState::splitOutputs (stream));
                    Log::info ("test hook " + juce::String (stepNumber) + ": the Headphones mix plays to " + AppState::joinOutputs (controller->getState().getOutputs (Mix::Monitor))
                               + ", the Stream mix also to " + AppState::joinOutputs (controller->getState().getOutputs (Mix::Stream)));
                }
            });
        }

        // HiDPI: the user's UI size, or automatic = what the desktop is set to. JUCE does not see KDE's
        // Xwayland scale, so Xft.dpi / kwinrc / GDK_SCALE are read here; MainComponent keeps it current when the
        // setting changes.
        juce::Desktop::getInstance().setGlobalScaleFactor (UiScale::effectiveScale (controller->getState().getSetting (ids::uiScale)));

        if (! cli.background)
            showWindow();

        if (setupMode != SetupMode::none)
            openSetup (setupMode);
    }

    // The window, with the setup wizard on top of it.
    void openSetup (SetupMode mode)
    {
        if (window == nullptr || mode == SetupMode::none)
            return;

        window->getMain().showSetupWizard (mode == SetupMode::firstRun ? SetupSheet::Mode::firstRun : SetupSheet::Mode::rerun);
    }

    void showWindow()
    {
        if (controller == nullptr)
            return;

        if (window == nullptr)
        {
            window = std::make_unique<MainWindow> (*controller, installManager.get());
            window->closeEndsTheApp = [this] { return installManager != nullptr && installManager->needsSetup(); };
        }

        if (window->isMinimised())
            window->setMinimised (false);

        window->setVisible (true);
        window->applyMinimumSize();
        window->toFront (true);
    }

    // True when this process is now the primary and should go on to run the app; false when it has handed over
    // to the primary (or found there is nothing to do) and should just exit.
    bool claimPrimary (const CommandLine& cli)
    {
        auto& instance = *singleInstance;

        if (cli.background)
        {
            if (instance.tryBecomePrimary())
                return true;

            Log::info ("Headroom is already running, so this copy has nothing to do.");
            return false;
        }

        // (none) / --show / --demo / --setup from here. --setup asks a running Headroom to open its wizard instead of its window.
        const juce::String request = cli.setup ? "setup" : "show";

       #if JUCE_LINUX
        // The menu entry runs `--show`. With the login service installed but not running, start it and let it show the
        // window: the audio engine then lives in the service and keeps running when the window is closed.
        if (! cli.demo && ! cli.setup && ! instance.isPrimaryRunning() && startUserService() && instance.sendShowToPrimary (3000))
            return false;
       #endif

        // Become the primary, or hand over to it. It may still be starting, or just have been told to quit (then its lock
        // is gone within moments and we take over), so keep trying for a while before giving up.
        const auto giveUpAt = juce::Time::getMillisecondCounter() + 5000;

        for (;;)
        {
            if (instance.tryBecomePrimary())
                return true;

            if (instance.sendCommand (request, 700) == "ok")
                return false;

            if (juce::Time::getMillisecondCounter() >= giveUpAt)
                break;
        }

        Log::warn ("Headroom is already running but did not answer; leaving it alone.");
        return false;
    }

   #if JUCE_LINUX
    // `systemctl --user start headroom.service`, but only if the unit is installed. True if systemctl succeeded.
    static bool startUserService()
    {
        const auto unit = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile (".config/systemd/user/headroom.service");

        if (! unit.existsAsFile())
            return false;

        Log::info ("Headroom is not running; starting headroom.service");

        juce::ChildProcess systemctl;

        if (! systemctl.start (juce::StringArray { "systemctl", "--user", "start", "headroom.service" }))
            return false;

        if (! systemctl.waitForProcessToFinish (5000))
        {
            systemctl.kill();
            return false;
        }

        return systemctl.getExitCode() == 0;
    }
   #endif

    // --quit: ask the running Headroom to save and quit, and wait until it is gone. Exits 0 either way: there being
    // nothing to quit is fine (the uninstaller runs this).
    void requestRunningInstanceToQuit()
    {
        SingleInstance instance;

        if (instance.isPrimaryRunning())
        {
            if (instance.sendCommand ("quit", 3000) == "ok")
            {
                const auto giveUpAt = juce::Time::getMillisecondCounter() + 5000;

                while (instance.isPrimaryRunning() && juce::Time::getMillisecondCounter() < giveUpAt)
                    juce::Thread::sleep (100);
            }
            else
            {
                Log::warn ("Headroom is running but did not answer.");
            }
        }

        setApplicationReturnValue (0);
        quit();
    }

    // ---- Setup, update and uninstall ---------------------------------------------------------------

    // Starts the installed Headroom with its window open: `<installed program> --show` starts the login service when nothing runs,
    // and asks the running one to show itself. Detached, with its output to /dev/null: this process is about to exit, and a
    // closed pipe must not kill the start-up of the new one.
    static void startInstalled (const juce::File& program)
    {
        if (program == juce::File() || ! program.existsAsFile())
            return;

        juce::ChildProcess process;
        process.start (juce::StringArray { program.getFullPathName(), "--show" }, 0);
    }

    // The Headroom that has already been installed is older than this AppImage: offer to update it. "Not now" goes on with the
    // normal start (which shows the installed, older one when it runs).
    void offerUpdate (const CommandLine& cli, const juce::String& installedVersion)
    {
        installLookAndFeel();
        const bool enabledAtLogin = installManager->getInstaller().isEnabledAtLogin();

        auto sheet = std::make_unique<UpdateSheet> (install::currentVersion(), installedVersion,
                                                    [this, enabledAtLogin] { runUpdate (enabledAtLogin); },
                                                    [this, cli] { declineUpdate (cli); });
        updateSheet = sheet.get();
        updateWindow = std::make_unique<UpdateWindow> (std::move (sheet), [this, cli] { declineUpdate (cli); });
    }

    void declineUpdate (const CommandLine& cli)
    {
        if (updateSheet == nullptr || updateSheet->isBusy())
            return;

        updateSheet = nullptr;
        juce::MessageManager::callAsync ([this, cli]
        {
            updateWindow.reset();
            startApplication (cli);
        });
    }

    void runUpdate (bool enabledAtLogin)
    {
        updateSheet->setBusy();

        InstallManager::SetupRequest request;
        request.startAtLogin = enabledAtLogin;      // an update changes nothing about starting at login
        request.startNow = true;                    // the running Headroom quits, and the service starts the new one

        installManager->installAsync (request, [this] (const install::Result& result)
        {
            if (! result.ok)
            {
                if (updateSheet != nullptr)
                    updateSheet->showError (result.error);

                return;
            }

            handOverTo = result.program;
            quit();
        });
    }

    // --setup-unattended: install with the defaults into $HOME, no window (CI, scripts). Options: --source <AppImage>,
    // --keep-in-place, --no-login-start, --no-default-devices, --no-keep-default, --no-start. Exits 0 when it worked.
    void runUnattendedSetup (const CommandLine& cli)
    {
        auto setup = InstallManager::realSetup();
        auto source = cli.source != juce::File() ? cli.source : setup.appImage;

        install::InstallOptions options;
        options.source = source;
        options.keepInPlace = cli.keepInPlace;
        options.startAtLogin = ! cli.noLoginStart;
        options.startNow = ! cli.noStart;

        const auto printLine = [] (const juce::String& line) { std::fputs ((line + "\n").toRawUTF8(), stdout); std::fflush (stdout); };

        if (! source.existsAsFile())
        {
            std::fputs ("Headroom has no AppImage to install. Run this from the AppImage, or give --source <file>.\n", stderr);
            setApplicationReturnValue (2);
            quit();
            return;
        }

        // The choices that live in the settings go in the same place the wizard puts them: the model (no engine is started here).
        if (cli.noLoginStart || cli.noDefaultDevices || cli.noKeepDefault)
        {
            AppState state (Paths::getStateFile());
            state.load();
            state.setSetting (ids::startAtLogin, cli.noLoginStart ? 0 : 1);

            if (cli.noDefaultDevices)
                state.setSetting (ids::makeDefault, 0);

            if (cli.noDefaultDevices || cli.noKeepDefault)
                state.setSetting (ids::keepDefault, 0);

            state.saveNow();
            printLine ("Settings saved");
        }

        auto runner = install::makeSystemRunner();
        install::Installer installer (install::Locations::forCurrentUser(), *runner, install::Payload::fromBinaryData());
        installer.setLogger (printLine);
        installer.setQuitRunning ([] { InstallManager::quitRunningHeadroom(); });

        const auto result = installer.install (options);

        if (! result.ok)
            std::fputs ((result.error + "\n").toRawUTF8(), stderr);

        setApplicationReturnValue (result.ok ? 0 : 1);
        quit();
    }

    // --uninstall-unattended [--delete-settings] [--no-password]: the same removal as the Settings button, without a window.
    void runUnattendedUninstall (const CommandLine& cli)
    {
        const auto printLine = [] (const juce::String& line) { std::fputs ((line + "\n").toRawUTF8(), stdout); std::fflush (stdout); };

        auto runner = install::makeSystemRunner();
        install::Installer installer (install::Locations::forCurrentUser(), *runner, install::Payload::fromBinaryData());
        installer.setLogger (printLine);
        installer.setQuitRunning ([] { InstallManager::quitRunningHeadroom(); });

        // The installer test (tests/integration/appimage-test.sh) moves the udev rule to a scratch file and stands in for pkexec. Only
        // this mode reads these; the Settings button never does, so nothing but a test can aim the root script anywhere.
        if (const auto rule = juce::SystemStats::getEnvironmentVariable ("HEADROOM_TEST_UDEV_RULE", {}); rule.isNotEmpty())
            installer.setUdevRule (juce::File (rule));

        if (const auto pkexec = juce::SystemStats::getEnvironmentVariable ("HEADROOM_TEST_PKEXEC", {}); pkexec.isNotEmpty())
            installer.setPkexec (pkexec);

        install::UninstallOptions options;
        options.deleteSettings = cli.deleteSettings;
        options.settingsFolders = { Paths::getConfigDirPath(), Paths::getStateDirPath() };
        options.askForPassword = ! cli.noPassword;

        const auto result = installer.uninstall (options);

        if (result.manualUdevCommand.isNotEmpty())
            printLine ("To finish, run: " + result.manualUdevCommand);

        setApplicationReturnValue (result.ok ? 0 : 1);
        quit();
    }

    // The Settings button: Headroom has quit and saved (that is why this runs from shutdown()); now it comes off the computer.
    void finishUninstall()
    {
        install::UninstallOptions options;
        options.deleteSettings = deleteSettingsWithUninstall;
        options.settingsFolders = installManager->getSettingsFolders();

        Log::info ("Uninstalling Headroom" + juce::String (deleteSettingsWithUninstall ? " and deleting its settings" : ""));
        const auto result = installManager->getInstaller().uninstall (options);

        // Nobody is looking at a window any more: say it was done (best effort; not every desktop has notify-send).
        juce::ChildProcess notify;
        notify.start (juce::StringArray { "notify-send", "Headroom", result.ok ? "Headroom has been removed." : "Headroom could not be removed completely." }, 0);
    }

    // --dev-single-instance-child <name> [eventFile]: what tests/unit/SingleInstanceTests.cpp starts. It becomes the primary
    // of an instance called <name>, answers commands until it is told to quit, and appends what happened to eventFile, one
    // word per line: primary, not-primary, show, quit. It ends by itself after 90 s so a test that dies leaves nothing behind.
    void runSingleInstanceChild (const CommandLine& cli)
    {
        singleInstance = std::make_unique<SingleInstance> (cli.singleInstanceChildName);

        const auto events = cli.singleInstanceChildEvents;
        const auto note = [events] (const juce::String& what)
        {
            if (events != juce::File())
                events.appendText (what + "\n");
        };

        if (! singleInstance->tryBecomePrimary())
        {
            note ("not-primary");
            setApplicationReturnValue (3);
            quit();
            return;
        }

        singleInstance->onShowRequested = [note] { note ("show"); };
        singleInstance->onQuitRequested = [note] { note ("quit"); juce::JUCEApplicationBase::quit(); };
        juce::Timer::callAfterDelay (90000, [] { juce::JUCEApplicationBase::quit(); });
        note ("primary");
    }

    // --scan-plugin <format> <fileOrIdentifier>: print the plugin descriptions as XML and exit. No GUI, no backend,
    // and nothing else on stdout: the parent parses it. Runs out of process so a crash costs one file.
    void runPluginScanner (const CommandLine& cli)
    {
        juce::AudioPluginFormatManager formats;
        juce::addDefaultFormatsToManager (formats);

        juce::XmlElement list ("LIST");

        for (auto* format : formats.getFormats())
        {
            if (format->getName() != cli.scanFormat)
                continue;

            juce::OwnedArray<juce::PluginDescription> found;
            format->findAllTypesForFile (found, cli.scanId);

            for (auto* description : found)
                if (auto xml = description->createXml())
                    list.addChildElement (xml.release());
        }

        const auto text = list.toString (juce::XmlElement::TextFormat().withoutHeader());
        std::fputs (text.toRawUTF8(), stdout);
        std::fflush (stdout);

        setApplicationReturnValue (0);
        quit();
    }

    // --screenshot <dir>: demo backend, fixed model in a throw-away config dir, run the engine ~1.5 s so meters
    // move, then render every view at 1x and 2x and exit. Works without a display on Linux.
    void startScreenshotMode (const CommandLine& cli)
    {
        screenshotRoot = juce::File::createTempFile ("headroom-screenshot");
        screenshotRoot.createDirectory();
        Paths::setRootOverride (screenshotRoot);     // never touch the user's real settings or log

        Log::init (Paths::getLogFile());
        installLookAndFeel();

        screenshotRunner = std::make_unique<ScreenshotRunner> (cli.screenshotDir, [this] (bool ok)
        {
            if (! ok)
            {
                setApplicationReturnValue (1);
                keepScreenshotRoot = true;
            }

            quit();
        });

        screenshotRunner->start();
    }

    std::unique_ptr<HeadroomLookAndFeel> lookAndFeel;
    std::unique_ptr<SingleInstance> singleInstance;
    std::unique_ptr<InstallManager> installManager;
    std::unique_ptr<AppController> controller;
    std::unique_ptr<MainWindow> window;

    std::unique_ptr<UpdateWindow> updateWindow;
    UpdateSheet* updateSheet = nullptr;
    juce::File handOverTo;                          // after shutdown: start this installed Headroom with its window open
    bool uninstallAfterShutdown = false, deleteSettingsWithUninstall = false;

    std::unique_ptr<ScreenshotRunner> screenshotRunner;
    juce::File screenshotRoot;
    bool keepScreenshotRoot = false;
};
}

START_JUCE_APPLICATION (hr::HeadroomApplication)
