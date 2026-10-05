// Install: copies Headroom into place, writes the menu entry, icon, WirePlumber drop-in and systemd unit, and takes it all out again.
#include "app/Install.h"

#include <BinaryData.h>

#include <algorithm>

namespace hr::install
{
namespace
{
    // The program that cleans a Wave's udev rule up again. A constant, identical to uninstall.sh's: no path, name or value of ours,
    // the user's or the environment's is ever put into what runs as root.
    constexpr const char* kUdevRemovalScript =
        "rm -f /etc/udev/rules.d/70-headroom-wave.rules /etc/udev/rules.d/.70-headroom-wave.rules.tmp || exit 10\n"
        "for u in /usr/bin/udevadm /bin/udevadm /usr/sbin/udevadm /sbin/udevadm; do [ -x \"$u\" ] && break; done\n"
        "\"$u\" control --reload || echo \"udevadm control --reload did not succeed\" >&2\n"
        "\"$u\" trigger --action=add --attr-match=idVendor=0fd9 --attr-match=idProduct=0070 || echo \"udevadm trigger did not succeed\" >&2\n"
        "exit 0";

    constexpr const char* kUdevManualCommand = "sudo rm /etc/udev/rules.d/70-headroom-wave.rules && sudo udevadm control --reload";

    juce::String resourceText (const char* originalName)
    {
        for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
        {
            const char* name = BinaryData::namedResourceList[i];

            if (juce::String (BinaryData::getNamedResourceOriginalFilename (name)) != originalName)
                continue;

            int size = 0;

            if (const char* data = BinaryData::getNamedResource (name, size))
                return juce::String::fromUTF8 (data, size).replace ("\r\n", "\n");
        }

        return {};
    }

    juce::String charText (juce::juce_wchar c)      { return juce::String::charToString (c); }

    // ---- Quoting ------------------------------------------------------------------------------------------------
    // Exec= in a .desktop file (Desktop Entry spec): an argument in double quotes; inside them " ` $ and \ get a backslash, which
    // the string level of the file escapes once more; a literal % is %%.
    juce::String quoteForDesktop (const juce::String& text)
    {
        juce::String out ("\"");

        for (auto c : text)
        {
            if (c == '%')                                   out += "%%";
            else if (c == '"' || c == '`' || c == '$')      out += "\\\\" + charText (c);
            else if (c == '\\')                             out += "\\\\\\\\";
            else                                            out += charText (c);
        }

        return out + "\"";
    }

    // ExecStart= in a unit: a plain word when nothing needs quoting, else in double quotes with \ and " escaped. % starts a
    // specifier (%h = the home folder) and is written %%, $ is written $$. `home` is the %h the program path may start with.
    juce::String quoteForSystemd (const juce::File& program, const juce::File& home)
    {
        juce::String prefix, rest;

        if (program.isAChildOf (home))
        {
            prefix = "%h/";
            rest = program.getRelativePathFrom (home).replace ("\\", "/");
        }
        else
        {
            rest = program.getFullPathName();
        }

        juce::String escaped;
        bool plain = true;

        for (auto c : rest)
        {
            const bool safe = juce::CharacterFunctions::isLetterOrDigit (c) && c < 128;

            if (c == '%')                       { escaped += "%%"; plain = false; }
            else if (c == '$')                  { escaped += "$$"; plain = false; }
            else if (c == '"' || c == '\\')     { escaped += "\\" + charText (c); plain = false; }
            else
            {
                escaped += charText (c);

                if (! safe && juce::String ("_./-+=,:@").indexOfChar (c) < 0)
                    plain = false;
            }
        }

        const auto word = prefix + escaped;
        return plain ? word : "\"" + word + "\"";
    }

    juce::String replaceKeyLine (const juce::String& text, const juce::String& key, const juce::String& newValue)
    {
        juce::StringArray lines = juce::StringArray::fromLines (text);

        for (auto& line : lines)
            if (line.startsWith (key + "="))
                line = key + "=" + newValue;

        return lines.joinIntoString ("\n") + "\n";
    }

    juce::File resolveLinks (juce::File file)
    {
        for (int i = 0; i < 8 && file.isSymbolicLink(); ++i)
            file = file.getLinkedTarget();

        return file;
    }

    bool looksLikeHeadroomFile (const juce::File& file)
    {
        return file.existsAsFile() && file.loadFileAsString().containsIgnoreCase ("headroom");
    }

    // "Headroom 0.2.4" -> "0.2.4"
    juce::String versionFrom (const juce::String& output)
    {
        for (const auto& line : juce::StringArray::fromLines (output))
        {
            const auto trimmed = line.trim();

            if (trimmed.startsWithIgnoreCase ("headroom "))
            {
                const auto number = trimmed.substring (9).trim().upToFirstOccurrenceOf (" ", false, false);

                if (number.isNotEmpty() && juce::CharacterFunctions::isDigit (number[0]))
                    return number;
            }
        }

        return {};
    }

    // The part of a version that counts: digits and dots before any '-' or '+'.
    std::vector<int> versionParts (const juce::String& version)
    {
        std::vector<int> parts;
        const auto core = version.trim().upToFirstOccurrenceOf ("-", false, false).upToFirstOccurrenceOf ("+", false, false);

        for (const auto& piece : juce::StringArray::fromTokens (core, ".", ""))
            parts.push_back (piece.getIntValue());

        return parts;
    }
}

// ---------------------------------------------------------------------------------------------------
// Small public pieces
// ---------------------------------------------------------------------------------------------------

Locations Locations::forCurrentUser()
{
    return Locations (juce::File::getSpecialLocation (juce::File::userHomeDirectory));
}

std::vector<juce::File> Locations::managedFolders() const
{
    return { home.getChildFile ("AppImages"), home.getChildFile ("Applications"), home.getChildFile (".local/share/AppImages") };
}

Payload Payload::fromBinaryData()
{
    Payload p;
    p.desktopEntry = resourceText ("headroom.desktop");
    p.unit = resourceText ("headroom.service");
    p.wireplumberConf = resourceText ("51-headroom-wave.conf");
    p.iconSvg = resourceText ("headroom.svg");
    return p;
}

bool Payload::isComplete() const
{
    return desktopEntry.contains ("Exec=") && unit.contains ("ExecStart=") && wireplumberConf.isNotEmpty() && iconSvg.contains ("<svg");
}

namespace
{
    class SystemRunner final : public CommandRunner
    {
    public:
        CommandResult run (const juce::StringArray& command, int timeoutMs) override
        {
            CommandResult result;

            if (command.isEmpty())
                return result;

            juce::ChildProcess process;

            if (! process.start (command))
                return result;

            result.started = true;

            if (! process.waitForProcessToFinish (timeoutMs))
            {
                process.kill();
                return result;                          // exitCode stays -1
            }

            result.output = process.readAllProcessOutput();
            result.exitCode = (int) process.getExitCode();
            return result;
        }

        bool exists (const juce::String& program) override
        {
            if (program.containsChar ('/'))
                return juce::File (program).existsAsFile();

            for (const auto& folder : juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("PATH", {}), ":", ""))
                if (folder.isNotEmpty() && juce::File (folder).getChildFile (program).existsAsFile())
                    return true;

            return false;
        }
    };
}

std::unique_ptr<CommandRunner> makeSystemRunner()
{
    return std::make_unique<SystemRunner>();
}

CommandResult RecordingRunner::run (const juce::StringArray& command, int)
{
    {
        const std::lock_guard<std::mutex> guard (lock);
        calls.push_back (command);
    }

    CommandResult result;
    result.started = true;
    result.exitCode = 0;
    const auto joined = command.joinIntoString (" ");

    if (joined.contains ("list-unit-files pipewire.service"))
        result.output = "pipewire.service enabled enabled\n";
    else if (command.size() > 1 && command[command.size() - 1] == "--version")
        result.output = versionLine() + "\n";

    return result;
}

std::vector<juce::StringArray> RecordingRunner::getCalls() const
{
    const std::lock_guard<std::mutex> guard (lock);
    return calls;
}

int RecordingRunner::count (const juce::String& fragment) const
{
    int n = 0;

    for (const auto& call : getCalls())
        if (call.joinIntoString (" ").contains (fragment))
            ++n;

    return n;
}

juce::String currentVersion()
{
    const auto fromTest = juce::SystemStats::getEnvironmentVariable ("HEADROOM_TEST_VERSION", {}).trim();
    return fromTest.isNotEmpty() ? fromTest : juce::String (HEADROOM_VERSION);
}

juce::String versionLine()
{
    return "Headroom " + currentVersion();
}

int compareVersions (const juce::String& a, const juce::String& b)
{
    const auto pa = versionParts (a), pb = versionParts (b);

    for (size_t i = 0; i < std::max (pa.size(), pb.size()); ++i)
    {
        const int x = i < pa.size() ? pa[i] : 0, y = i < pb.size() ? pb[i] : 0;

        if (x != y)
            return x < y ? -1 : 1;
    }

    return 0;
}

juce::String renderDesktopEntry (const juce::String& templateText, const juce::File& program)
{
    return replaceKeyLine (templateText, "Exec", quoteForDesktop (program.getFullPathName()) + " --show");
}

juce::String renderUnit (const juce::String& templateText, const juce::File& program, const juce::File& home)
{
    return replaceKeyLine (templateText, "ExecStart", quoteForSystemd (program, home) + " --background");
}

juce::File parseExecStart (const juce::String& unitText, const juce::File& home)
{
    for (auto line : juce::StringArray::fromLines (unitText))
    {
        line = line.trim();

        if (! line.startsWith ("ExecStart="))
            continue;

        auto value = line.fromFirstOccurrenceOf ("=", false, false).trim().trimCharactersAtStart ("-@:+!");
        juce::String word;

        if (value.startsWithChar ('"'))
        {
            for (int i = 1; i < value.length(); ++i)
            {
                const auto c = value[i];

                if (c == '"')
                    break;

                if (c == '\\' && i + 1 < value.length())
                    word += charText (value[++i]);
                else
                    word += charText (c);
            }
        }
        else
        {
            word = value.upToFirstOccurrenceOf (" ", false, false).upToFirstOccurrenceOf ("\t", false, false);
        }

        // Specifiers and escapes a unit may use for a path: %h (home), %% and $$.
        juce::String path;

        for (int i = 0; i < word.length(); ++i)
        {
            if (word[i] == '%' && i + 1 < word.length())
            {
                const auto next = word[i + 1];
                path += next == 'h' ? home.getFullPathName() : charText (next);
                ++i;
            }
            else if (word[i] == '$' && i + 1 < word.length() && word[i + 1] == '$')
            {
                path += "$";
                ++i;
            }
            else
            {
                path += charText (word[i]);
            }
        }

        return path.isEmpty() ? juce::File() : juce::File (path);
    }

    return {};
}

const char* udevRemovalScript()
{
    return kUdevRemovalScript;
}

// ---------------------------------------------------------------------------------------------------
// Installer
// ---------------------------------------------------------------------------------------------------

struct Installer::Run
{
    Result result;
};

Installer::Installer (Locations locations, CommandRunner& commandRunner, Payload payloadToWrite)
    : loc (std::move (locations)), runner (commandRunner), payload (std::move (payloadToWrite))
{
}

void Installer::note (Run& run, const juce::String& line)
{
    run.result.log.add (line);

    if (logLine)
        logLine (line);
}

CommandResult Installer::runSystemctl (std::initializer_list<const char*> args, int timeoutMs)
{
    juce::StringArray command { "systemctl", "--user" };

    for (const char* arg : args)
        command.add (arg);

    return runner.run (command, timeoutMs);
}

bool Installer::isInManagedFolder (const juce::File& file) const
{
    for (const auto& folder : loc.managedFolders())
        if (file.isAChildOf (folder))
            return true;

    return false;
}

Info Installer::inspect() const
{
    Info info;
    info.unitPresent = loc.unitFile().existsAsFile();

    if (info.unitPresent)
    {
        info.program = parseExecStart (loc.unitFile().loadFileAsString(), loc.home);
        info.programPresent = info.program != juce::File() && info.program.existsAsFile();
        info.isAppImage = info.program.getFileName().endsWithIgnoreCase (".appimage");
        info.managedElsewhere = info.isAppImage && ! info.program.isAChildOf (loc.optDir());
    }

    return info;
}

juce::String Installer::installedVersion (const juce::File& program)
{
    juce::StringArray command;

    // A test plays a newer build with $HEADROOM_TEST_VERSION, which the installed one must not hear (it is asked what it is).
    if (juce::SystemStats::getEnvironmentVariable ("HEADROOM_TEST_VERSION", {}).isNotEmpty())
        command.addArray ({ "env", "-u", "HEADROOM_TEST_VERSION" });

    command.add (program.getFullPathName());
    command.add ("--version");

    const auto result = runner.run (command, 30000);
    return result.ok() ? versionFrom (result.output) : juce::String();
}

namespace
{
    // Writes the file whole (a temporary file renamed over it) unless it already says exactly this. True if it changed.
    bool writeIfChanged (const juce::File& file, const juce::String& text)
    {
        if (file.existsAsFile() && file.loadFileAsString().replace ("\r\n", "\n") == text)
            return false;

        file.getParentDirectory().createDirectory();
        return file.replaceWithText (text, false, false, "\n");
    }
}

bool Installer::preflight (Run& run)
{
    auto fail = [&run] (const juce::String& message)
    {
        run.result.ok = false;
        run.result.error = message;
        return false;
    };

    if (! runner.exists ("systemctl"))
        return fail ("This computer does not use systemd, which Headroom needs to start itself.");

    if (! runSystemctl ({ "show-environment" }).ok())
        return fail ("Your desktop session is not reachable (systemctl --user does not work). Start Headroom from a normal desktop login, not over plain SSH or su.");

    if (! runSystemctl ({ "list-unit-files", "pipewire.service" }).output.contains ("pipewire.service"))
        return fail ("PipeWire does not seem to be installed. It is standard on Bazzite; Headroom cannot work without it.");

    return true;
}

void Installer::migrateFromTarball (Run& run)
{
    const auto optDir = loc.optDir();
    const auto legacy = optDir.getChildFile ("headroom");

    if (! legacy.existsAsFile())
        return;

    // The files install.sh from the tarball put in the program folder. They are stale now: the AppImage brings its own.
    note (run, "Removing the program files of the earlier tarball install");

    legacy.deleteFile();
    optDir.getChildFile ("models").deleteRecursively();
    optDir.getChildFile ("share").deleteRecursively();

    for (const char* name : { "install.sh", "uninstall.sh", "README.txt" })
        optDir.getChildFile (name).deleteFile();

    // The symlink in ~/.local/bin is ours only if it points at that program.
    const auto link = loc.binLink();

    if (link.isSymbolicLink() && link.getLinkedTarget() == legacy)
        link.deleteFile();
}

Result Installer::install (const InstallOptions& options)
{
    Run run;

    auto fail = [&] (const juce::String& message)
    {
        run.result.ok = false;
        run.result.error = message;
        note (run, "Setup stopped: " + message);
        return run.result;
    };

    note (run, "Installing Headroom " + currentVersion());

    if (! options.source.existsAsFile())
        return fail ("Headroom could not find its own file to install. Start it from the downloaded AppImage and try again.");

    if (! preflight (run))
        return fail (run.result.error);

    note (run, "Checked the system: systemd and PipeWire are there");

    // ---- The program ----------------------------------------------------------------------------------------
    juce::File program = options.source;

    if (options.keepInPlace)
    {
        options.source.setExecutePermission (true);
        note (run, "Headroom stays where it is: " + program.getFullPathName());
    }
    else
    {
        program = loc.programCopy();
        const bool sameFile = program.existsAsFile() && resolveLinks (program) == resolveLinks (options.source);

        if (sameFile)
        {
            note (run, "Headroom already runs from " + program.getFullPathName());
        }
        else if (program.existsAsFile() && program.hasIdenticalContentTo (options.source))
        {
            note (run, "The installed copy is already up to date: " + program.getFullPathName());
        }
        else
        {
            loc.optDir().createDirectory();

            // Next to the target and renamed over it: the old file is replaced whole, never half-written (a running copy
            // keeps the old file open, so replacing it is safe).
            juce::TemporaryFile temporary (program, loc.optDir().getChildFile (program.getFileName() + ".hr-new"));

            if (! options.source.copyFileTo (temporary.getFile()))
                return fail ("Headroom could not copy itself to " + loc.optDir().getFullPathName() + ". Check that there is room and that you may write there.");

            temporary.getFile().setExecutePermission (true);

            if (! temporary.overwriteTargetFileWithTemporary())
                return fail ("Headroom could not put itself in place in " + loc.optDir().getFullPathName() + ".");

            note (run, "Copied Headroom to " + program.getFullPathName());
        }
    }

    migrateFromTarball (run);

    // ---- Menu entry and icon --------------------------------------------------------------------------------
    bool launcherChanged = false;

    {
        // Gear Lever and friends make their own menu entry for an AppImage they look after. Do not add a second one.
        juce::File foreign;

        for (const auto& entry : loc.applicationsDir().findChildFiles (juce::File::findFiles, false, "*.desktop"))
            if (entry != loc.desktopFile() && entry.loadFileAsString().contains (program.getFullPathName()))
                foreign = entry;

        if (foreign != juce::File())
        {
            note (run, "Your launcher already has a menu entry for it (" + foreign.getFileName() + "); not adding another");
        }
        else if (writeIfChanged (loc.desktopFile(), renderDesktopEntry (payload.desktopEntry, program)))
        {
            launcherChanged = true;
            note (run, "Menu entry: " + loc.desktopFile().getFullPathName());
        }
        else
        {
            note (run, "Menu entry is up to date");
        }
    }

    if (writeIfChanged (loc.iconFile(), payload.iconSvg))
    {
        launcherChanged = true;
        note (run, "Icon: " + loc.iconFile().getFullPathName());
    }

    if (launcherChanged)
    {
        // Best effort: each of these only exists on some desktops.
        runner.run ({ "update-desktop-database", loc.applicationsDir().getFullPathName() }, 10000);
        runner.run ({ "gtk-update-icon-cache", "-q", "-t", loc.iconThemeDir().getFullPathName() }, 10000);

        if (runner.exists ("kbuildsycoca6"))
            runner.run ({ "kbuildsycoca6" }, 20000);
    }

    // ---- WirePlumber drop-in ---------------------------------------------------------------------------------
    const bool wirePlumberChanged = writeIfChanged (loc.wireplumberDropIn(), payload.wireplumberConf);
    note (run, wirePlumberChanged ? "Elgato Wave settings: " + loc.wireplumberDropIn().getFullPathName() : juce::String ("Elgato Wave settings are up to date"));

    // Somebody else's files for the Wave (an earlier fix attempt) are never touched, only mentioned.
    for (const auto& other : loc.wireplumberDir().findChildFiles (juce::File::findFiles, false, "*.conf"))
    {
        if (other == loc.wireplumberDropIn())
            continue;

        const auto text = other.loadFileAsString();

        if (text.containsIgnoreCase ("Elgato_Wave") || text.containsIgnoreCase ("0x0fd9"))
            note (run, "Note: " + other.getFullPathName() + " also configures an Elgato Wave. Headroom takes care of the mic by itself, so that file is "
                       "no longer needed and can be removed (Headroom leaves it alone).");
    }

    // ---- The service ---------------------------------------------------------------------------------------
    if (! writeIfChanged (loc.unitFile(), renderUnit (payload.unit, program, loc.home)))
        note (run, "Start-at-login service is up to date");
    else
        note (run, "Start-at-login service: " + loc.unitFile().getFullPathName());

    if (! runSystemctl ({ "daemon-reload" }).ok())
        return fail ("The system would not load Headroom's start-up file (systemctl --user daemon-reload failed).");

    if (! runSystemctl ({ options.startAtLogin ? "enable" : "disable", "headroom.service" }).ok())
        return fail (options.startAtLogin ? "Headroom could not be set to start when you log in."
                                           : "Headroom could not be set to not start when you log in.");

    note (run, options.startAtLogin ? "Headroom starts when you log in" : "Headroom does not start when you log in");

    if (wirePlumberChanged)
    {
        note (run, "Applying the Elgato Wave settings (your sound may crackle for a second)");
        run.result.wireplumberRestarted = runSystemctl ({ "restart", "wireplumber.service" }, 30000).ok();

        if (! run.result.wireplumberRestarted)
            note (run, "(Could not restart WirePlumber; the settings apply after your next login.)");

        if (wirePlumberPauseMs > 0)
            juce::Thread::sleep (wirePlumberPauseMs);
    }

    if (options.startNow)
    {
        // A copy running without the service would make the service below exit as "already running": ask it to go first.
        if (quitRunning)
            quitRunning();

        if (! runSystemctl ({ "restart", "headroom.service" }, 30000).ok())
            return fail ("Headroom was installed but could not be started.");

        for (int attempt = 0; attempt < 8 && ! run.result.serviceStarted; ++attempt)
        {
            if (serviceCheckPauseMs > 0)
                juce::Thread::sleep (serviceCheckPauseMs);

            run.result.serviceStarted = runSystemctl ({ "is-active", "--quiet", "headroom.service" }).ok();
        }

        if (! run.result.serviceStarted)
        {
            const auto journal = runner.run ({ "journalctl", "--user", "-u", "headroom.service", "-n", "15", "--no-pager" }, 10000);

            for (const auto& line : juce::StringArray::fromLines (journal.output))
                note (run, "   " + line);

            return fail ("Headroom was installed but did not start. The log has the details.");
        }

        note (run, "Headroom is running");
    }

    run.result.ok = true;
    run.result.program = program;
    note (run, "Done");
    return run.result;
}

bool Installer::isEnabledAtLogin()
{
    return runSystemctl ({ "is-enabled", "--quiet", "headroom.service" }).ok();
}

bool Installer::setStartAtLogin (bool enabled)
{
    if (! loc.unitFile().existsAsFile())
        return false;

    return runSystemctl ({ enabled ? "enable" : "disable", "headroom.service" }).ok();
}

// ---------------------------------------------------------------------------------------------------
// Uninstall
// ---------------------------------------------------------------------------------------------------

// Removes a default-device setting, but only if it points at one of Headroom's devices (the user's own choice stays).
void Installer::clearDefaultIfOurs (Run& run, const char* key)
{
    if (! runner.exists ("pw-metadata"))
        return;

    const auto listing = runner.run ({ "pw-metadata", "-n", "default" }, 5000);

    for (const auto& line : juce::StringArray::fromLines (listing.output))
    {
        if (line.contains (juce::String ("key:'") + key + "'") && line.contains ("headroom."))
        {
            runner.run ({ "pw-metadata", "-n", "default", "-d", "0", key }, 5000);
            note (run, juce::String ("Cleared ") + key);
            return;
        }
    }
}

void Installer::removeUdevRule (Run& run, const UninstallOptions& options)
{
    // "Allow hardware controls" put one system file in place after asking for a password. Take it out again, but only if it is
    // there: with no such file nothing is asked. What runs as root is /bin/sh with the constant script, once.
    if (! udevRule.existsAsFile())
        return;

    juce::String pkexec = pkexecPath;

    if (pkexec.isEmpty())
        for (const char* candidate : { "/usr/bin/pkexec", "/bin/pkexec" })
            if (pkexec.isEmpty() && juce::File (candidate).existsAsFile())
                pkexec = candidate;

    if (options.askForPassword && pkexec.isNotEmpty())
    {
        note (run, "Removing the permission for the Elgato hardware controls (this needs your password)");
        const auto result = runner.run ({ pkexec, "/bin/sh", "-c", kUdevRemovalScript }, 180000);

        if (result.ok())
        {
            note (run, "Removed the permission");
            return;
        }

        note (run, "The permission was not removed (the password box was cancelled, or it could not be shown)");
    }

    run.result.manualUdevCommand = kUdevManualCommand;
    note (run, juce::String ("It does no harm. To remove it yourself, run this in a terminal: ") + kUdevManualCommand);
}

Result Installer::uninstall (const UninstallOptions& options)
{
    Run run;
    note (run, "Removing Headroom");

    // A running Headroom saves and quits by itself first (state saved, PipeWire nodes removed).
    if (quitRunning)
        quitRunning();

    const bool hadUnit = loc.unitFile().existsAsFile();

    if (hadUnit && runner.exists ("systemctl"))
    {
        runSystemctl ({ "disable", "headroom.service" });

        if (runSystemctl ({ "is-active", "--quiet", "headroom.service" }).ok())
            runSystemctl ({ "stop", "--no-block", "headroom.service" });

        note (run, "Stopped the start-at-login service");
    }

    // Headroom is gone from the graph now, so nothing puts these back.
    clearDefaultIfOurs (run, "default.configured.audio.sink");
    clearDefaultIfOurs (run, "default.configured.audio.source");

    // ---- Files: only Headroom's own ----------------------------------------------------------------------------
    const bool hadDropIn = looksLikeHeadroomFile (loc.wireplumberDropIn());

    if (looksLikeHeadroomFile (loc.unitFile()))             loc.unitFile().deleteFile();
    if (looksLikeHeadroomFile (loc.desktopFile()))          loc.desktopFile().deleteFile();
    if (hadDropIn)                                          loc.wireplumberDropIn().deleteFile();
    if (loc.iconFile().existsAsFile())                      loc.iconFile().deleteFile();

    // The program folder: the copy and what the tarball put there, never the folder itself unless it is empty after that.
    const auto optDir = loc.optDir();
    optDir.getChildFile ("Headroom.AppImage").deleteFile();
    optDir.getChildFile ("Headroom.AppImage.hr-new").deleteFile();

    const auto legacy = optDir.getChildFile ("headroom");
    const auto link = loc.binLink();

    if (link.isSymbolicLink() && link.getLinkedTarget() == legacy)
        link.deleteFile();

    legacy.deleteFile();
    optDir.getChildFile ("models").deleteRecursively();
    optDir.getChildFile ("share").deleteRecursively();

    for (const char* name : { "install.sh", "uninstall.sh", "README.txt" })
        optDir.getChildFile (name).deleteFile();

    if (optDir.isDirectory() && optDir.getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0)
        optDir.deleteFile();

    note (run, "Program, menu entry, icon and start-at-login service removed");

    if (options.deleteSettings)
    {
        for (const auto& folder : options.settingsFolders)
            if (folder != juce::File() && folder.isDirectory())
                folder.deleteRecursively();

        note (run, "Your Headroom settings and logs were deleted");
    }
    else
    {
        note (run, "Your settings are kept, in case you install Headroom again");
    }

    if (runner.exists ("systemctl"))
        runSystemctl ({ "daemon-reload" });

    runner.run ({ "update-desktop-database", loc.applicationsDir().getFullPathName() }, 10000);

    if (runner.exists ("kbuildsycoca6"))
        runner.run ({ "kbuildsycoca6" }, 20000);

    if (hadDropIn && runner.exists ("systemctl"))
    {
        note (run, "Restoring the standard Elgato Wave behaviour");
        runSystemctl ({ "restart", "wireplumber.service" }, 30000);
    }

    removeUdevRule (run, options);

    run.result.ok = true;
    note (run, "Headroom has been removed");
    return run.result;
}
}
