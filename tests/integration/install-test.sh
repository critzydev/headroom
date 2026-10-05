#!/usr/bin/env bash
# Runs the release tarball's install.sh and uninstall.sh against a scratch HOME with a stub systemctl,
# from a different working directory (like a double-click in a file manager), twice each.
#
#   tests/integration/install-test.sh <headroom-x.y.z-x86_64.tar.gz>
set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <tarball>" >&2
    exit 2
fi

TARBALL="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"

# install.sh refuses to run as root (it is a per-user install), and CI containers are root: become a normal user.
if [ "$(id -u)" -eq 0 ]; then
    id hrtest >/dev/null 2>&1 || useradd -m hrtest
    HANDOVER="$(mktemp -d)"
    cp "$TARBALL" "$HANDOVER/"
    cp "${BASH_SOURCE[0]}" "$HANDOVER/install-test.sh"
    chmod -R a+rX "$HANDOVER"
    exec runuser -u hrtest -- bash "$HANDOVER/install-test.sh" "$HANDOVER/$(basename "$TARBALL")"
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

export HOME="$WORK/home"
export STUB_LOG="$WORK/systemctl.log"
export STUB_STATE="$WORK/state"
export HEADROOM_NO_PAUSE=1
mkdir -p "$HOME" "$WORK/bin" "$STUB_STATE"
: >"$STUB_LOG"

# A systemctl that records what it is asked and pretends the service runs once it was (re)started.
cat >"$WORK/bin/systemctl" <<'STUB'
#!/bin/sh
echo "systemctl $*" >> "$STUB_LOG"
case "$*" in
    *"list-unit-files pipewire.service"*) echo "pipewire.service enabled enabled" ;;
    *"is-active"*"headroom.service"*) [ -f "$STUB_STATE/running" ] || exit 3 ;;
    *"restart headroom.service"*|*"start headroom.service"*) : > "$STUB_STATE/running" ;;
    *"stop headroom.service"*|*"disable"*"headroom.service"*) rm -f "$STUB_STATE/running" ;;
esac
exit 0
STUB
chmod +x "$WORK/bin/systemctl"

# A pkexec that records what it is asked. uninstall.sh runs exactly `pkexec /bin/sh -c <constant script>`: the stub keeps
# the number of arguments, the shell, the flag and the script text so that the test can check all of them, and, as this user,
# stands in for the removal by deleting the scratch copy of the rule. PKEXEC_STUB_EXIT makes it fail the way a cancelled
# password box does.
cat >"$WORK/bin/pkexec" <<'STUB'
#!/bin/sh
echo "pkexec called with $# arguments" >> "$STUB_LOG"
printf '%s' "$1" > "$STUB_STATE/pkexec-1"
printf '%s' "$2" > "$STUB_STATE/pkexec-2"
printf '%s' "$3" > "$STUB_STATE/pkexec-3"
[ "${PKEXEC_STUB_EXIT:-0}" = "0" ] || exit "$PKEXEC_STUB_EXIT"
rm -f "$HEADROOM_TEST_UDEV_RULE"
exit 0
STUB
chmod +x "$WORK/bin/pkexec"
export PATH="$WORK/bin:$PATH"

# The system file "Allow hardware controls" writes lives under /etc; the installer test points uninstall.sh's look at a scratch copy.
export HEADROOM_TEST_UDEV_RULE="$WORK/70-headroom-wave.rules"

fail() { echo "FAIL: $*"; exit 1; }
ok()   { echo "PASS: $*"; }

# A stand-in program: the real one is not needed to test the installer.
printf '#!/bin/sh\nexit 0\n' >"$WORK/headroom-fake"
chmod +x "$WORK/headroom-fake"

# Build a tarball from the same scripts if we were not given one built from the repo (kept simple: use the given one).
mkdir -p "$WORK/extract"
tar xzf "$TARBALL" -C "$WORK/extract"
PKG="$WORK/extract/headroom"
[ -f "$PKG/install.sh" ] || fail "install.sh is missing from the tarball"
# The real binary is large and irrelevant here; replace it so the "start" in the script is instant.
cp "$WORK/headroom-fake" "$PKG/headroom"

# Somebody else's WirePlumber file for the Wave (an earlier fix attempt): must survive and be mentioned.
OTHER_DROPIN="$HOME/.config/wireplumber/wireplumber.conf.d/99-elgato-wave3.conf"
mkdir -p "$(dirname "$OTHER_DROPIN")"
cat >"$OTHER_DROPIN" <<'CONF'
monitor.alsa.rules = [ { matches = [ { device.name = "~alsa_card.usb-Elgato_Systems_Elgato_Wave_3*" } ]
                         actions = { update-props = { device.profile = "pro-audio" } } } ]
CONF
cp "$OTHER_DROPIN" "$WORK/other-dropin.orig"

# ---- install (twice), from another directory -----------------------------------------------------------------

( cd / && bash "$PKG/install.sh" >"$WORK/install1.log" 2>&1 ) || { cat "$WORK/install1.log"; fail "install.sh failed"; }
ok "install.sh runs from another working directory"

cmp -s "$OTHER_DROPIN" "$WORK/other-dropin.orig" || fail "install.sh changed somebody else's WirePlumber file"
grep -q "99-elgato-wave3.conf" "$WORK/install1.log" || fail "install.sh did not mention the other WirePlumber file for the Wave"
ok "another Wave drop-in is mentioned but left alone"

APP="$HOME/.local/opt/headroom"
[ -x "$APP/headroom" ] || fail "program not installed"
[ -L "$HOME/.local/bin/headroom" ] && [ "$(readlink "$HOME/.local/bin/headroom")" = "$APP/headroom" ] || fail "symlink in ~/.local/bin is wrong"
[ -x "$APP/uninstall.sh" ] || fail "uninstall.sh not kept in the app folder"
grep -q "^Exec=\"$HOME/.local/bin/headroom\" --show" "$HOME/.local/share/applications/headroom.desktop" || fail "desktop file Exec line is wrong"
[ -f "$HOME/.local/share/icons/hicolor/scalable/apps/headroom.svg" ] || fail "icon missing"
[ -f "$HOME/.config/wireplumber/wireplumber.conf.d/51-headroom-wave.conf" ] || fail "WirePlumber drop-in missing"
grep -q "ExecStart=%h/.local/bin/headroom --background" "$HOME/.config/systemd/user/headroom.service" || fail "unit is wrong"
[ -f "$APP/models/DeepFilterNet3_ll_onnx.tar.gz" ] && [ -f "$APP/models/DeepFilterNet3_onnx.tar.gz" ] || fail "the noise removal models were not installed next to the program"
ok "everything was installed where it should be"

grep -q "systemctl --user daemon-reload" "$STUB_LOG" || fail "daemon-reload was not run"
grep -q "systemctl --user enable headroom.service" "$STUB_LOG" || fail "the service was not enabled"
grep -q "systemctl --user restart headroom.service" "$STUB_LOG" || fail "the service was not started"
[ "$(grep -c 'restart wireplumber.service' "$STUB_LOG")" -eq 1 ] || fail "WirePlumber should have been restarted once"
ok "the service was enabled and started, WirePlumber restarted for the new drop-in"

( cd / && bash "$PKG/install.sh" >"$WORK/install2.log" 2>&1 ) || { cat "$WORK/install2.log"; fail "second install.sh run failed"; }
[ "$(grep -c 'restart wireplumber.service' "$STUB_LOG")" -eq 1 ] || fail "an unchanged drop-in must not restart WirePlumber again"
ok "install.sh is idempotent (no needless WirePlumber restart)"

# ---- uninstall (twice) ------------------------------------------------------------------------------------------

mkdir -p "$HOME/.config/headroom"
echo "<HEADROOM/>" >"$HOME/.config/headroom/state.xml"

( cd / && bash "$APP/uninstall.sh" >"$WORK/uninstall1.log" 2>&1 ) || { cat "$WORK/uninstall1.log"; fail "uninstall.sh failed"; }

[ ! -e "$APP" ] || fail "app folder still there"
[ ! -e "$HOME/.local/bin/headroom" ] || fail "symlink still there"
[ ! -e "$HOME/.local/share/applications/headroom.desktop" ] || fail "desktop file still there"
[ ! -e "$HOME/.local/share/icons/hicolor/scalable/apps/headroom.svg" ] || fail "icon still there"
[ ! -e "$HOME/.config/wireplumber/wireplumber.conf.d/51-headroom-wave.conf" ] || fail "WirePlumber drop-in still there"
cmp -s "$OTHER_DROPIN" "$WORK/other-dropin.orig" || fail "uninstall.sh touched somebody else's WirePlumber file"
[ ! -e "$HOME/.config/systemd/user/headroom.service" ] || fail "unit still there"
[ -f "$HOME/.config/headroom/state.xml" ] || fail "uninstall deleted the user's settings without --purge"
grep -q "disable --now headroom.service" "$STUB_LOG" || fail "the service was not disabled"
ok "uninstall.sh removed everything and kept the settings"
grep -q "pkexec" "$STUB_LOG" && fail "uninstall.sh asked for a password although there was no udev rule" || true
ok "with no udev rule, uninstall.sh asks for nothing"

( cd / && bash "$PKG/uninstall.sh" --purge >"$WORK/uninstall2.log" 2>&1 ) || { cat "$WORK/uninstall2.log"; fail "second uninstall.sh run failed"; }
[ ! -e "$HOME/.config/headroom" ] || fail "--purge left the settings behind"
ok "uninstall.sh is idempotent and --purge deletes the settings"

# ---- uninstall with the udev rule from "Allow hardware controls" ---------------------------------------------------------

# What may run as root is exactly this: /bin/sh, -c, and this constant script (no path, variable or value of ours in it).
EXPECTED_REMOVE_SCRIPT='rm -f /etc/udev/rules.d/70-headroom-wave.rules /etc/udev/rules.d/.70-headroom-wave.rules.tmp || exit 10
for u in /usr/bin/udevadm /bin/udevadm /usr/sbin/udevadm /sbin/udevadm; do [ -x "$u" ] && break; done
"$u" control --reload || echo "udevadm control --reload did not succeed" >&2
"$u" trigger --action=add --attr-match=idVendor=0fd9 --attr-match=idProduct=0070 || echo "udevadm trigger did not succeed" >&2
exit 0'

echo "SUBSYSTEM==\"usb\"" >"$HEADROOM_TEST_UDEV_RULE"
( cd / && bash "$PKG/uninstall.sh" >"$WORK/uninstall3.log" 2>&1 ) || { cat "$WORK/uninstall3.log"; fail "uninstall.sh failed with the udev rule present"; }
[ ! -e "$HEADROOM_TEST_UDEV_RULE" ] || fail "uninstall.sh left the udev rule behind"
[ "$(grep -c '^pkexec' "$STUB_LOG")" -eq 1 ] || fail "uninstall.sh should have asked for the password exactly once"
grep -q "^pkexec called with 3 arguments$" "$STUB_LOG" || fail "uninstall.sh gave pkexec something other than a shell, -c and a script"
[ "$(cat "$STUB_STATE/pkexec-1")" = "/bin/sh" ] || fail "pkexec was not asked to run /bin/sh"
[ "$(cat "$STUB_STATE/pkexec-2")" = "-c" ] || fail "pkexec was not given -c"
[ "$(cat "$STUB_STATE/pkexec-3")" = "$EXPECTED_REMOVE_SCRIPT" ] || fail "the script given to pkexec is not the constant one"
ok "uninstall.sh removes the udev rule with one pkexec of /bin/sh and a constant script"

echo "SUBSYSTEM==\"usb\"" >"$HEADROOM_TEST_UDEV_RULE"
( cd / && PKEXEC_STUB_EXIT=126 bash "$PKG/uninstall.sh" >"$WORK/uninstall4.log" 2>&1 ) || { cat "$WORK/uninstall4.log"; fail "a cancelled password box must not make uninstall.sh fail"; }
[ -e "$HEADROOM_TEST_UDEV_RULE" ] || fail "the rule was removed although the password box was cancelled"
grep -q "sudo rm /etc/udev/rules.d/70-headroom-wave.rules" "$WORK/uninstall4.log" || fail "uninstall.sh did not say how to remove the rule by hand"
ok "a cancelled password box leaves the rule, and uninstall.sh says how to remove it"

echo "ALL INSTALLER CHECKS PASSED"
