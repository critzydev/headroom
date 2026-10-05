#!/usr/bin/env bash
# Headroom uninstaller: undoes everything install.sh did. Your settings are kept
# unless you run it with --purge.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="$HOME/.local/opt/headroom"
BIN_DIR="$HOME/.local/bin"
APPS_DIR="$HOME/.local/share/applications"
ICON_DIR="$HOME/.local/share/icons/hicolor/scalable/apps"
WP_DIR="$HOME/.config/wireplumber/wireplumber.conf.d"
UNIT_DIR="$HOME/.config/systemd/user"

PURGE=0
[ "${1:-}" != "--purge" ] || PURGE=1

step() { printf '\n==> %s\n' "$*"; }
say()  { printf '    %s\n' "$*"; }

maybe_pause() {
    [ -z "${HEADROOM_NO_PAUSE:-}" ] || return 0
    [ -t 0 ] && [ -t 1 ] || return 0
    case "$(ps -o comm= -p "$PPID" 2>/dev/null || true)" in
        bash|zsh|fish|sh|dash|ksh|tcsh|csh) return 0 ;;
    esac
    printf '\nPress Enter to close this window. '
    read -r _ || true
}

# Removes a default-device setting, but only if it points at one of Headroom's devices.
clear_default_if_ours() {
    local key="$1" current
    command -v pw-metadata >/dev/null 2>&1 || return 0
    current="$(timeout 5 pw-metadata -n default 2>/dev/null || true)"
    current="$(grep "key:'$key'" <<<"$current" || true)"
    if grep -q 'headroom\.' <<<"$current"; then
        timeout 5 pw-metadata -n default -d 0 "$key" >/dev/null 2>&1 || true
        say "Cleared $key"
    fi
}

step "Stopping Headroom"
# Ask a running copy to save and quit by itself first (a clean shutdown; the systemctl stop below is a plain SIGTERM).
# Best effort, and idempotent: with nothing running, or when this fails, it does nothing. It is the program that came
# with this script, so it knows --quit (an older running copy simply has nothing to answer and is stopped below).
if [ -x "$SCRIPT_DIR/headroom" ]; then
    timeout 10 "$SCRIPT_DIR/headroom" --quit >/dev/null 2>&1 || true
fi
if [ -f "$UNIT_DIR/headroom.service" ] || systemctl --user is-enabled --quiet headroom.service 2>/dev/null; then
    systemctl --user disable --now headroom.service >/dev/null 2>&1 || true
fi
# A copy that was started by hand rather than by the service.
pkill -u "$(id -u)" -x headroom >/dev/null 2>&1 || true
sleep 1

step "Giving your sound settings back"
# Headroom is gone now, so nothing will put these back.
clear_default_if_ours default.configured.audio.sink
clear_default_if_ours default.configured.audio.source

step "Removing files"
WP_HAD_DROPIN=0
[ ! -f "$WP_DIR/51-headroom-wave.conf" ] || WP_HAD_DROPIN=1

rm -f "$UNIT_DIR/headroom.service"
rm -f "$WP_DIR/51-headroom-wave.conf"
rm -f "$APPS_DIR/headroom.desktop" "$ICON_DIR/headroom.svg"
if [ -L "$BIN_DIR/headroom" ]; then rm -f "$BIN_DIR/headroom"; fi
rm -rf "$APP_DIR"
say "Program, menu entry, icon and start-at-login service removed."

if [ "$PURGE" -eq 1 ]; then
    rm -rf "$HOME/.config/headroom" "$HOME/.local/state/headroom"
    say "Your Headroom settings and logs were deleted."
else
    say "Your settings are kept in ~/.config/headroom (run uninstall.sh --purge to delete them too)."
fi

systemctl --user daemon-reload >/dev/null 2>&1 || true
update-desktop-database "$APPS_DIR" >/dev/null 2>&1 || true
if command -v kbuildsycoca6 >/dev/null 2>&1; then kbuildsycoca6 >/dev/null 2>&1 || true; fi

if [ "$WP_HAD_DROPIN" -eq 1 ]; then
    step "Restoring the standard Wave:3 behaviour"
    systemctl --user restart wireplumber.service >/dev/null 2>&1 || say "(Could not restart WirePlumber; it applies after your next login.)"
fi

# "Allow hardware controls" (ClipGuard) put one system file in place, after asking for a password. Take it out again, but
# only if it is there: with no such file nothing is asked. It is the one thing here that needs a password, so if that box is
# cancelled the rest stays removed and we say how to remove this file by hand.
#
# What runs as root is /bin/sh with the constant script below, once: it names no variable, path or value from here, the
# user or the environment (the same approach as Headroom's own "Allow hardware controls"). HEADROOM_TEST_UDEV_RULE only
# moves the place where we look for the file, for the installer test; it never reaches the script.
UDEV_RULE="${HEADROOM_TEST_UDEV_RULE:-/etc/udev/rules.d/70-headroom-wave.rules}"
REMOVE_SCRIPT='rm -f /etc/udev/rules.d/70-headroom-wave.rules /etc/udev/rules.d/.70-headroom-wave.rules.tmp || exit 10
for u in /usr/bin/udevadm /bin/udevadm /usr/sbin/udevadm /sbin/udevadm; do [ -x "$u" ] && break; done
"$u" control --reload || echo "udevadm control --reload did not succeed" >&2
"$u" trigger --action=add --attr-match=idVendor=0fd9 --attr-match=idProduct=0070 || echo "udevadm trigger did not succeed" >&2
exit 0'
if [ -e "$UDEV_RULE" ]; then
    step "Removing the permission for the Wave:3's hardware controls"
    say "Headroom set this up once so it could control ClipGuard. Removing it needs your password."
    if command -v pkexec >/dev/null 2>&1 && pkexec /bin/sh -c "$REMOVE_SCRIPT"; then
        say "Removed."
    else
        say "It was not removed (the password box was cancelled, or this computer cannot show one)."
        say "It does no harm. To remove it yourself, run this in a terminal:"
        say "    sudo rm /etc/udev/rules.d/70-headroom-wave.rules && sudo udevadm control --reload"
    fi
fi

step "Headroom has been removed"
say "Discord and your games will use your normal sound devices again."
maybe_pause
exit 0
