// "Allow hardware controls": the one-time udev rule that lets the person at the computer open the Wave:3's USB node, and the
// single `pkexec` that installs it.
//
// What runs as root is not Headroom. It is `/bin/sh -c <script>`, two root-owned system programs and one script that is a
// constant of this file: it contains no path, name or value that comes from the user, the environment or a setting, and
// nothing is interpolated into it. The script writes the rule text atomically and asks udev to look at the Wave:3 again.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace hr::wave::permission
{
constexpr const char* kRuleFileName = "70-headroom-wave.rules";
constexpr const char* kRulesDirectory = "/etc/udev/rules.d";

// The whole content of the rule file. TAG+="uaccess" gives the user at the seat access to the device node of exactly the
// models whose USB controls Headroom drives, not of every Elgato device: the original Wave:3 (0fd9:0070, tested) and, only
// when `withExperimental` (the user switched on the experimental option), the models with an experimental
// protocol (the Wave XLR, 0fd9:007d). No apostrophes: the script quotes it in '...'.
std::string ruleText (bool withExperimental = false);

// The script pkexec runs, as a constant: umask 022, write the rule to `.70-headroom-wave.rules.tmp` in the rules folder,
// `mv -f` it over the real name (a rename, so the file is the old one or the new one, never half of one), then
// `udevadm control --reload`, `udevadm trigger --action=add --attr-match=idVendor=0fd9 --attr-match=idProduct=0070` and
// `udevadm settle`, with udevadm found among the fixed system paths (/usr/bin, /bin, /usr/sbin, /sbin). Its exit code is
// 0 when the rule is in place (a udevadm that does not answer only prints a warning: the rule is read at the next start),
// 10 to 13 when it could not be written. Never 126 or 127, which pkexec uses for itself. With `withExperimental` the rule has
// the experimental models too, and udev is told about each product in it.
std::string installScript (bool withExperimental = false);

// What pkexec is given after its own name: { shell, "-c", installScript() }. Nothing else, and only these three.
std::vector<std::string> pkexecArguments (const std::string& shell, bool withExperimental = false);

// ---- Asking for it (from the app) ------------------------------------------------------------------------
struct InstallResult
{
    enum class Outcome { Done, Cancelled, NotAuthorised, NoPkexec, Failed };

    Outcome outcome = Outcome::Failed;
    std::string detail;                 // for the log: exit code and what the script printed
};

// Runs `pkexec /bin/sh -c <installScript()>` (pkexec and sh by absolute system paths) and waits for it: the password box
// is open meanwhile, and there is exactly one. `shouldAbort` is polled while waiting; when it says true the child is
// stopped and the outcome is Cancelled.
InstallResult installViaPkexec (const std::function<bool()>& shouldAbort, bool withExperimental = false);

// Plain words for what happened, for the settings page.
std::string describe (InstallResult::Outcome);
}
