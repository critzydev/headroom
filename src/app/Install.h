// Install: puts Headroom on a computer for one user, updates it and takes it off again. Everything here is plain
// file and `systemctl --user` work with no UI and no audio, so it runs the same from the setup wizard, from the unattended
// command-line modes and from the unit tests (a scratch home folder and a recording CommandRunner).
//
// Rules the code keeps: nothing needs root (the one exception, removing the udev rule that "Allow hardware controls" put in
// place, goes through a constant pkexec script); every step checks before it acts, so everything can run again; every step
// says what it did through the log; and only files that are Headroom's own are ever replaced or deleted.
#pragma once

#include "Juce.h"

#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace hr::install
{
// ---- Where everything goes ---------------------------------------------------------------------------------------
// All of it derives from one home folder, so a test can work in a scratch folder. The places are the ones install.sh uses.
struct Locations
{
    explicit Locations (juce::File homeFolder) : home (std::move (homeFolder)) {}
    static Locations forCurrentUser();                  // $HOME

    juce::File home;

    juce::File optDir() const               { return home.getChildFile (".local/opt/headroom"); }
    juce::File programCopy() const          { return optDir().getChildFile ("Headroom.AppImage"); }     // where the AppImage installs itself
    juce::File binLink() const              { return home.getChildFile (".local/bin/headroom"); }       // the tarball's symlink
    juce::File applicationsDir() const      { return home.getChildFile (".local/share/applications"); }
    juce::File desktopFile() const          { return applicationsDir().getChildFile ("headroom.desktop"); }
    juce::File iconThemeDir() const         { return home.getChildFile (".local/share/icons/hicolor"); }
    juce::File iconFile() const             { return iconThemeDir().getChildFile ("scalable/apps/headroom.svg"); }
    juce::File wireplumberDir() const       { return home.getChildFile (".config/wireplumber/wireplumber.conf.d"); }
    juce::File wireplumberDropIn() const    { return wireplumberDir().getChildFile ("51-headroom-wave.conf"); }
    juce::File unitDir() const              { return home.getChildFile (".config/systemd/user"); }
    juce::File unitFile() const             { return unitDir().getChildFile ("headroom.service"); }

    // Folders where an AppImage manager (Gear Lever, AppImageLauncher, appimaged) keeps the files it looks after.
    std::vector<juce::File> managedFolders() const;
};

// ---- The files Headroom writes ---------------------------------------------------------------------------------
// Compiled into the program, so that one AppImage is all there is. The menu entry and the unit are the files in packaging/
// with their Exec / ExecStart line replaced by the installed program.
struct Payload
{
    juce::String desktopEntry, unit, wireplumberConf, iconSvg;
    static Payload fromBinaryData();
    bool isComplete() const;
};

// ---- Running programs --------------------------------------------------------------------------------------------
struct CommandResult
{
    bool started = false;                   // the program could be run at all
    int exitCode = -1;
    juce::String output;                    // stdout and stderr together

    bool ok() const noexcept                { return started && exitCode == 0; }
};

class CommandRunner
{
public:
    virtual ~CommandRunner() = default;
    // Runs the program (looked up on PATH unless it has a path) and waits for it. Never throws; a program that is not there
    // or does not finish in time comes back as started == false or exitCode == -1.
    virtual CommandResult run (const juce::StringArray& command, int timeoutMs) = 0;
    // Is there such a program (on PATH, or at that path)?
    virtual bool exists (const juce::String& program) = 0;
};

std::unique_ptr<CommandRunner> makeSystemRunner();

// A runner that runs nothing and answers like a healthy desktop session. It keeps what it was asked. For the screenshot run
// (which walks the setup wizard in a scratch home) and for trying the installer out without touching the system.
class RecordingRunner final : public CommandRunner
{
public:
    CommandResult run (const juce::StringArray& command, int timeoutMs) override;
    bool exists (const juce::String&) override      { return true; }

    std::vector<juce::StringArray> getCalls() const;
    int count (const juce::String& fragment) const;    // calls whose words, joined by spaces, contain `fragment`

private:
    mutable std::mutex lock;
    std::vector<juce::StringArray> calls;
};

// ---- What is there now -------------------------------------------------------------------------------------------
struct Info
{
    bool unitPresent = false;               // ~/.config/systemd/user/headroom.service exists
    juce::File program;                     // what that unit starts (empty when unreadable)
    bool programPresent = false;            // ... and that file exists
    bool isAppImage = false;                // ... and is an AppImage (not the tarball's binary)
    bool managedElsewhere = false;          // ... and lives where an AppImage manager keeps it (not in Headroom's own folder)

    bool installed() const noexcept         { return unitPresent && programPresent; }
};

// ---- Doing it ----------------------------------------------------------------------------------------------------
struct InstallOptions
{
    juce::File source;                      // the AppImage to install: the running one ($APPIMAGE)
    bool keepInPlace = false;               // run it from where it is (Gear Lever and friends) instead of copying it
    bool startAtLogin = true;
    bool startNow = true;                   // (re)start the service at the end. The setup wizard says false: it is still the
                                            // running Headroom, and starts the service itself once it has quit.
};

struct UninstallOptions
{
    bool deleteSettings = false;
    std::vector<juce::File> settingsFolders;    // what "delete settings" removes (config and state folders of Headroom)
    bool askForPassword = true;                 // false: never run pkexec, only say how to remove the udev rule
};

struct Result
{
    bool ok = false;
    juce::String error;                     // one or two plain sentences, empty when ok
    juce::StringArray log;                  // what was done, one line each
    juce::File program;                     // install: what the service starts now
    bool wireplumberRestarted = false;
    bool serviceStarted = false;
    juce::String manualUdevCommand;         // uninstall: set when the udev rule is still there and has to be removed by hand
};

// The version this program says it is: HEADROOM_VERSION, or $HEADROOM_TEST_VERSION (tests only, to play a newer build).
juce::String currentVersion();

// "0.2.4" against "0.3.0": negative, zero, positive. Numbers by position; text after a '-' or '+' (a dev suffix) is ignored.
int compareVersions (const juce::String& a, const juce::String& b);

// The text a `--version` run prints, to compare with: "Headroom 0.2.4".
juce::String versionLine();

// What the unit and the menu entry look like for a program path (also what the tests check).
juce::String renderDesktopEntry (const juce::String& templateText, const juce::File& program);
juce::String renderUnit (const juce::String& templateText, const juce::File& program, const juce::File& home);
juce::File parseExecStart (const juce::String& unitText, const juce::File& home);

// The script `pkexec /bin/sh -c` runs to take the udev rule away again: a constant, nothing is put into it.
const char* udevRemovalScript();

class Installer
{
public:
    Installer (Locations locations, CommandRunner& runner, Payload payload);

    // Called with each line as it happens (the result's log has them too).
    void setLogger (std::function<void (const juce::String&)> logger)       { logLine = std::move (logger); }
    // Asks a running Headroom to save and quit and waits for it (the app uses SingleInstance for it).
    void setQuitRunning (std::function<void()> quit)                        { quitRunning = std::move (quit); }
    // Tests: where the udev rule lives, which pkexec to run (empty = look in the system folders) and how long to wait.
    void setUdevRule (juce::File rule)                                       { udevRule = std::move (rule); }
    void setPkexec (juce::String path)                                       { pkexecPath = std::move (path); }
    void setPauses (int afterWirePlumberMs, int serviceCheckMs)              { wirePlumberPauseMs = afterWirePlumberMs; serviceCheckPauseMs = serviceCheckMs; }

    const Locations& getLocations() const noexcept                          { return loc; }

    Info inspect() const;
    // Runs `<program> --version` (the program the service starts) and returns the number, "" if it did not answer.
    juce::String installedVersion (const juce::File& program);

    Result install (const InstallOptions& options);
    Result uninstall (const UninstallOptions& options);

    // The "start when I log in" switch: enable or disable the unit (nothing if Headroom is not installed). True if done.
    bool setStartAtLogin (bool enabled);
    bool isEnabledAtLogin();

    // Is this file in a folder where an AppImage manager looks after it?
    bool isInManagedFolder (const juce::File& file) const;

private:
    struct Run;                                 // the state of one install / uninstall

    CommandResult runSystemctl (std::initializer_list<const char*> args, int timeoutMs = 15000);
    void note (Run&, const juce::String& line);
    bool preflight (Run&);
    void migrateFromTarball (Run&);
    void clearDefaultIfOurs (Run&, const char* key);
    void removeUdevRule (Run&, const UninstallOptions&);

    Locations loc;
    CommandRunner& runner;
    Payload payload;
    std::function<void (const juce::String&)> logLine;
    std::function<void()> quitRunning;
    juce::File udevRule { "/etc/udev/rules.d/70-headroom-wave.rules" };
    juce::String pkexecPath;
    int wirePlumberPauseMs = 2000, serviceCheckPauseMs = 1000;
};
}
