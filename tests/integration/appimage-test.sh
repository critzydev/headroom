#!/usr/bin/env bash
# Tests the AppImage's self-install, update and uninstall in a scratch HOME with a stub systemctl, pkexec and pw-metadata.
# It runs the real AppImage in its unattended modes (no window):
#
#   tests/integration/appimage-test.sh <Headroom-x.y.z-x86_64.AppImage>
#
# Without FUSE (a container) it sets APPIMAGE_EXTRACT_AND_RUN=1. The unattended modes need no display (a CI container has none).
set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <AppImage>" >&2
    exit 2
fi

APPIMAGE_SRC="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"

# The installer works for one user and a CI container is root: become a normal user.
if [ "$(id -u)" -eq 0 ]; then
    id hrtest >/dev/null 2>&1 || useradd -m hrtest
    HANDOVER="$(mktemp -d)"
    cp "$APPIMAGE_SRC" "$HANDOVER/"
    cp "${BASH_SOURCE[0]}" "$HANDOVER/appimage-test.sh"
    chmod -R a+rX "$HANDOVER"
    chmod a+rx "$HANDOVER"
    exec runuser -u hrtest -- bash "$HANDOVER/appimage-test.sh" "$HANDOVER/$(basename "$APPIMAGE_SRC")"
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

export HOME="$WORK/home"
export TMPDIR="$WORK/tmp"
export XDG_RUNTIME_DIR="$WORK/run"
export STUB_LOG="$WORK/calls.log"
export STUB_STATE="$WORK/state"
export APPIMAGE_EXTRACT_AND_RUN=1
mkdir -p "$HOME" "$TMPDIR" "$WORK/bin" "$STUB_STATE"
mkdir -m 700 "$XDG_RUNTIME_DIR"
unset XDG_CONFIG_HOME XDG_STATE_HOME XDG_DATA_HOME APPIMAGE APPDIR HEADROOM_INSTANCE HEADROOM_DATA_DIR
: >"$STUB_LOG"

# A systemctl that records what it is asked and pretends the service runs once it was (re)started.
cat >"$WORK/bin/systemctl" <<'STUB'
#!/bin/sh
echo "systemctl $*" >> "$STUB_LOG"
case "$*" in
    *"list-unit-files pipewire.service"*) echo "pipewire.service enabled enabled" ;;
    *"is-active"*"headroom.service"*) [ -f "$STUB_STATE/running" ] || exit 3 ;;
    *"restart headroom.service"*|*"start headroom.service"*) : > "$STUB_STATE/running" ;;
    *"stop"*"headroom.service"*|*"disable"*"headroom.service"*) rm -f "$STUB_STATE/running" ;;
esac
exit 0
STUB
chmod +x "$WORK/bin/systemctl"

# Stands in for pkexec (the program under test is given its path): keeps what it was given, and removes the scratch copy of the
# rule when the "password box" is not cancelled.
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

# pw-metadata: one default sink that is Headroom's, one source that is the user's own. Deleting is recorded.
cat >"$WORK/bin/pw-metadata" <<'STUB'
#!/bin/sh
echo "pw-metadata $*" >> "$STUB_LOG"
if [ "$#" -le 2 ]; then
    echo "update: id:0 key:'default.configured.audio.sink' value:'{ \"name\": \"headroom.sink\" }' type:'Spa:String:JSON'"
    echo "update: id:0 key:'default.configured.audio.source' value:'{ \"name\": \"alsa_input.usb-user-mic\" }' type:'Spa:String:JSON'"
fi
exit 0
STUB
chmod +x "$WORK/bin/pw-metadata"
export PATH="$WORK/bin:$PATH"

export HEADROOM_TEST_UDEV_RULE="$WORK/70-headroom-wave.rules"
export HEADROOM_TEST_PKEXEC="$WORK/bin/pkexec"

fail() { echo "FAIL: $*"; echo "---- calls so far"; cat "$STUB_LOG"; exit 1; }
ok()   { echo "PASS: $*"; }
calls() { grep -c -- "$1" "$STUB_LOG" || true; }

# Everything runs from another folder, like a double-click in a file manager.
run_hr() { ( cd / && "$APPIMAGE_SRC" "$@" ); }

APP="$HOME/.local/opt/headroom/Headroom.AppImage"
UNIT="$HOME/.config/systemd/user/headroom.service"
DESKTOP="$HOME/.local/share/applications/headroom.desktop"
ICON="$HOME/.local/share/icons/hicolor/scalable/apps/headroom.svg"
DROPIN="$HOME/.config/wireplumber/wireplumber.conf.d/51-headroom-wave.conf"

# ---- the AppImage runs and says what it is ----------------------------------------------------------------------------

VERSION_LINE="$(run_hr --version 2>&1 | tail -n 1)"
case "$VERSION_LINE" in
    "Headroom "[0-9]*) ok "--version prints: $VERSION_LINE" ;;
    *) fail "--version printed: $VERSION_LINE" ;;
esac

[ "$(HEADROOM_TEST_VERSION=99.0.0 run_hr --version 2>&1 | tail -n 1)" = "Headroom 99.0.0" ] || fail "HEADROOM_TEST_VERSION is not honoured"
ok "a newer build can be played with HEADROOM_TEST_VERSION"

# Somebody else's WirePlumber file for the Wave (an earlier fix attempt): must survive.
OTHER_DROPIN="$HOME/.config/wireplumber/wireplumber.conf.d/99-elgato-wave3.conf"
mkdir -p "$(dirname "$OTHER_DROPIN")"
echo 'monitor.alsa.rules = [ { matches = [ { device.name = "~alsa_card.usb-Elgato_Systems_Elgato_Wave_3*" } ] } ]' >"$OTHER_DROPIN"
cp "$OTHER_DROPIN" "$WORK/other-dropin.orig"

# ---- install ----------------------------------------------------------------------------------------------------------

run_hr --setup-unattended --source "$APPIMAGE_SRC" >"$WORK/install1.log" 2>&1 || { cat "$WORK/install1.log"; fail "--setup-unattended failed"; }

[ -x "$APP" ] && cmp -s "$APP" "$APPIMAGE_SRC" || fail "the AppImage was not copied to $APP"
[ -f "$UNIT" ] && grep -qx 'ExecStart=%h/.local/opt/headroom/Headroom.AppImage --background' "$UNIT" || fail "the unit does not start the installed AppImage"
grep -q '^Restart=on-failure' "$UNIT" && grep -q '^StartLimitBurst=6' "$UNIT" || fail "the unit lost its restart settings"
grep -qF "Exec=\"$APP\" --show" "$DESKTOP" || fail "the menu entry does not start the installed AppImage"
[ -f "$ICON" ] || fail "icon missing"
[ -f "$DROPIN" ] || fail "WirePlumber drop-in missing"
ok "the AppImage installed itself: program, unit, menu entry, icon and drop-in"

if command -v desktop-file-validate >/dev/null 2>&1; then
    desktop-file-validate "$DESKTOP" || fail "the menu entry does not validate"
    ok "the menu entry validates"
fi

cmp -s "$OTHER_DROPIN" "$WORK/other-dropin.orig" || fail "somebody else's WirePlumber file was changed"
grep -q "99-elgato-wave3.conf" "$WORK/install1.log" || fail "the other Wave drop-in was not mentioned"
ok "another Wave drop-in is mentioned and left alone"

[ "$(calls 'systemctl --user daemon-reload')" -ge 1 ] || fail "daemon-reload was not run"
[ "$(calls 'systemctl --user enable headroom.service')" -eq 1 ] || fail "the service was not enabled"
[ "$(calls 'restart wireplumber.service')" -eq 1 ] || fail "WirePlumber should have been restarted once"
[ "$(calls 'restart headroom.service')" -eq 1 ] || fail "the service was not started"
[ -f "$STUB_STATE/running" ] || fail "the service is not running"
ok "the service was enabled and started, WirePlumber restarted once for the new drop-in"

[ "$(run_hr --version 2>&1 | tail -n 1)" = "$VERSION_LINE" ] && [ "$("$APP" --version 2>&1 | tail -n 1)" = "$VERSION_LINE" ] || fail "the installed copy does not report the same version"
ok "the installed copy runs and reports $VERSION_LINE"

# ---- again: nothing changes -------------------------------------------------------------------------------------------

BEFORE="$(stat -c %Y "$APP")"
run_hr --setup-unattended --source "$APPIMAGE_SRC" >"$WORK/install2.log" 2>&1 || { cat "$WORK/install2.log"; fail "the second --setup-unattended failed"; }
[ "$(calls 'restart wireplumber.service')" -eq 1 ] || fail "an unchanged drop-in must not restart WirePlumber again"
[ "$(stat -c %Y "$APP")" = "$BEFORE" ] || fail "an identical copy was written again"
ok "running setup again is idempotent (no needless WirePlumber restart, no rewrite)"

# ---- update to a newer build ------------------------------------------------------------------------------------------

NEWER="$WORK/Headroom-newer.AppImage"
cp "$APPIMAGE_SRC" "$NEWER"
printf 'newer build' >>"$NEWER"          # different bytes after the image: what a new release is, as far as the installer can tell
chmod +x "$NEWER"
HEADROOM_TEST_VERSION=99.0.0 run_hr --setup-unattended --source "$NEWER" >"$WORK/update.log" 2>&1 || { cat "$WORK/update.log"; fail "the update failed"; }
cmp -s "$APP" "$NEWER" || fail "the installed file is not the newer build"
[ "$(calls 'restart headroom.service')" -eq 3 ] || fail "the service was not restarted for the update"
[ "$(calls 'restart wireplumber.service')" -eq 1 ] || fail "an update must not touch WirePlumber"
ok "an update replaces the installed file whole and restarts the service"

# ---- without start at login, without starting, with settings ---------------------------------------------------------

run_hr --setup-unattended --source "$APPIMAGE_SRC" --no-login-start --no-default-devices --no-start >"$WORK/install3.log" 2>&1 || { cat "$WORK/install3.log"; fail "--setup-unattended with options failed"; }
[ "$(calls 'systemctl --user disable headroom.service')" -eq 1 ] || fail "--no-login-start did not disable the unit"
grep -q 'startAtLogin="0"' "$HOME/.config/headroom/state.xml" && grep -q 'makeDefault="0"' "$HOME/.config/headroom/state.xml" || fail "the options did not reach state.xml"
[ "$(calls 'restart headroom.service')" -eq 3 ] || fail "--no-start started the service"
ok "the options reach the settings: no start at login, not the default device, no start"

# ---- uninstall --------------------------------------------------------------------------------------------------------

echo "SUBSYSTEM==\"usb\"" >"$HEADROOM_TEST_UDEV_RULE"
EXPECTED_REMOVE_SCRIPT='rm -f /etc/udev/rules.d/70-headroom-wave.rules /etc/udev/rules.d/.70-headroom-wave.rules.tmp || exit 10
for u in /usr/bin/udevadm /bin/udevadm /usr/sbin/udevadm /sbin/udevadm; do [ -x "$u" ] && break; done
"$u" control --reload || echo "udevadm control --reload did not succeed" >&2
"$u" trigger --action=add --attr-match=idVendor=0fd9 --attr-match=idProduct=0070 || echo "udevadm trigger did not succeed" >&2
exit 0'

( cd / && PKEXEC_STUB_EXIT=126 "$APPIMAGE_SRC" --uninstall-unattended >"$WORK/uninstall1.log" 2>&1 ) || { cat "$WORK/uninstall1.log"; fail "uninstall failed when the password box was cancelled"; }
[ -e "$HEADROOM_TEST_UDEV_RULE" ] || fail "the rule was removed although the password box was cancelled"
grep -q "sudo rm /etc/udev/rules.d/70-headroom-wave.rules" "$WORK/uninstall1.log" || fail "uninstall did not say how to remove the rule by hand"
[ ! -e "$APP" ] && [ ! -e "$UNIT" ] && [ ! -e "$DESKTOP" ] && [ ! -e "$ICON" ] && [ ! -e "$DROPIN" ] || fail "something of Headroom is left"
ok "a cancelled password box leaves the rule, says how to remove it, and the rest is removed"

cmp -s "$OTHER_DROPIN" "$WORK/other-dropin.orig" || fail "uninstall touched somebody else's WirePlumber file"
[ -f "$HOME/.config/headroom/state.xml" ] || fail "uninstall deleted the settings without being asked"
[ ! -d "$HOME/.local/opt/headroom" ] || fail "the program folder is still there"
[ "$(calls "pw-metadata -n default -d 0 default.configured.audio.sink")" -ge 1 ] || fail "Headroom's default sink was not given back"
[ "$(calls "pw-metadata -n default -d 0 default.configured.audio.source")" -eq 0 ] || fail "the user's own default source was cleared"
ok "uninstall keeps the settings and other people's files, and gives back only Headroom's default device"

: >"$STUB_LOG"
( cd / && "$APPIMAGE_SRC" --uninstall-unattended >"$WORK/uninstall2.log" 2>&1 ) || { cat "$WORK/uninstall2.log"; fail "uninstall with the rule present failed"; }
[ ! -e "$HEADROOM_TEST_UDEV_RULE" ] || fail "the udev rule is still there"
[ "$(grep -c '^pkexec' "$STUB_LOG")" -eq 1 ] || fail "uninstall should have asked for the password exactly once"
grep -q "^pkexec called with 3 arguments$" "$STUB_LOG" || fail "uninstall gave pkexec something other than a shell, -c and a script"
[ "$(cat "$STUB_STATE/pkexec-1")" = "/bin/sh" ] && [ "$(cat "$STUB_STATE/pkexec-2")" = "-c" ] || fail "pkexec was not asked to run /bin/sh -c"
[ "$(cat "$STUB_STATE/pkexec-3")" = "$EXPECTED_REMOVE_SCRIPT" ] || fail "the script given to pkexec is not the constant one"
ok "the udev rule is removed with one pkexec of /bin/sh and a constant script"

: >"$STUB_LOG"
( cd / && "$APPIMAGE_SRC" --uninstall-unattended >"$WORK/uninstall3.log" 2>&1 ) || { cat "$WORK/uninstall3.log"; fail "the second uninstall failed"; }
grep -q "pkexec" "$STUB_LOG" && fail "uninstall asked for a password although there was no udev rule" || true
ok "uninstall is idempotent, and with no rule it asks for nothing"

# ---- keep the file where an AppImage manager put it ----------------------------------------------------------------

mkdir -p "$HOME/AppImages"
cp "$APPIMAGE_SRC" "$HOME/AppImages/Headroom.AppImage"
chmod +x "$HOME/AppImages/Headroom.AppImage"
run_hr --setup-unattended --source "$HOME/AppImages/Headroom.AppImage" --keep-in-place --no-start >"$WORK/install4.log" 2>&1 || { cat "$WORK/install4.log"; fail "--keep-in-place failed"; }
grep -qx 'ExecStart=%h/AppImages/Headroom.AppImage --background' "$UNIT" || fail "the unit does not point at the file in AppImages"
[ ! -e "$APP" ] || fail "--keep-in-place copied the file anyway"
ok "--keep-in-place points the service at the file where it is"

( cd / && "$APPIMAGE_SRC" --uninstall-unattended --delete-settings >"$WORK/uninstall4.log" 2>&1 ) || { cat "$WORK/uninstall4.log"; fail "uninstall --delete-settings failed"; }
[ -f "$HOME/AppImages/Headroom.AppImage" ] || fail "uninstall deleted the AppImage that the manager looks after"
[ ! -e "$UNIT" ] || fail "the unit is still there"
[ ! -e "$HOME/.config/headroom" ] || fail "--delete-settings left the settings"
ok "uninstall leaves the managed AppImage alone, and --delete-settings deletes the settings"

echo "ALL APPIMAGE CHECKS PASSED"
