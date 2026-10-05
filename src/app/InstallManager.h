// InstallManager: what the app and its setup screens know about installing. It wraps the Installer with the facts of
// this process (is it an AppImage, which one, is Headroom already installed, is this one newer), runs the install on a
// background thread, and carries the app-level actions the screens ask for (open the installed Headroom, uninstall) to
// whoever owns the process (Main.cpp), which is the only place that may quit the app.
#pragma once

#include "Juce.h"
#include "app/Install.h"

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

namespace hr
{
class InstallManager
{
public:
    struct Setup
    {
        install::Locations locations = install::Locations::forCurrentUser();
        std::shared_ptr<install::CommandRunner> runner;         // null = the real system
        juce::File appImage;                                    // the AppImage this process runs from ($APPIMAGE); empty = not one
        std::vector<juce::File> settingsFolders;                // what "delete my settings" removes
        bool supported = false;                                 // installing works on this system at all (Linux, systemd)
        std::function<void()> quitRunning;                      // ask another running Headroom to save and quit, and wait
        std::function<void (const juce::String&)> log;
        int pauseAfterWirePlumberMs = 2000, serviceCheckMs = 1000;
        juce::File udevRule { "/etc/udev/rules.d/70-headroom-wave.rules" };     // tests and the screenshot run
        juce::String pkexec;                                    // tests: which pkexec (empty = the system's)
    };

    // The real one for this process: this user's home, the system runner, $APPIMAGE.
    static Setup realSetup();

    // Asks the Headroom that is running (another process) to save and quit, and waits up to 8 s until it is gone.
    static void quitRunningHeadroom();

    explicit InstallManager (Setup setup);
    ~InstallManager();

    // ---- Facts -----------------------------------------------------------------------------------------
    bool isSupported() const noexcept                           { return setup.supported; }
    bool runsFromAppImage() const                               { return setup.appImage != juce::File() && setup.appImage.existsAsFile(); }
    const juce::File& getAppImage() const noexcept              { return setup.appImage; }
    bool canInstall() const                                     { return isSupported() && runsFromAppImage(); }
    install::Info inspect() const                               { return installer.inspect(); }
    bool isInstalled() const                                    { return installer.inspect().installed(); }
    bool isThisTheInstalledCopy() const;                        // this AppImage is the file the service starts
    bool isInManagedFolder() const;                             // this AppImage lives where Gear Lever and friends keep it
    // Setup and uninstall buttons are shown when there is something to set up or remove.
    bool offersSetupAndUninstall() const                        { return isSupported() && isInstalled(); }

    // True when Headroom is not installed and this is an AppImage that could install itself: the wizard opens by itself.
    bool needsSetup() const                                     { return canInstall() && ! isInstalled(); }

    // Runs the installed program with --version (a moment, and an AppImage mounts itself for it): "" when not installed.
    juce::String installedVersion();

    // This is an AppImage, an older one is installed (not one an AppImage manager looks after), and this one is newer.
    bool offersUpdate (juce::String& installedVersionOut);

    // ---- Doing it --------------------------------------------------------------------------------------
    // What the setup wizard asks for. Which file gets installed is decided here: this AppImage, unless an AppImage of the same or
    // a newer version is installed already (a re-run of setup from an old download must not put an old version back).
    struct SetupRequest
    {
        bool keepInPlace = false;
        bool startAtLogin = true;
        bool startNow = false;          // the wizard is the running Headroom: it starts the service itself once it has quit
    };

    // Runs on a thread of its own; `done` is called on the message thread (not at all if the manager is gone by then).
    void installAsync (SetupRequest request, std::function<void (const install::Result&)> done);
    bool isBusy() const noexcept                                { return busy.load(); }

    // The "start when I log in" switch of Settings.
    void setStartAtLogin (bool enabled);

    // The screens ask; Main.cpp does (it owns the process): quit this one and start the installed Headroom with its window open.
    std::function<void (const juce::File& program)> onOpenInstalled;
    void openInstalled (const juce::File& program)              { if (onOpenInstalled) onOpenInstalled (program); }

    // Quit, take Headroom off the computer once everything is saved, and optionally delete the settings.
    std::function<void (bool deleteSettings)> onUninstall;
    void requestUninstall (bool deleteSettings)                 { if (onUninstall) onUninstall (deleteSettings); }

    install::Installer& getInstaller() noexcept                 { return installer; }
    const std::vector<juce::File>& getSettingsFolders() const   { return setup.settingsFolders; }

private:
    Setup setup;
    std::shared_ptr<install::CommandRunner> runner;
    install::Installer installer;
    std::thread worker;
    std::atomic<bool> busy { false };

    JUCE_DECLARE_WEAK_REFERENCEABLE (InstallManager)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InstallManager)
};
}
