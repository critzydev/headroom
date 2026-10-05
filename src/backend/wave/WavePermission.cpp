// The udev rule text, the constant script that installs it and the pkexec call that runs it (see WavePermission.h).
#include "backend/wave/WavePermission.h"

#include "backend/ElgatoModels.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#if defined (__linux__)
 #include <cerrno>
 #include <fcntl.h>
 #include <signal.h>
 #include <spawn.h>
 #include <sys/wait.h>
 #include <unistd.h>

extern char** environ;
#endif

namespace hr::wave::permission
{
namespace
{
    // The rule, line by line. Nothing here may contain an apostrophe: installScript() puts each line in '...'.
    const char* const kRuleComment1 = "# Written by Headroom when you press \"Allow hardware controls\": it lets you, at this computer, control the";
    const char* const kRuleComment2 = "# Elgato Wave:3 (ClipGuard). The Headroom uninstall.sh removes it.";

    std::string hex4 (uint16_t value)
    {
        char text[8];
        std::snprintf (text, sizeof (text), "%04x", (unsigned) value);
        return text;
    }

    // The products the rule is for: the tested ones, and the experimental ones only when the user asked for them.
    std::vector<uint16_t> products (bool withExperimental)
    {
        auto pids = elgato::pidsWith (elgato::Hardware::Tested);

        if (withExperimental)
            for (const auto pid : elgato::pidsWith (elgato::Hardware::Experimental))
                pids.push_back (pid);

        return pids;
    }

    std::string secondComment (bool withExperimental)
    {
        if (! withExperimental)
            return kRuleComment2;

        std::string names;

        for (const auto pid : elgato::pidsWith (elgato::Hardware::Experimental))
            if (const auto* model = elgato::findByPid (pid))
                names += std::string (names.empty() ? "" : ", ") + model->shortName;

        return "# Elgato Wave:3 (ClipGuard) and, because you switched on the experimental option, the " + names
             + " (mic gain). The Headroom uninstall.sh removes it.";
    }

    std::string ruleLine (uint16_t pid)
    {
        return "SUBSYSTEM==\"usb\", ATTR{idVendor}==\"0fd9\", ATTR{idProduct}==\"" + hex4 (pid) + "\", TAG+=\"uaccess\"";
    }
}

std::string ruleText (bool withExperimental)
{
    std::string text = std::string (kRuleComment1) + "\n" + secondComment (withExperimental) + "\n";

    for (const auto pid : products (withExperimental))
        text += ruleLine (pid) + "\n";

    return text;
}

std::string installScript (bool withExperimental)
{
    // Assembled from literals and the table of products only: nothing in it comes from the user, the environment or a
    // setting. The line that carries the rule is the rule lines above, each quoted, and the paths are written out:
    // /etc/udev/rules.d, the hidden temp name, the final name, and the four udevadm locations.
    std::string printfLine = std::string ("printf '%s\\n' '") + kRuleComment1 + "' '" + secondComment (withExperimental) + "'";

    for (const auto pid : products (withExperimental))
        printfLine += " '" + ruleLine (pid) + "'";

    std::string script = std::string ("umask 022 || exit 10\n"
                                      "d=/etc/udev/rules.d\n"
                                      "t=$d/.70-headroom-wave.rules.tmp\n"
                                      "mkdir -p \"$d\" || exit 11\n")
                       + printfLine + " > \"$t\" || { rm -f \"$t\"; exit 12; }\n"
                         "mv -f \"$t\" \"$d/70-headroom-wave.rules\" || { rm -f \"$t\"; exit 13; }\n"
                         "for u in /usr/bin/udevadm /bin/udevadm /usr/sbin/udevadm /sbin/udevadm; do [ -x \"$u\" ] && break; done\n"
                         "\"$u\" control --reload || echo \"udevadm control --reload did not succeed\" >&2\n";

    for (const auto pid : products (withExperimental))
        script += "\"$u\" trigger --action=add --attr-match=idVendor=0fd9 --attr-match=idProduct=" + hex4 (pid)
                + " || echo \"udevadm trigger did not succeed\" >&2\n";

    script += "\"$u\" settle --timeout=5 || echo \"udevadm settle did not succeed\" >&2\n"
              "exit 0\n";
    return script;
}

std::vector<std::string> pkexecArguments (const std::string& shell, bool withExperimental)
{
    return { shell, "-c", installScript (withExperimental) };
}

std::string describe (InstallResult::Outcome outcome)
{
    switch (outcome)
    {
        case InstallResult::Outcome::Done:              return {};
        case InstallResult::Outcome::Cancelled:         return "You cancelled the password box, so nothing was changed.";
        case InstallResult::Outcome::NotAuthorised:     return "That did not work: the password was wrong, or this account cannot make system changes.";
        case InstallResult::Outcome::NoPkexec:          return "This computer cannot ask for your password from here.";
        case InstallResult::Outcome::Failed:            return "Something went wrong while allowing hardware controls. The details are in Headroom's log.";
    }

    return {};
}

InstallResult installViaPkexec (const std::function<bool()>& shouldAbort, bool withExperimental)
{
    InstallResult result;

   #if defined (__linux__)
    // Both by absolute system path: nothing is looked up through PATH.
    const auto firstExecutable = [] (std::initializer_list<const char*> candidates) -> const char*
    {
        for (const char* candidate : candidates)
            if (::access (candidate, X_OK) == 0)
                return candidate;

        return nullptr;
    };

    const char* pkexec = firstExecutable ({ "/usr/bin/pkexec", "/bin/pkexec" });
    const char* shell = firstExecutable ({ "/bin/sh", "/usr/bin/sh" });

    if (pkexec == nullptr || shell == nullptr)
    {
        result.outcome = InstallResult::Outcome::NoPkexec;
        result.detail = pkexec == nullptr ? "pkexec was not found" : "/bin/sh was not found";
        return result;
    }

    int pipeFds[2] = { -1, -1 };

    if (::pipe2 (pipeFds, O_CLOEXEC) != 0)
    {
        result.detail = std::string ("pipe: ") + std::strerror (errno);
        return result;
    }

    posix_spawn_file_actions_t actions;
    ::posix_spawn_file_actions_init (&actions);
    ::posix_spawn_file_actions_addopen (&actions, 0, "/dev/null", O_RDONLY, 0);
    ::posix_spawn_file_actions_adddup2 (&actions, pipeFds[1], 1);
    ::posix_spawn_file_actions_adddup2 (&actions, pipeFds[1], 2);

    // pkexec <shell> -c <script>: three arguments, all constants.
    std::string program = "pkexec";
    auto arguments = pkexecArguments (shell, withExperimental);
    std::vector<char*> argv { program.data() };

    for (auto& argument : arguments)
        argv.push_back (argument.data());

    argv.push_back (nullptr);

    pid_t child = 0;
    const int spawned = ::posix_spawn (&child, pkexec, &actions, nullptr, argv.data(), environ);
    ::posix_spawn_file_actions_destroy (&actions);
    ::close (pipeFds[1]);

    if (spawned != 0)
    {
        ::close (pipeFds[0]);
        result.detail = std::string ("could not start pkexec: ") + std::strerror (spawned);
        return result;
    }

    ::fcntl (pipeFds[0], F_SETFL, O_NONBLOCK);

    std::string output;

    const auto drain = [&]
    {
        char buffer[512];

        for (;;)
        {
            const auto count = ::read (pipeFds[0], buffer, sizeof (buffer));

            if (count <= 0)
                break;

            if (output.size() < 4096)
                output.append (buffer, (size_t) count);
        }
    };

    const auto giveUpAt = std::chrono::steady_clock::now() + std::chrono::minutes (10);
    int status = 0;
    bool aborted = false, timedOut = false;

    for (;;)
    {
        drain();
        const pid_t finished = ::waitpid (child, &status, WNOHANG);

        if (finished == child)
            break;

        if (finished < 0 && errno != EINTR)
        {
            result.detail = std::string ("waitpid: ") + std::strerror (errno);
            ::close (pipeFds[0]);
            return result;
        }

        if ((shouldAbort && shouldAbort()) || std::chrono::steady_clock::now() > giveUpAt)
        {
            aborted = shouldAbort && shouldAbort();
            timedOut = ! aborted;
            ::kill (child, SIGTERM);
            ::waitpid (child, &status, 0);
            break;
        }

        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }

    drain();
    ::close (pipeFds[0]);

    if (aborted)
    {
        result.outcome = InstallResult::Outcome::Cancelled;
        result.detail = "stopped because Headroom is quitting";
        return result;
    }

    if (timedOut)
    {
        result.outcome = InstallResult::Outcome::Failed;
        result.detail = "pkexec did not finish in 10 minutes";
        return result;
    }

    if (WIFEXITED (status))
    {
        const int code = WEXITSTATUS (status);
        result.detail = "pkexec exit code " + std::to_string (code) + (output.empty() ? std::string() : "; output: " + output);

        // pkexec: 126 = the password box was dismissed, 127 = not authorised (wrong password, no polkit helper, not allowed).
        // The script never exits with either; it uses 10 to 13 for what it could not do.
        result.outcome = code == 0 ? InstallResult::Outcome::Done
                       : code == 126 ? InstallResult::Outcome::Cancelled
                       : code == 127 ? InstallResult::Outcome::NotAuthorised
                                     : InstallResult::Outcome::Failed;
    }
    else
    {
        result.detail = "pkexec ended by signal " + std::to_string (WIFSIGNALED (status) ? WTERMSIG (status) : 0);
    }

    return result;
   #else
    (void) shouldAbort;
    (void) withExperimental;
    result.outcome = InstallResult::Outcome::NoPkexec;
    result.detail = "not Linux";
    return result;
   #endif
}
}
