// Unit tests for app/Install: the self-install, update and uninstall of the AppImage, in a scratch home folder with a recording
// stand-in for systemctl and friends. Nothing here touches the real home or runs a real program.
#include "Juce.h"

#include "app/Install.h"

#include <functional>
#include <map>
#include <vector>

namespace hr
{
namespace
{
    using namespace install;

    // Records every command and answers like a healthy desktop session unless told otherwise.
    class FakeRunner final : public CommandRunner
    {
    public:
        std::vector<juce::StringArray> calls;
        std::map<juce::String, juce::String> versions;     // program path -> what its --version prints
        juce::StringArray absent;                          // programs that do not exist
        juce::String metadata;                             // what `pw-metadata -n default` prints
        bool sessionWorks = true, pipewireThere = true, serviceActive = true, enabled = true;
        int pkexecExit = 0;
        std::function<void()> onPkexecSuccess;

        CommandResult run (const juce::StringArray& command, int) override
        {
            calls.push_back (command);
            CommandResult r;
            r.started = true;
            r.exitCode = 0;
            const auto joined = command.joinIntoString (" ");

            if (command[0] == "systemctl")
            {
                if (joined.contains ("show-environment"))                       r.exitCode = sessionWorks ? 0 : 1;
                else if (joined.contains ("list-unit-files pipewire.service"))  r.output = pipewireThere ? "pipewire.service enabled enabled\n" : "0 unit files listed.\n";
                else if (joined.contains ("is-active"))                         r.exitCode = serviceActive ? 0 : 3;
                else if (joined.contains ("is-enabled"))                        r.exitCode = enabled ? 0 : 1;
            }
            else if (command[0] == "pw-metadata" && command.size() == 3)
            {
                r.output = metadata;
            }
            else if (command[command.size() - 1] == "--version")
            {
                const auto it = versions.find (command[command.size() - 2]);

                if (it != versions.end())
                    r.output = "Headroom " + it->second + "\n";
                else
                    r.exitCode = 255;
            }
            else if (command[0].endsWith ("pkexec"))
            {
                r.exitCode = pkexecExit;

                if (pkexecExit == 0 && onPkexecSuccess)
                    onPkexecSuccess();
            }

            return r;
        }

        bool exists (const juce::String& program) override      { return ! absent.contains (program); }

        int count (const juce::String& a, const juce::String& b = {}) const
        {
            int n = 0;

            for (const auto& call : calls)
            {
                const auto joined = call.joinIntoString (" ");

                if (joined.contains (a) && joined.contains (b))
                    ++n;
            }

            return n;
        }
    };

    struct Scratch
    {
        Scratch() : root (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("hr-install-" + juce::Uuid().toString().substring (0, 8)))
        {
            home = root.getChildFile ("home");
            home.createDirectory();
        }

        ~Scratch()      { root.deleteRecursively(); }

        juce::File makeAppImage (const juce::String& where, const juce::String& content)
        {
            auto file = home.getChildFile (where);
            file.getParentDirectory().createDirectory();
            file.replaceWithText (content);
            return file;
        }

        juce::File root, home;
    };

    struct Rig
    {
        Rig() : loc (scratch.home), installer (loc, runner, Payload::fromBinaryData())
        {
            installer.setPauses (0, 0);
            installer.setLogger ([this] (const juce::String& line) { lines.add (line); });
            installer.setQuitRunning ([this] { ++quitCalls; });
            installer.setUdevRule (scratch.root.getChildFile ("70-headroom-wave.rules"));
            installer.setPkexec ("/usr/bin/pkexec");
        }

        Scratch scratch;
        Locations loc;
        FakeRunner runner;
        Installer installer;
        juce::StringArray lines;
        int quitCalls = 0;
    };

    juce::String text (const juce::File& file)      { return file.loadFileAsString(); }
}

class InstallTests final : public juce::UnitTest
{
public:
    InstallTests() : juce::UnitTest ("Install", "headroom") {}

    void runTest() override
    {
        pieces();
        freshInstall();
        again();
        keepInPlace();
        tarballMigration();
        update();
        uninstallKeepsOthers();
        uninstallSettingsAndDefaults();
        udevRule();
        failures();
    }

private:
    void pieces()
    {
        beginTest ("the packaged files are in the program and have the lines the installer replaces");
        {
            const auto payload = Payload::fromBinaryData();
            expect (payload.isComplete());
            expect (payload.desktopEntry.contains ("\nExec=") || payload.desktopEntry.startsWith ("Exec="));
            expect (payload.unit.contains ("[Service]") && payload.unit.contains ("Restart=on-failure"));
            expect (payload.wireplumberConf.contains ("0x0fd9"));
            expect (payload.iconSvg.contains ("<svg"));
        }

        beginTest ("version numbers compare by position and ignore dev suffixes");
        {
            expect (compareVersions ("0.2.4", "0.3.0") < 0);
            expect (compareVersions ("0.3.0", "0.2.4") > 0);
            expect (compareVersions ("0.2.4", "0.2.4") == 0);
            expect (compareVersions ("0.2.10", "0.2.9") > 0);
            expect (compareVersions ("1.0", "1.0.0") == 0);
            expect (compareVersions ("0.2.4-dev.abc1234", "0.2.4") == 0);
            expect (compareVersions ("1.0.0", "0.9.9") > 0);
            expect (compareVersions ("", "0.1") < 0);
        }

        beginTest ("the unit and the menu entry start the installed program, quoted where a path needs it");
        {
            const auto payload = Payload::fromBinaryData();
            const juce::File home ("/home/user");

            const auto plain = renderUnit (payload.unit, home.getChildFile (".local/opt/headroom/Headroom.AppImage"), home);
            expect (plain.contains ("\nExecStart=%h/.local/opt/headroom/Headroom.AppImage --background\n"), plain);
            expect (plain.contains ("Restart=on-failure") && plain.contains ("StartLimitBurst=6"));
            expect (plain.contains ("WantedBy=graphical-session.target"));

            const auto spaced = renderUnit (payload.unit, home.getChildFile ("My Apps/100% Headroom.AppImage"), home);
            expect (spaced.contains ("\nExecStart=\"%h/My Apps/100%% Headroom.AppImage\" --background\n"), spaced);

            const auto outside = renderUnit (payload.unit, juce::File ("/opt/hr/Headroom.AppImage"), home);
           #if ! JUCE_WINDOWS
            expect (outside.contains ("\nExecStart=/opt/hr/Headroom.AppImage --background\n"), outside);
           #endif

            // What was written reads back as the same program.
            for (const auto& program : { home.getChildFile (".local/opt/headroom/Headroom.AppImage"), home.getChildFile ("My Apps/100% Headroom.AppImage"),
                                         juce::File ("/opt/hr/Headroom.AppImage") })
                expect (parseExecStart (renderUnit (payload.unit, program, home), home) == program, program.getFullPathName());

            const auto entry = renderDesktopEntry (payload.desktopEntry, home.getChildFile ("My Apps/Headroom.AppImage"));
           #if ! JUCE_WINDOWS     // a Windows path has backslashes, which the Exec quoting escapes (Linux paths have none)
            expect (entry.contains ("\nExec=\"" + home.getChildFile ("My Apps/Headroom.AppImage").getFullPathName() + "\" --show\n"), entry);
           #endif
            expect (entry.contains ("Name=Headroom") && entry.contains ("Icon=headroom"));
        }
    }

    void freshInstall()
    {
        beginTest ("a first install copies the AppImage and writes the menu entry, icon, drop-in and unit");
        Rig rig;
        const auto source = rig.scratch.makeAppImage ("Downloads/Headroom-0.3.0-x86_64.AppImage", "appimage one");

        // Somebody else's WirePlumber file for the Wave stays as it is (and is mentioned).
        const auto other = rig.loc.wireplumberDir().getChildFile ("99-elgato-wave3.conf");
        other.getParentDirectory().createDirectory();
        other.replaceWithText ("monitor.alsa.rules = [ { matches = [ { device.name = \"~alsa_card.usb-Elgato_Systems_Elgato_Wave_3*\" } ] } ]\n");
        const auto otherBefore = text (other);

        InstallOptions options;
        options.source = source;
        options.startNow = false;

        const auto result = rig.installer.install (options);
        expect (result.ok, result.error);
        expect (result.program == rig.loc.programCopy());
        expect (rig.loc.programCopy().existsAsFile() && text (rig.loc.programCopy()) == "appimage one");
        expect (! rig.loc.optDir().getChildFile ("Headroom.AppImage.hr-new").exists(), "no temporary file is left");

        const auto desktop = text (rig.loc.desktopFile());
       #if ! JUCE_WINDOWS
        expect (desktop.contains ("Exec=\"" + rig.loc.programCopy().getFullPathName() + "\" --show"), desktop);
       #else
        expect (desktop.contains (" --show") && desktop.contains ("Headroom.AppImage"), desktop);
       #endif
        expect (text (rig.loc.iconFile()).contains ("<svg"));
        expect (text (rig.loc.wireplumberDropIn()) == Payload::fromBinaryData().wireplumberConf);
        expect (text (rig.loc.unitFile()).contains ("ExecStart=%h/.local/opt/headroom/Headroom.AppImage --background"));

        expect (rig.runner.count ("systemctl --user daemon-reload") == 1);
        expect (rig.runner.count ("systemctl --user enable headroom.service") == 1);
        expect (rig.runner.count ("restart wireplumber.service") == 1, "WirePlumber is restarted for the new drop-in");
        expect (result.wireplumberRestarted);
        expect (rig.runner.count ("restart headroom.service") == 0, "the wizard starts the service itself, after it has quit");
        expect (rig.quitCalls == 0);

        expect (text (other) == otherBefore, "the other drop-in is untouched");
        expect (rig.lines.joinIntoString ("\n").contains ("99-elgato-wave3.conf"), "... and mentioned");
        expect (rig.lines.joinIntoString ("\n").contains ("Installing Headroom"), "everything is logged");

        beginTest ("start now (unattended): the running copy is asked to quit, the service restarts and is checked");
        {
            Rig second;
            const auto src = second.scratch.makeAppImage ("Headroom.AppImage", "x");
            InstallOptions o;
            o.source = src;
            o.startAtLogin = false;
            const auto r = second.installer.install (o);
            expect (r.ok && r.serviceStarted, r.error);
            expect (second.quitCalls == 1);
            expect (second.runner.count ("systemctl --user disable headroom.service") == 1 && second.runner.count ("enable headroom.service") == 0,
                    "start at login off means disable");
            expect (second.runner.count ("restart headroom.service") == 1 && second.runner.count ("is-active --quiet headroom.service") >= 1);
        }
    }

    void again()
    {
        beginTest ("running the install again changes nothing and does not restart WirePlumber");
        Rig rig;
        const auto source = rig.scratch.makeAppImage ("Downloads/Headroom.AppImage", "appimage one");
        InstallOptions options;
        options.source = source;
        options.startNow = false;

        expect (rig.installer.install (options).ok);
        const auto time = rig.loc.programCopy().getLastModificationTime();
        const auto unit = text (rig.loc.unitFile());

        rig.runner.calls.clear();
        const auto second = rig.installer.install (options);
        expect (second.ok, second.error);
        expect (rig.runner.count ("restart wireplumber.service") == 0);
        expect (! second.wireplumberRestarted);
        expect (rig.loc.programCopy().getLastModificationTime() == time, "the identical copy is not written again");
        expect (text (rig.loc.unitFile()) == unit);

        beginTest ("installing from the installed copy itself does not copy it onto itself");
        {
            InstallOptions fromCopy;
            fromCopy.source = rig.loc.programCopy();
            fromCopy.startNow = false;
            expect (rig.installer.install (fromCopy).ok);
            expect (text (rig.loc.programCopy()) == "appimage one");
        }

        beginTest ("a changed file is replaced whole, and what is installed says what it is");
        {
            rig.scratch.makeAppImage ("Downloads/Headroom.AppImage", "appimage two");
            expect (rig.installer.install (options).ok);
            expect (text (rig.loc.programCopy()) == "appimage two");

            const auto info = rig.installer.inspect();
            expect (info.installed() && info.unitPresent && info.programPresent);
            expect (info.program == rig.loc.programCopy());
            expect (info.isAppImage && ! info.managedElsewhere);
        }
    }

    void keepInPlace()
    {
        beginTest ("keeping the AppImage where Gear Lever put it points the service there and adds no second menu entry");
        Rig rig;
        const auto source = rig.scratch.makeAppImage ("AppImages/headroom.appimage", "gear lever copy");

        // The entry Gear Lever made for it.
        const auto gear = rig.loc.applicationsDir().getChildFile ("headroom-1a2b3c.desktop");
        gear.getParentDirectory().createDirectory();
        gear.replaceWithText ("[Desktop Entry]\nName=Headroom\nExec=" + source.getFullPathName() + " %U\nType=Application\n");

        expect (rig.installer.isInManagedFolder (source));
        expect (! rig.installer.isInManagedFolder (rig.scratch.home.getChildFile ("Downloads/headroom.appimage")));

        InstallOptions options;
        options.source = source;
        options.keepInPlace = true;
        options.startNow = false;
        const auto result = rig.installer.install (options);

        expect (result.ok, result.error);
        expect (result.program == source);
        expect (! rig.loc.programCopy().exists(), "nothing is copied");
        expect (text (rig.loc.unitFile()).contains ("ExecStart=%h/AppImages/headroom.appimage --background"));
        expect (! rig.loc.desktopFile().exists(), "Gear Lever's entry is the one in the menu");

        const auto info = rig.installer.inspect();
        expect (info.installed() && info.managedElsewhere && info.program == source);

        beginTest ("uninstalling leaves the AppImage that Gear Lever looks after, and its entry");
        {
            const auto removed = rig.installer.uninstall ({});
            expect (removed.ok);
            expect (source.existsAsFile() && gear.existsAsFile());
            expect (! rig.loc.unitFile().exists());
        }
    }

    void tarballMigration()
    {
        beginTest ("an install from the tarball becomes an AppImage install and its old program files go");
        Rig rig;

        // What install.sh left behind.
        const auto opt = rig.loc.optDir();
        opt.getChildFile ("models").createDirectory();
        opt.getChildFile ("models/DeepFilterNet3_onnx.tar.gz").replaceWithText ("m");
        opt.getChildFile ("share").createDirectory();
        opt.getChildFile ("share/headroom.svg").replaceWithText ("s");
        for (const char* entryName : { "headroom", "install.sh", "uninstall.sh", "README.txt" })
            opt.getChildFile (entryName).replaceWithText ("old");
        opt.getChildFile ("my-notes.txt").replaceWithText ("not ours");

        const auto source = rig.scratch.makeAppImage ("Downloads/Headroom.AppImage", "appimage");
        InstallOptions options;
        options.source = source;
        options.startNow = false;
        expect (rig.installer.install (options).ok);

        expect (! opt.getChildFile ("headroom").exists() && ! opt.getChildFile ("models").exists() && ! opt.getChildFile ("share").exists());
        expect (! opt.getChildFile ("install.sh").exists() && ! opt.getChildFile ("uninstall.sh").exists());
        expect (opt.getChildFile ("Headroom.AppImage").existsAsFile());
        expect (opt.getChildFile ("my-notes.txt").existsAsFile(), "files that are not Headroom's stay");

        beginTest ("uninstalling removes the program folder only when nothing else is in it");
        {
            const auto removed = rig.installer.uninstall ({});
            expect (removed.ok);
            expect (opt.isDirectory() && opt.getChildFile ("my-notes.txt").existsAsFile() && ! opt.getChildFile ("Headroom.AppImage").exists());
            opt.getChildFile ("my-notes.txt").deleteFile();
            expect (rig.installer.uninstall ({}).ok);
            expect (! opt.exists(), "an empty program folder is removed");
        }
    }

    void update()
    {
        beginTest ("an update replaces the installed file, quits the running Headroom and restarts the service");
        Rig rig;
        const auto oldFile = rig.scratch.makeAppImage ("Downloads/old.AppImage", "version 0.2.4");
        InstallOptions first;
        first.source = oldFile;
        first.startNow = false;
        expect (rig.installer.install (first).ok);

        rig.runner.versions[rig.loc.programCopy().getFullPathName()] = "0.2.4";
        const auto info = rig.installer.inspect();
        expect (info.installed());
        expect (rig.installer.installedVersion (info.program) == "0.2.4");
        expect (compareVersions ("0.3.0", rig.installer.installedVersion (info.program)) > 0, "the new one is newer: offer the update");

        const auto newFile = rig.scratch.makeAppImage ("Downloads/new.AppImage", "version 0.3.0");
        rig.runner.calls.clear();

        InstallOptions update;
        update.source = newFile;
        update.startAtLogin = rig.installer.isEnabledAtLogin();
        update.startNow = true;
        const auto result = rig.installer.install (update);

        expect (result.ok && result.serviceStarted, result.error);
        expect (text (rig.loc.programCopy()) == "version 0.3.0");
        expect (rig.quitCalls == 1);
        expect (rig.runner.count ("restart headroom.service") == 1);
        expect (rig.runner.count ("restart wireplumber.service") == 0, "the drop-in did not change");
        expect (rig.runner.count ("enable headroom.service") == 1, "it was enabled and stays enabled");

        beginTest ("a program that does not answer --version has no version, so it counts as older");
        expect (rig.installer.installedVersion (rig.scratch.home.getChildFile ("nothing")).isEmpty());
        expect (compareVersions ("0.3.0", {}) > 0);
    }

    void uninstallKeepsOthers()
    {
        beginTest ("uninstall removes what Headroom put in place, twice over, and leaves everything else");
        Rig rig;
        const auto source = rig.scratch.makeAppImage ("Downloads/Headroom.AppImage", "appimage");
        InstallOptions options;
        options.source = source;
        options.startNow = false;
        expect (rig.installer.install (options).ok);

        const auto other = rig.loc.wireplumberDir().getChildFile ("99-elgato-wave3.conf");
        other.replaceWithText ("# somebody else's\n");
        const auto otherUnit = rig.loc.unitDir().getChildFile ("something-else.service");
        otherUnit.replaceWithText ("[Unit]\nDescription=Something else\n");
        const auto otherEntry = rig.loc.applicationsDir().getChildFile ("something-else.desktop");
        otherEntry.replaceWithText ("[Desktop Entry]\nName=Something else\n");
        const auto settings = rig.scratch.home.getChildFile (".config/headroom");
        settings.createDirectory();
        settings.getChildFile ("state.xml").replaceWithText ("<HEADROOM/>");

        rig.runner.calls.clear();
        const auto first = rig.installer.uninstall ({});
        expect (first.ok, first.error);
        expect (rig.quitCalls == 1, "a running Headroom is asked to quit first");
        expect (! rig.loc.unitFile().exists() && ! rig.loc.desktopFile().exists() && ! rig.loc.iconFile().exists());
        expect (! rig.loc.wireplumberDropIn().exists() && ! rig.loc.programCopy().exists() && ! rig.loc.optDir().exists());
        expect (other.existsAsFile() && otherUnit.existsAsFile() && otherEntry.existsAsFile(), "other people's files stay");
        expect (settings.getChildFile ("state.xml").existsAsFile(), "settings are kept");
        expect (source.existsAsFile(), "the downloaded AppImage is not ours to delete");
        expect (rig.runner.count ("systemctl --user disable headroom.service") == 1);
        expect (rig.runner.count ("systemctl --user stop --no-block headroom.service") == 1);
        expect (rig.runner.count ("restart wireplumber.service") == 1, "the Wave:3 behaviour goes back to the stock one");

        rig.runner.calls.clear();
        const auto second = rig.installer.uninstall ({});
        expect (second.ok, "uninstalling again is fine");
        expect (rig.runner.count ("disable headroom.service") == 0 && rig.runner.count ("wireplumber") == 0, "... and does nothing");

        beginTest ("another program's file called headroom.service or headroom.desktop is not ours to delete");
        {
            Rig third;
            third.loc.unitDir().createDirectory();
            third.loc.unitFile().replaceWithText ("[Unit]\nDescription=Totally different\n");
            third.loc.applicationsDir().createDirectory();
            third.loc.desktopFile().replaceWithText ("[Desktop Entry]\nName=Different\nExec=/usr/bin/different\n");
            expect (third.installer.uninstall ({}).ok);
            expect (third.loc.unitFile().existsAsFile() && third.loc.desktopFile().existsAsFile());
        }
    }

    void uninstallSettingsAndDefaults()
    {
        beginTest ("delete my settings removes the folders that were named, and only those");
        {
            Rig rig;
            const auto config = rig.scratch.home.getChildFile (".config/headroom");
            const auto state = rig.scratch.home.getChildFile (".local/state/headroom");
            const auto unrelated = rig.scratch.home.getChildFile (".config/headroom-other");
            for (const auto& folder : { config, state, unrelated })
            {
                folder.createDirectory();
                folder.getChildFile ("f.txt").replaceWithText ("x");
            }

            UninstallOptions options;
            options.deleteSettings = true;
            options.settingsFolders = { config, state };
            expect (rig.installer.uninstall (options).ok);
            expect (! config.exists() && ! state.exists());
            expect (unrelated.isDirectory(), "a folder that was not named stays");
        }

        beginTest ("default devices are given back only when they point at Headroom");
        {
            Rig rig;
            rig.runner.metadata = "update: id:0 key:'default.configured.audio.sink' value:'{ \"name\": \"headroom.sink\" }' type:'Spa:String:JSON'\n"
                                  "update: id:0 key:'default.configured.audio.source' value:'{ \"name\": \"alsa_input.usb-Elgato\" }' type:'Spa:String:JSON'\n";
            expect (rig.installer.uninstall ({}).ok);
            expect (rig.runner.count ("pw-metadata -n default -d 0 default.configured.audio.sink") == 1, "ours: cleared");
            expect (rig.runner.count ("pw-metadata -n default -d 0 default.configured.audio.source") == 0, "the user's own choice stays");
        }

        beginTest ("without pw-metadata nothing is asked");
        {
            Rig rig;
            rig.runner.absent.add ("pw-metadata");
            expect (rig.installer.uninstall ({}).ok);
            expect (rig.runner.count ("pw-metadata") == 0);
        }
    }

    void udevRule()
    {
        // What may run as root, character for character (tests/integration/install-test.sh has the same text).
        const juce::String expected =
            "rm -f /etc/udev/rules.d/70-headroom-wave.rules /etc/udev/rules.d/.70-headroom-wave.rules.tmp || exit 10\n"
            "for u in /usr/bin/udevadm /bin/udevadm /usr/sbin/udevadm /sbin/udevadm; do [ -x \"$u\" ] && break; done\n"
            "\"$u\" control --reload || echo \"udevadm control --reload did not succeed\" >&2\n"
            "\"$u\" trigger --action=add --attr-match=idVendor=0fd9 --attr-match=idProduct=0070 || echo \"udevadm trigger did not succeed\" >&2\n"
            "exit 0";

        beginTest ("the udev rule from Allow hardware controls is removed with one pkexec of /bin/sh and a constant script");
        {
            expect (juce::String (udevRemovalScript()) == expected);

            Rig rig;
            const auto rule = rig.scratch.root.getChildFile ("70-headroom-wave.rules");
            rule.replaceWithText ("SUBSYSTEM==\"usb\"\n");
            rig.runner.onPkexecSuccess = [rule] { rule.deleteFile(); };

            const auto result = rig.installer.uninstall ({});
            expect (result.ok && result.manualUdevCommand.isEmpty());
            expect (! rule.exists());
            expect (rig.runner.count ("pkexec") == 1);

            for (const auto& call : rig.runner.calls)
                if (call[0].endsWith ("pkexec"))
                    expect (call.size() == 4 && call[1] == "/bin/sh" && call[2] == "-c" && call[3] == expected, "exactly: pkexec /bin/sh -c <the constant script>");
        }

        beginTest ("a cancelled password box leaves the rule and says how to remove it by hand");
        {
            Rig rig;
            const auto rule = rig.scratch.root.getChildFile ("70-headroom-wave.rules");
            rule.replaceWithText ("x");
            rig.runner.pkexecExit = 126;
            const auto result = rig.installer.uninstall ({});
            expect (result.ok, "removing Headroom itself still works");
            expect (rule.exists());
            expect (result.manualUdevCommand.contains ("sudo rm /etc/udev/rules.d/70-headroom-wave.rules"));
        }

        beginTest ("with no rule, or with passwords switched off, nothing is asked");
        {
            Rig rig;
            expect (rig.installer.uninstall ({}).ok);
            expect (rig.runner.count ("pkexec") == 0);

            Rig quiet;
            quiet.scratch.root.getChildFile ("70-headroom-wave.rules").replaceWithText ("x");
            UninstallOptions options;
            options.askForPassword = false;
            const auto result = quiet.installer.uninstall (options);
            expect (quiet.runner.count ("pkexec") == 0 && result.manualUdevCommand.isNotEmpty());
        }
    }

    void failures()
    {
        beginTest ("without a reachable desktop session nothing is written and the error is plain");
        {
            Rig rig;
            rig.runner.sessionWorks = false;
            InstallOptions options;
            options.source = rig.scratch.makeAppImage ("Headroom.AppImage", "x");
            const auto result = rig.installer.install (options);
            expect (! result.ok && result.error.contains ("desktop session"), result.error);
            expect (! rig.loc.optDir().exists() && ! rig.loc.unitFile().exists());
        }

        beginTest ("without systemd, without PipeWire, and without a file to install");
        {
            Rig noSystemd;
            noSystemd.runner.absent.add ("systemctl");
            InstallOptions options;
            options.source = noSystemd.scratch.makeAppImage ("Headroom.AppImage", "x");
            expect (noSystemd.installer.install (options).error.contains ("systemd"));

            Rig noPipewire;
            noPipewire.runner.pipewireThere = false;
            options.source = noPipewire.scratch.makeAppImage ("Headroom.AppImage", "x");
            expect (noPipewire.installer.install (options).error.contains ("PipeWire"));

            Rig noFile;
            options.source = noFile.scratch.home.getChildFile ("missing.AppImage");
            expect (! noFile.installer.install (options).ok);
        }

        beginTest ("a service that does not come up says so, with its log lines");
        {
            Rig rig;
            rig.runner.serviceActive = false;
            InstallOptions options;
            options.source = rig.scratch.makeAppImage ("Headroom.AppImage", "x");
            const auto result = rig.installer.install (options);
            expect (! result.ok && result.error.contains ("did not start"), result.error);
            expect (rig.runner.count ("journalctl --user -u headroom.service") == 1);
        }

        beginTest ("the start-at-login switch only works for an installed Headroom");
        {
            Rig rig;
            expect (! rig.installer.setStartAtLogin (true));
            InstallOptions options;
            options.source = rig.scratch.makeAppImage ("Headroom.AppImage", "x");
            options.startNow = false;
            expect (rig.installer.install (options).ok);
            rig.runner.calls.clear();
            expect (rig.installer.setStartAtLogin (false));
            expect (rig.runner.count ("systemctl --user disable headroom.service") == 1);
        }
    }
};

static InstallTests installTests;
}
