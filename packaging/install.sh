#!/usr/bin/env bash
# Headroom installer. Needs no root, does not depend on the folder you start it from,
# and is safe to run again (it just updates everything).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

APP_DIR="$HOME/.local/opt/headroom"
BIN_DIR="$HOME/.local/bin"
APPS_DIR="$HOME/.local/share/applications"
ICON_DIR="$HOME/.local/share/icons/hicolor/scalable/apps"
WP_DIR="$HOME/.config/wireplumber/wireplumber.conf.d"
UNIT_DIR="$HOME/.config/systemd/user"

step() { printf '\n==> %s\n' "$*"; }
say()  { printf '    %s\n' "$*"; }

# When started from a file manager the terminal closes as soon as we exit,
# so wait for a key press. From a normal shell there is nothing to wait for.
maybe_pause() {
    [ -z "${HEADROOM_NO_PAUSE:-}" ] || return 0
    [ -t 0 ] && [ -t 1 ] || return 0
    case "$(ps -o comm= -p "$PPID" 2>/dev/null || true)" in
        bash|zsh|fish|sh|dash|ksh|tcsh|csh) return 0 ;;
    esac
    printf '\nPress Enter to close this window. '
    read -r _ || true
}

# With no terminal at all (double-click, "Run" without a console) show a small dialog instead.
tell_user() {
    [ -t 1 ] && return 0
    if command -v kdialog >/dev/null 2>&1; then
        kdialog --title "Headroom" --msgbox "$1" >/dev/null 2>&1 || true
    elif command -v zenity >/dev/null 2>&1; then
        zenity --info --title "Headroom" --text "$1" >/dev/null 2>&1 || true
    elif command -v notify-send >/dev/null 2>&1; then
        notify-send "Headroom" "$1" >/dev/null 2>&1 || true
    fi
}

fail() {
    printf '\nHeadroom could not be installed.\n%s\n' "$*" >&2
    tell_user "Headroom could not be installed. $*"
    maybe_pause
    exit 1
}

trap 'fail "Something unexpected went wrong (line $LINENO). Nothing has been harmed; you can run install.sh again."' ERR

# ------------------------------------------------------------------------------------------------

step "Checking your system"

[ "$(id -u)" -ne 0 ] || fail "Please run install.sh as yourself, not with sudo or as root."

for file in headroom share/headroom.desktop share/headroom.svg share/headroom.service share/51-headroom-wave.conf; do
    [ -f "$SCRIPT_DIR/$file" ] || fail "The file '$file' is missing next to install.sh. Please extract the whole download and try again."
done

command -v systemctl >/dev/null 2>&1 || fail "This computer does not use systemd, which Headroom needs to start itself."

if ! systemctl --user show-environment >/dev/null 2>&1; then
    fail "Your desktop session is not reachable (systemctl --user does not work). Please run this from a normal desktop login, not over plain SSH or su."
fi

pipewire_units="$(systemctl --user list-unit-files pipewire.service 2>/dev/null || true)"
case "$pipewire_units" in
    *pipewire.service*) ;;
    *) fail "PipeWire does not seem to be installed. It is standard on Bazzite; Headroom cannot work without it." ;;
esac

say "Looks good."

# ------------------------------------------------------------------------------------------------

step "Installing Headroom"

mkdir -p "$APP_DIR/share" "$BIN_DIR" "$APPS_DIR" "$ICON_DIR" "$WP_DIR" "$UNIT_DIR"

# Stop a running copy first so the program file can be replaced. Ask it to save and quit properly first (a copy started
# by hand too); best effort, and a no-op when nothing runs. `systemctl stop` below is only the fallback: it sends SIGTERM,
# which this Headroom also answers with a save, but an older one just dies.
timeout 10 "$SCRIPT_DIR/headroom" --quit >/dev/null 2>&1 || true

if systemctl --user is-active --quiet headroom.service 2>/dev/null; then
    say "Stopping the running Headroom..."
    systemctl --user stop headroom.service
fi

# Replace the program atomically; never leave a half-written file behind.
install -m 755 "$SCRIPT_DIR/headroom" "$APP_DIR/headroom.new"
mv -f "$APP_DIR/headroom.new" "$APP_DIR/headroom"

install -m 755 "$SCRIPT_DIR/install.sh" "$APP_DIR/install.sh"
[ ! -f "$SCRIPT_DIR/uninstall.sh" ] || install -m 755 "$SCRIPT_DIR/uninstall.sh" "$APP_DIR/uninstall.sh"
[ ! -f "$SCRIPT_DIR/README.txt" ] || install -m 644 "$SCRIPT_DIR/README.txt" "$APP_DIR/README.txt"
install -m 644 "$SCRIPT_DIR"/share/* "$APP_DIR/share/"

# The noise removal networks (Fast and Best) live next to the program: it looks for a "models" folder beside itself.
if [ -d "$SCRIPT_DIR/models" ]; then
    mkdir -p "$APP_DIR/models"
    install -m 644 "$SCRIPT_DIR"/models/* "$APP_DIR/models/"
    say "Noise removal models: $APP_DIR/models"
else
    say "Note: this download has no models folder, so noise removal will use its lightest mode."
fi

ln -sfn "$APP_DIR/headroom" "$BIN_DIR/headroom"
say "Program: $APP_DIR/headroom"

# Menu entry and icon. The menu entry points at the exact program path so it works
# even if ~/.local/bin is not on the PATH of your desktop.
sed "s|^Exec=@BINDIR@/headroom|Exec=\"$BIN_DIR/headroom\"|" "$SCRIPT_DIR/share/headroom.desktop" > "$APPS_DIR/headroom.desktop.new"
chmod 644 "$APPS_DIR/headroom.desktop.new"
mv -f "$APPS_DIR/headroom.desktop.new" "$APPS_DIR/headroom.desktop"
install -m 644 "$SCRIPT_DIR/share/headroom.svg" "$ICON_DIR/headroom.svg"
say "Menu entry: Headroom (in your application menu)"

update-desktop-database "$APPS_DIR" >/dev/null 2>&1 || true
gtk-update-icon-cache -q -t "$HOME/.local/share/icons/hicolor" >/dev/null 2>&1 || true
if command -v kbuildsycoca6 >/dev/null 2>&1; then kbuildsycoca6 >/dev/null 2>&1 || true; fi

# WirePlumber settings for the Wave:3. Only restart WirePlumber if they actually changed.
WP_CHANGED=0
if ! cmp -s "$SCRIPT_DIR/share/51-headroom-wave.conf" "$WP_DIR/51-headroom-wave.conf" 2>/dev/null; then
    install -m 644 "$SCRIPT_DIR/share/51-headroom-wave.conf" "$WP_DIR/51-headroom-wave.conf"
    WP_CHANGED=1
fi
say "Wave:3 settings: $WP_DIR/51-headroom-wave.conf"

# Files that other people (or an earlier fix attempt) put there for the Wave are never touched or deleted;
# we only mention them, because Headroom now takes care of the mic by itself.
for other in "$WP_DIR"/*.conf; do
    [ -f "$other" ] || continue
    [ "$(basename "$other")" != "51-headroom-wave.conf" ] || continue
    if grep -qiE 'Elgato_Wave|0x0fd9' "$other" 2>/dev/null; then
        say "Note: $other also configures the Wave:3."
        say "      Headroom now takes care of the mic, so that file is no longer needed and can be removed (Headroom leaves it alone)."
    fi
done

install -m 644 "$SCRIPT_DIR/share/headroom.service" "$UNIT_DIR/headroom.service"
say "Start-at-login service: $UNIT_DIR/headroom.service"

# ------------------------------------------------------------------------------------------------

step "Starting Headroom"

systemctl --user daemon-reload
systemctl --user enable headroom.service >/dev/null 2>&1

if [ "$WP_CHANGED" -eq 1 ]; then
    say "Applying the Wave:3 settings (your sound may crackle for a second)..."
    systemctl --user restart wireplumber.service >/dev/null 2>&1 || say "(Could not restart WirePlumber; the settings apply after your next login.)"
    sleep 2
fi

# A copy that is running without the service (started by hand, or from the menu while the service was stopped) would make
# the service below exit as "already running". Ask it to save and quit first; best effort, and a no-op when nothing runs.
timeout 10 "$SCRIPT_DIR/headroom" --quit >/dev/null 2>&1 || true

systemctl --user restart headroom.service

# Give it a moment, then make sure it really is running.
for _ in 1 2 3 4 5 6 7 8; do
    sleep 1
    systemctl --user is-active --quiet headroom.service && break
done

if ! systemctl --user is-active --quiet headroom.service; then
    say "The last lines of its log:"
    journalctl --user -u headroom.service -n 15 --no-pager 2>/dev/null | sed 's/^/      /' || true
    fail "Headroom was installed but did not start. Please send the lines above to whoever helped you set this up."
fi

# Open the window (the running copy shows itself).
( "$BIN_DIR/headroom" --show >/dev/null 2>&1 & ) || true

trap - ERR

step "All done"
say "Headroom is running and will start by itself every time you log in."
say "Its window is open now; you can open it again from the application menu."
say "In Discord, choose 'Headroom Mic' as your microphone (see README.txt)."
tell_user "Headroom is installed and running. Its window is open now."
maybe_pause
exit 0
