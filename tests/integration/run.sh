#!/usr/bin/env bash
# Headroom end-to-end test on a private PipeWire graph.
#
#   tests/integration/run.sh <path-to-headroom-binary> [log-dir]
#
# Starts its own D-Bus session, PipeWire and WirePlumber (with the shipped WirePlumber drop-in),
# fakes an Elgato Wave:3 (and later a Wave:1 and an ordinary stereo mic), runs headroom under Xvfb and checks the graph and the audio.
# Prints PASS/FAIL lines; exits non-zero if anything failed. Logs and recordings end up in log-dir.
#
# Needs: pipewire wireplumber pipewire-utils (pw-cli pw-dump pw-link pw-play pw-record),
#        dbus-run-session, xvfb-run (+ xauth), python3.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

if [ "$#" -lt 1 ]; then
    echo "usage: $0 <path-to-headroom-binary> [log-dir]" >&2
    exit 2
fi

BIN="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
LOG_DIR="${2:-$PWD/it-logs}"
mkdir -p "$LOG_DIR"
LOG_DIR="$(cd "$LOG_DIR" && pwd)"

if [ ! -x "$BIN" ]; then
    echo "FAIL: $BIN is not an executable file" >&2
    exit 2
fi

# Everything else runs inside a private D-Bus session bus.
if [ -z "${HEADROOM_IT_INNER:-}" ]; then
    export HEADROOM_IT_INNER=1
    exec dbus-run-session -- "$BASH" "${BASH_SOURCE[0]}" "$BIN" "$LOG_DIR"
fi

# ------------------------------------------------------------------------------------------------
# Environment: nothing here may touch the real user session.

WORK="$(mktemp -d /tmp/headroom-it.XXXXXX)"
export HOME="$WORK/home"
export XDG_RUNTIME_DIR="$WORK/run"
export XDG_CONFIG_HOME="$WORK/config"
export XDG_STATE_HOME="$WORK/state"
export XDG_DATA_HOME="$WORK/data"
export XDG_CACHE_HOME="$WORK/cache"
mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_STATE_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME"
mkdir -m 700 -p "$XDG_RUNTIME_DIR"

ANALYSE=(python3 "$HERE/analyse.py")

SINK="alsa_output.usb-Elgato_Systems_Elgato_Wave_3_TEST-00.analog-stereo"
SRC="alsa_input.usb-Elgato_Systems_Elgato_Wave_3_TEST-00.mono-fallback"
CHANNELS=(system game music chat)
HR_LOG="$XDG_STATE_HOME/headroom/headroom.log"
PIDFILE="$WORK/headroom.pid"

FAILS=0
PIDS=()
FEEDER_PID=""
FOLLOWER_PID=""
HR_PID=""
HR_JOB=""      # the xvfb-run that runs Headroom (its exit status is Headroom's)
LOG_MARK=0

pass() { printf 'PASS: %s\n' "$*"; }
fail() { printf 'FAIL: %s\n' "$*"; FAILS=$((FAILS + 1)); }
note() { printf 'NOTE: %s\n' "$*"; }
say()  { printf '== %s\n' "$*"; }

# check "label" command...  (analyse.py prints "OK: ..." or "FAIL: ...")
check() {
    local label="$1" output rc
    shift
    output="$("$@" 2>&1)"
    rc=$?
    if [ "$rc" -eq 0 ]; then
        pass "$label - ${output#OK: }"
    else
        fail "$label - ${output#FAIL: }"
    fi
}

wait_for() {
    local timeout="$1" end
    shift
    end=$((SECONDS + timeout))
    until "$@" >/dev/null 2>&1; do
        [ "$SECONDS" -lt "$end" ] || return 1
        sleep 0.3
    done
}

# expect_clean_exit "label" <pid of the background xvfb-run>: xvfb-run ends with the exit status of the program it ran, so this
# is Headroom's own status after a --quit. systemd restarts headroom.service on any status but 0 (Restart=on-failure), so an
# orderly quit that ends in 1 would bring Headroom straight back in the middle of an update or a logout.
expect_clean_exit() {
    local label="$1" job="$2" rc waited=0
    while kill -0 "$job" 2>/dev/null; do
        waited=$((waited + 1))
        if [ "$waited" -gt 150 ]; then
            fail "$label - still not finished 15 s after --quit"
            return 1
        fi
        sleep 0.1
    done
    wait "$job"
    rc=$?
    if [ "$rc" -eq 0 ]; then
        pass "$label - exit status 0"
    else
        fail "$label - exit status $rc (systemd restarts the service on anything but 0)"
        return 1
    fi
}

cleanup() {
    set +e
    say "collecting logs"
    pw-dump >"$LOG_DIR/pw-dump-final.json" 2>/dev/null
    pw-link -l >"$LOG_DIR/pw-link-final.txt" 2>/dev/null
    cp -f "$XDG_STATE_HOME"/headroom/* "$LOG_DIR/" 2>/dev/null
    cp -f "$WORK"/*.wav "$LOG_DIR/" 2>/dev/null
    for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null; done
    sleep 1
    for pid in "${PIDS[@]}"; do kill -9 "$pid" 2>/dev/null; done
    pkill -9 -u "$(id -u)" -f "Xvfb" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

# Tools must exist before anything is started.
for tool in pipewire wireplumber pw-cli pw-dump pw-link pw-play pw-record xvfb-run python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        fail "required tool not found: $tool"
    fi
done
[ "$FAILS" -eq 0 ] || exit 1

{
    pipewire --version
    wireplumber --version
} >"$LOG_DIR/versions.txt" 2>&1
cat "$LOG_DIR/versions.txt"

# ------------------------------------------------------------------------------------------------
# Helpers for the scenarios

# Headroom's log lines since start_headroom (the log file is appended to across runs).
log_since() { tail -n +"$((LOG_MARK + 1))" "$HR_LOG" 2>/dev/null; }
log_has()   { local text; text="$(log_since)"; grep -q -- "$1" <<<"$text"; }
log_count() { local text; text="$(log_since)"; grep -c -- "$1" <<<"$text" || true; }

link_feeder() {
    local outputs inputs out in
    outputs="$(pw-link -o 2>/dev/null)" || return 1
    inputs="$(pw-link -i 2>/dev/null)" || return 1
    out="$(grep -m1 '^hr-it-mic-feeder:' <<<"$outputs")" || return 1
    in="$(grep -m1 "^$SRC:input" <<<"$inputs")" || return 1
    pw-link "$out" "$in"
}

# The fake mic is fed by pw-play. Autoconnect is off so WirePlumber leaves it alone; we link it by hand.
start_feeder() {
    pw-play -P "node.name=hr-it-mic-feeder node.autoconnect=false" "$WORK/mic440.wav" >>"$LOG_DIR/mic-feeder.log" 2>&1 &
    FEEDER_PID=$!
    PIDS+=("$FEEDER_PID")
    wait_for 20 link_feeder
}

stop_feeder() {
    [ -n "$FEEDER_PID" ] || return 0
    kill "$FEEDER_PID" 2>/dev/null
    wait "$FEEDER_PID" 2>/dev/null
    FEEDER_PID=""
}

start_headroom() {
    local _
    rm -f "$PIDFILE"
    LOG_MARK="$(wc -l <"$HR_LOG" 2>/dev/null || echo 0)"
    PIPEWIRE_DEBUG=3 xvfb-run -a -s "-screen 0 1280x800x24" sh -c 'echo $$ > "$1"; shift; exec env HEADROOM_TEST_NO_FX=1 "$@"' \
        sh "$PIDFILE" "$BIN" --background >>"$LOG_DIR/headroom-stdout.log" 2>&1 &
    HR_JOB=$!
    PIDS+=("$HR_JOB")

    HR_PID=""
    for _ in $(seq 1 60); do
        sleep 0.5
        HR_PID="$(cat "$PIDFILE" 2>/dev/null || true)"
        [ -n "$HR_PID" ] && break
    done
    sleep 1

    if [ -z "$HR_PID" ] || ! kill -0 "$HR_PID" 2>/dev/null; then
        fail "headroom did not start (or exited immediately)"
        echo "--- headroom stdout/stderr:"
        cat "$LOG_DIR/headroom-stdout.log"
        echo "--- headroom log:"
        cat "$HR_LOG" 2>/dev/null
        echo "--- processes:"
        ps -eo pid,ppid,stat,cmd | grep -Ei "headroom|xvfb" | grep -v grep
        return 1
    fi
    pass "headroom is running (pid $HR_PID)"
}

NODE_NAMES=(headroom.engine headroom.mic headroom.stream)
for channel in "${CHANNELS[@]}"; do NODE_NAMES+=("headroom.ch.$channel"); done

# ------------------------------------------------------------------------------------------------
say "PipeWire"

pipewire >"$LOG_DIR/pipewire.log" 2>&1 &
PW_PID=$!
PIDS+=("$PW_PID")

if ! wait_for 30 pw-dump; then
    fail "PipeWire did not come up"
    tail -n 30 "$LOG_DIR/pipewire.log"
    exit 1
fi
pass "PipeWire is running"

# ------------------------------------------------------------------------------------------------
say "WirePlumber with the shipped Wave:3 drop-in"

DROPIN_SRC="$ROOT/packaging/51-headroom-wave.conf"
mkdir -p "$XDG_CONFIG_HOME/wireplumber/wireplumber.conf.d"
cp "$DROPIN_SRC" "$XDG_CONFIG_HOME/wireplumber/wireplumber.conf.d/51-headroom-wave.conf"

check "drop-in is structurally sound" "${ANALYSE[@]}" lint-conf "$DROPIN_SRC"

WIREPLUMBER_DEBUG=I wireplumber >"$LOG_DIR/wireplumber.log" 2>&1 &
WP_PID=$!
PIDS+=("$WP_PID")

check "WirePlumber is up" "${ANALYSE[@]}" ready --timeout 30

if kill -0 "$WP_PID" 2>/dev/null; then
    pass "WirePlumber is still alive with the drop-in installed"
else
    fail "WirePlumber exited"
fi

if grep -Eqi "failed to open '[^']*51-headroom-wave|skipping merge|invalid section name|section '[^']*' has no value|old WirePlumber 0\.4" "$LOG_DIR/wireplumber.log"; then
    fail "WirePlumber reported a configuration problem:"
    grep -Ei "51-headroom-wave|skipping merge|invalid section|has no value" "$LOG_DIR/wireplumber.log" | head -n 20
else
    pass "WirePlumber logged no configuration parse errors"
fi

if grep -Eq "opening fragment file: .*51-headroom-wave\.conf" "$LOG_DIR/wireplumber.log"; then
    pass "WirePlumber read 51-headroom-wave.conf"
else
    fail "WirePlumber did not read 51-headroom-wave.conf"
fi

# ------------------------------------------------------------------------------------------------
say "Fake Elgato Wave:3"

create_fake_devices() {
    pw-cli create-node adapter "{ factory.name=support.null-audio-sink node.name=$SINK node.description=\"Fake Wave:3 Headphones\" media.class=Audio/Sink audio.position=[FL FR] object.linger=true }" >>"$LOG_DIR/fake-sink.log" 2>&1
    pw-cli create-node adapter "{ factory.name=support.null-audio-sink node.name=$SRC node.description=\"Fake Wave:3 Mic\" media.class=Audio/Source/Virtual audio.position=[MONO] object.linger=true }" >>"$LOG_DIR/fake-source.log" 2>&1
}

create_fake_devices

check "fake Wave:3 sink and mono source exist" "${ANALYSE[@]}" nodes "$SINK" "$SRC" --timeout 15

"${ANALYSE[@]}" gen "$WORK/mic440.wav" --freq 440 --amp 0.5 --seconds 170 --channels 1 >/dev/null
"${ANALYSE[@]}" gen "$WORK/tone1k.wav" --freq 1000 --amp 0.25 --seconds 8 --channels 2 >/dev/null
"${ANALYSE[@]}" gen "$WORK/long1k.wav" --freq 1000 --amp 0.1 --seconds 170 --channels 2 >/dev/null

if start_feeder; then
    pass "440 Hz test tone is playing into the fake mic"
else
    fail "could not connect the tone generator to the fake mic"
    pw-link -o -i
fi

# ------------------------------------------------------------------------------------------------
say "Headroom"

start_headroom || exit 1

# (a) Headroom's nodes exist and the engine is linked to everything.
check "(a) Headroom nodes exist" "${ANALYSE[@]}" nodes "${NODE_NAMES[@]}" --timeout 45
check "(a) the engine is linked" "${ANALYSE[@]}" graph --sink "$SINK" --source "$SRC" --channels "${CHANNELS[@]}" --timeout 30
check "(a) the mic is linked before the headphones (capture first)" "${ANALYSE[@]}" order --sink "$SINK" --source "$SRC"

if wait_for 15 log_has "mic state [A-Za-z]* -> Ok"; then
    pass "(a) headroom reports the mic as working"
else
    fail "(a) headroom never reported the mic as working"
fi

if ! kill -0 "$HR_PID" 2>/dev/null; then
    fail "headroom died during start-up"
    tail -n 40 "$HR_LOG" 2>/dev/null
    exit 1
fi

# (b) A tone played into the Music channel comes out of the headphones at the expected level.
say "(b) Music channel to the headphones"
pw-record --target "$SINK" -P stream.capture.sink=true --rate 48000 --channels 2 --format s16 "$WORK/headphones.wav" \
    >"$LOG_DIR/pw-record-headphones.log" 2>&1 &
REC_PID=$!
sleep 1.5
pw-play --target headroom.ch.music "$WORK/tone1k.wav" >"$LOG_DIR/pw-play-music.log" 2>&1
sleep 0.5
kill -INT "$REC_PID" 2>/dev/null
wait "$REC_PID" 2>/dev/null
check "(b) 1 kHz tone reaches the headphones at the expected level" \
    "${ANALYSE[@]}" tone "$WORK/headphones.wav" --freq 1000 --expect-dbfs -15.05 --tol 3

# (c) The fake mic's tone comes out of Headroom Mic.
say "(c) mic to Headroom Mic"
pw-record --target headroom.mic --rate 48000 --channels 2 --format s16 "$WORK/headroom-mic.wav" \
    >"$LOG_DIR/pw-record-mic.log" 2>&1 &
REC_PID=$!
sleep 4
kill -INT "$REC_PID" 2>/dev/null
wait "$REC_PID" 2>/dev/null
check "(c) the 440 Hz mic tone comes out of Headroom Mic" \
    "${ANALYSE[@]}" tone "$WORK/headroom-mic.wav" --freq 440 --expect-dbfs -9.03 --tol 3

# (d) A Spotify-looking stream with no target ends up on the Music channel.
say "(d) app routing"
pw-play -P "application.process.binary=spotify application.name=Spotify node.name=hr-it-spotify media.name=ci-spotify" \
    "$WORK/long1k.wav" >"$LOG_DIR/pw-play-spotify.log" 2>&1 &
PIDS+=($!)
check "(d) the Spotify stream is linked to headroom.ch.music" \
    "${ANALYSE[@]}" linked hr-it-spotify headroom.ch.music --timeout 20

# (e) Headroom is the system default.
check "(e) default sink and source point at Headroom" "${ANALYSE[@]}" defaults --timeout 15

# Helpers for the default-device scenarios (e3) and (e2): what KDE, Steam or a plugged-in headset do to the "default" metadata.
set_default()   { pw-metadata -n default 0 "default.configured.audio.$1" "{ \"name\": \"$2\" }" Spa:String:JSON >/dev/null 2>&1; }   # sink|source <node.name>
clear_default() { pw-metadata -n default -d 0 "default.configured.audio.$1" >/dev/null 2>&1; }                                        # sink|source
now_ms()        { echo $(( $(date +%s%N) / 1000000 )); }
sink_reverts()  { log_count 'keep default: default output .*switching back to Headroom'; }

# A stream that has no target of its own follows the default: WirePlumber (not Headroom) links it to whatever the
# default output is and moves it when that changes. It starts on Headroom's fallback channel because Headroom is the default.
start_follower() {
    pw-play -P "node.name=hr-it-follower media.name=ci-follower" "$WORK/long1k.wav" >>"$LOG_DIR/pw-play-follower.log" 2>&1 &
    FOLLOWER_PID=$!
    PIDS+=("$FOLLOWER_PID")
    check "$1 a stream that follows the default starts on headroom.ch.system" "${ANALYSE[@]}" linked hr-it-follower headroom.ch.system --timeout 20
}

stop_follower() {
    [ -n "$FOLLOWER_PID" ] || return 0
    kill "$FOLLOWER_PID" 2>/dev/null
    wait "$FOLLOWER_PID" 2>/dev/null
    FOLLOWER_PID=""
}

# (e3) keepDefault (the "keep Headroom as the default" lock) is on unless the user turns it off, so this Headroom has it
#      on: whatever else becomes the default output or input (a headset that was plugged in, the KDE audio applet,
#      Steam's clear-default) is put back within a moment, and a stream that follows the default comes back with it.
say "(e3) keep Headroom as the default (on by default)"
STATE_FILE="$XDG_CONFIG_HOME/headroom/state.xml"
start_follower "(e3)"

T0="$(now_ms)"
set_default sink "$SINK"        # the fake Wave headphones become the default output, as if they had just been plugged in
if wait_for 5 log_has "keep default: default output was changed to $SINK, switching back to Headroom"; then
    ELAPSED=$(( $(now_ms) - T0 ))
    if [ "$ELAPSED" -le 2000 ]; then
        pass "(e3) the default output was switched back to Headroom after $ELAPSED ms"
    else
        fail "(e3) the default output was switched back, but only after $ELAPSED ms (expected within 2 s)"
    fi
else
    fail "(e3) headroom did not switch the default output back (no 'keep default' line in the log)"
fi
check "(e3) the default output is Headroom's again" "${ANALYSE[@]}" defaults --timeout 5
check "(e3) the stream that follows the default is back on headroom.ch.system" "${ANALYSE[@]}" linked hr-it-follower headroom.ch.system --timeout 15

set_default source "$SRC"       # the same for the default input
if wait_for 5 log_has "keep default: default input was changed to $SRC, switching back to Headroom"; then
    pass "(e3) the default input was switched back to Headroom Mic"
else
    fail "(e3) headroom did not switch the default input back (no 'keep default' line in the log)"
fi
check "(e3) the default input is Headroom Mic again" "${ANALYSE[@]}" defaults --timeout 5

clear_default sink              # Steam's wpctl clear-default
if wait_for 5 log_has "keep default: default output was cleared, switching back to Headroom"; then
    pass "(e3) a cleared default output was switched back to Headroom"
else
    fail "(e3) headroom did not put back a cleared default output"
fi
check "(e3) the default is Headroom's after the clear" "${ANALYSE[@]}" defaults --timeout 5

# A program that keeps changing the default is not fought forever: after 10 reverts within a minute headroom stops.
# (The reverts above count too, so it takes fewer than 11 changes.)
FIGHT=0
FIGHT_BEFORE=0
fight_step_done() { [ "$(sink_reverts)" -gt "$FIGHT_BEFORE" ] || log_has "keep default: default output was changed more than"; }
while [ "$FIGHT" -lt 14 ] && ! log_has "keep default: default output was changed more than"; do
    FIGHT=$((FIGHT + 1))
    FIGHT_BEFORE="$(sink_reverts)"
    set_default sink "$SINK"
    wait_for 5 fight_step_done
done
if log_has "keep default: default output was changed more than" && [ "$FIGHT" -le 11 ]; then
    pass "(e3) after $FIGHT changes in a row headroom gave up and said so in the log"
else
    fail "(e3) headroom did not give up on a program that keeps changing the default (after $FIGHT changes)"
fi
sleep 2
check "(e3) once it has given up, the other program's choice stays" "${ANALYSE[@]}" defaults --expect-sink "$SINK" --timeout 5
GAVE_UP_REVERTS="$(sink_reverts)"
clear_default sink
sleep 2
if [ "$(sink_reverts)" -eq "$GAVE_UP_REVERTS" ]; then
    pass "(e3) and nothing is switched back any more"
else
    fail "(e3) headroom went on switching the default back after it had given up"
fi
stop_follower

# (e2) With the lock off a deliberate choice must be respected, and only a cleared or vanished default is put back.
#      keepDefault is a setting: quit Headroom, switch it off in state.xml (which also shows that the setting was saved
#      and is read back) and start it again. Everything after this runs with the lock off.
say "(e2) restart Headroom with keepDefault switched off"
hr_gone() { ! kill -0 "$HR_PID" 2>/dev/null; }
timeout 30 "$BIN" --quit >>"$LOG_DIR/headroom-stdout.log" 2>&1
if wait_for 30 hr_gone; then
    pass "(e2) headroom quit and saved its settings"
    expect_clean_exit "(e2) and Headroom itself ended" "$HR_JOB"
else
    fail "(e2) headroom did not quit"
    kill -9 "$HR_PID" 2>/dev/null
fi
check "(e2) headroom.* nodes are gone" "${ANALYSE[@]}" gone --prefix headroom. --timeout 15
if grep -q 'keepDefault="1"' "$STATE_FILE" 2>/dev/null; then
    pass "(e2) state.xml records keepDefault=1, the default"
else
    fail "(e2) state.xml does not contain keepDefault=\"1\" (found: $(grep -o 'keepDefault="[^"]*"' "$STATE_FILE" 2>&1 | head -n 1))"
fi
sed -i 's/keepDefault="1"/keepDefault="0"/' "$STATE_FILE" 2>/dev/null
if grep -q 'keepDefault="0"' "$STATE_FILE" 2>/dev/null; then
    pass "(e2) keepDefault switched off in state.xml"
else
    fail "(e2) could not switch keepDefault off in $STATE_FILE"
fi
start_headroom || exit 1
check "(e2) Headroom nodes exist again" "${ANALYSE[@]}" nodes "${NODE_NAMES[@]}" --timeout 45
if log_has "Loaded settings"; then
    pass "(e2) headroom read state.xml back"
else
    fail "(e2) headroom did not load state.xml"
fi
check "(e2) Headroom took over the defaults at start, whatever they were" "${ANALYSE[@]}" defaults --timeout 15

# (e2) Steam-style "clear default" is undone, but a deliberate choice by the user is left alone.
say "(e2) default devices are kept, deliberate choices respected (keepDefault off)"
start_follower "(e2)"
pw-metadata -n default -d 0 default.configured.audio.sink >/dev/null 2>&1
pw-metadata -n default -d 0 default.configured.audio.source >/dev/null 2>&1
check "(e2) cleared defaults are put back" "${ANALYSE[@]}" defaults --timeout 15
pw-metadata -n default 0 default.configured.audio.sink "{ \"name\": \"$SINK\" }" Spa:String:JSON >/dev/null 2>&1
sleep 4
check "(e2) a sink the user picked on purpose is left alone" "${ANALYSE[@]}" defaults --expect-sink "$SINK" --timeout 5
check "(e2) a stream that follows the default goes along with it (WirePlumber moves it, Headroom does not have to)" \
    "${ANALYSE[@]}" linked hr-it-follower "$SINK" --timeout 10
if log_has "keep default:"; then
    fail "(e2) with the lock off headroom switched a default back: $(log_since | grep -m 1 'keep default:')"
else
    pass "(e2) with the lock off headroom never switched a default back"
fi
pw-metadata -n default -d 0 default.configured.audio.sink >/dev/null 2>&1
check "(e2) and Headroom takes over again once it is cleared" "${ANALYSE[@]}" defaults --timeout 15
check "(e2) the stream that follows the default comes back with it" "${ANALYSE[@]}" linked hr-it-follower headroom.ch.system --timeout 15
pw-metadata -n default 0 default.configured.audio.sink '{ "name": "alsa_output.no-such-device" }' Spa:String:JSON >/dev/null 2>&1
check "(e2) a default that names a device which does not exist is replaced" "${ANALYSE[@]}" defaults --timeout 15
stop_follower

# (g) The mic goes quiet after it was heard: muted on the device. That must never trigger a recovery.
say "(g) mic muted on the device"
ATTEMPTS_BEFORE="$(log_count 'no signal since capture started')"
stop_feeder
if wait_for 20 log_has "mic state Ok -> MutedOnDevice"; then
    pass "(g) a mic that went quiet is reported as muted on the device"
else
    fail "(g) headroom did not notice the mic going quiet"
fi
sleep 4
if [ "$(log_count 'no signal since capture started')" -eq "$ATTEMPTS_BEFORE" ] && ! log_has "MutedOnDevice -> Recovering"; then
    pass "(g) no automatic recovery for a mic muted on the device"
else
    fail "(g) headroom tried to recover a mic that was muted on the device"
fi
if start_feeder && wait_for 20 log_has "mic state MutedOnDevice -> Ok"; then
    pass "(g) the banner clears by itself when the mic comes back"
else
    fail "(g) headroom did not notice the mic coming back"
fi

# (f) If headroom is killed hard, its nodes vanish.
say "(f) crash cleanup"
kill -9 "$HR_PID" 2>/dev/null
check "(f) headroom.* nodes are gone after kill -9" "${ANALYSE[@]}" gone --prefix headroom. --timeout 15

# (i) A mic that is silent from the very start (the Wave:3 firmware bug, or a muted device):
#     two automatic recovery attempts, then "tap the top of the mic". Headphones must keep working.
say "(i) silent mic from the start"
stop_feeder
if start_headroom; then
    check "(i) Headroom nodes exist again" "${ANALYSE[@]}" nodes "${NODE_NAMES[@]}" --timeout 45

    if wait_for 60 log_has "mic state Recovering -> SilentFromStart"; then
        pass "(i) after recovering it tells the user to unmute the mic"
    else
        fail "(i) headroom never reached SilentFromStart"
    fi

    ATTEMPTS="$(log_count 'no signal since capture started')"
    if [ "$ATTEMPTS" -eq 2 ]; then
        pass "(i) exactly two automatic recovery attempts"
    else
        fail "(i) expected 2 automatic recovery attempts, saw $ATTEMPTS"
    fi

    check "(i) the headphones and the mic stay linked" \
        "${ANALYSE[@]}" graph --sink "$SINK" --source "$SRC" --channels "${CHANNELS[@]}" --timeout 20

    if start_feeder && wait_for 20 log_has "mic state SilentFromStart -> Ok"; then
        pass "(i) the mic is picked up as soon as it has a signal"
    else
        fail "(i) headroom did not recover once the mic had a signal"
    fi
fi

# (k) A forced mic recovery must leave Headroom Mic, the other Headroom nodes and whatever listens to
#     Headroom Mic completely alone: only the Wave-side links change. It also runs the user's hook.
say "(k) recovery keeps Headroom Mic and its listeners intact"
kill -9 "$HR_PID" 2>/dev/null
"${ANALYSE[@]}" gone --prefix headroom. --timeout 15 >/dev/null
[ -n "$FEEDER_PID" ] || start_feeder

mkdir -p "$XDG_CONFIG_HOME/headroom/hooks"
export HOOK_OUT="$WORK/hook.out"
cat >"$XDG_CONFIG_HOME/headroom/hooks/on-mic-recovered" <<'HOOK'
#!/bin/sh
echo "$HEADROOM_MIC_STATE" >> "$HOOK_OUT"
HOOK
chmod +x "$XDG_CONFIG_HOME/headroom/hooks/on-mic-recovered"

export HEADROOM_TEST_FORCE_RECOVERY_AFTER_MS=25000
KEEPER_STARTED=0
if start_headroom; then
    unset HEADROOM_TEST_FORCE_RECOVERY_AFTER_MS
    check "(k) Headroom nodes exist" "${ANALYSE[@]}" nodes "${NODE_NAMES[@]}" --timeout 45
    check "(k) the engine is linked" "${ANALYSE[@]}" graph --sink "$SINK" --source "$SRC" --channels "${CHANNELS[@]}" --timeout 30

    # Another app (think: a voice assistant) records from Headroom Mic the whole time.
    pw-record -P "node.name=hr-it-keeper" --target headroom.mic --rate 48000 --channels 2 --format s16 "$WORK/keeper.wav" \
        >"$LOG_DIR/pw-record-keeper.log" 2>&1 &
    KEEPER_PID=$!
    PIDS+=("$KEEPER_PID")
    check "(k) the listener is linked to Headroom Mic" "${ANALYSE[@]}" linked headroom.mic hr-it-keeper --timeout 15
    "${ANALYSE[@]}" snapshot --keeper hr-it-keeper --source "$SRC" --sink "$SINK" >"$WORK/snap-before.json"

    if wait_for 60 log_has "WaveGuard: recovery finished (mic ok)"; then
        pass "(k) headroom recovered the mic and says so in the log"
    else
        fail "(k) no 'recovery finished (mic ok)' line in the log"
    fi
    sleep 3
    "${ANALYSE[@]}" snapshot --keeper hr-it-keeper --source "$SRC" --sink "$SINK" >"$WORK/snap-after.json"
    check "(k) Headroom's nodes and the listener were untouched, only the Wave links were recreated" \
        "${ANALYSE[@]}" compare "$WORK/snap-before.json" "$WORK/snap-after.json"

    if kill -0 "$KEEPER_PID" 2>/dev/null; then
        pass "(k) the recording app is still running"
    else
        fail "(k) the recording app lost its stream"
    fi
    kill -INT "$KEEPER_PID" 2>/dev/null
    wait "$KEEPER_PID" 2>/dev/null
    check "(k) and it still hears the mic after the recovery" \
        "${ANALYSE[@]}" tone "$WORK/keeper.wav" --freq 440 --expect-dbfs -9.03 --tol 3 --tail 2

    if wait_for 10 grep -qx ok "$HOOK_OUT"; then
        pass "(k) the on-mic-recovered hook ran with HEADROOM_MIC_STATE=ok"
    else
        fail "(k) the on-mic-recovered hook did not run (hook output: $(cat "$HOOK_OUT" 2>/dev/null))"
    fi
fi
unset HEADROOM_TEST_FORCE_RECOVERY_AFTER_MS

# (j) PipeWire itself restarts while Headroom is running: Headroom must reconnect and rebuild everything.
say "(j) PipeWire restarts underneath Headroom"
kill -9 "$PW_PID" "$WP_PID" 2>/dev/null
wait "$PW_PID" "$WP_PID" 2>/dev/null
FEEDER_PID=""
if wait_for 20 log_has "lost the connection to PipeWire"; then
    pass "(j) headroom noticed that PipeWire went away"
else
    fail "(j) headroom did not notice that PipeWire went away"
fi
if kill -0 "$HR_PID" 2>/dev/null; then
    pass "(j) headroom survived the loss of PipeWire"
else
    fail "(j) headroom crashed when PipeWire went away"
    tail -n 30 "$HR_LOG" 2>/dev/null
fi

pipewire >>"$LOG_DIR/pipewire.log" 2>&1 &
PW_PID=$!
PIDS+=("$PW_PID")
if wait_for 30 pw-dump; then
    WIREPLUMBER_DEBUG=I wireplumber >>"$LOG_DIR/wireplumber.log" 2>&1 &
    WP_PID=$!
    PIDS+=("$WP_PID")
    check "(j) WirePlumber is up again" "${ANALYSE[@]}" ready --timeout 30
    create_fake_devices
    start_feeder
else
    fail "(j) PipeWire did not restart"
fi

reconnected() { [ "$(log_count 'connected to PipeWire')" -ge 2 ]; }
if wait_for 40 reconnected; then
    pass "(j) headroom reconnected"
else
    fail "(j) headroom did not reconnect"
fi
check "(j) Headroom nodes are back" "${ANALYSE[@]}" nodes "${NODE_NAMES[@]}" --timeout 45
check "(j) the engine is linked again" "${ANALYSE[@]}" graph --sink "$SINK" --source "$SRC" --channels "${CHANNELS[@]}" --timeout 30
check "(j) Headroom is the default again" "${ANALYSE[@]}" defaults --timeout 20
if kill -0 "$HR_PID" 2>/dev/null; then
    pass "(j) headroom is still the same process"
else
    fail "(j) headroom died"
fi

# (l) The safety net in the PipeWire backend: a second Headroom that somehow runs next to the first must find its
#     headroom.engine node, create nothing and say so. Single-instance handling stops that in real life, so this
#     one is given an instance name of its own, and its own log so that it cannot confuse the checks above.
say "(l) a second Headroom finds the first one and creates nothing"
mkdir -p "$WORK/state2"
SECOND_LOG="$WORK/state2/headroom/headroom.log"
xvfb-run -a -s "-screen 0 1280x800x24" env XDG_STATE_HOME="$WORK/state2" HEADROOM_INSTANCE=second HEADROOM_TEST_NO_FX=1     "$BIN" --background >>"$LOG_DIR/headroom-second-stdout.log" 2>&1 &
SECOND_JOB=$!
PIDS+=("$SECOND_JOB")
if wait_for 30 grep -q "Headroom is already running" "$SECOND_LOG"; then
    pass "(l) the second Headroom noticed the first one and says so"
else
    fail "(l) the second Headroom did not notice the first one"
    tail -n 30 "$LOG_DIR/headroom-second-stdout.log" 2>/dev/null
fi
sleep 2
ENGINES="$(pw-dump 2>/dev/null | grep -c '"node.name": "headroom.engine"')"
if [ "$ENGINES" -eq 1 ]; then
    pass "(l) there is still exactly one headroom.engine node"
else
    fail "(l) expected one headroom.engine node, found $ENGINES"
fi
check "(l) the first Headroom's graph is intact" "${ANALYSE[@]}" graph --sink "$SINK" --source "$SRC" --channels "${CHANNELS[@]}" --timeout 30
if kill -0 "$HR_PID" 2>/dev/null; then
    pass "(l) the first Headroom is still the same process"
else
    fail "(l) the first Headroom died"
fi
# --quit with the second one's instance name ends it cleanly (and shows that --quit works on Linux too).
if HEADROOM_INSTANCE=second timeout 30 "$BIN" --quit >>"$LOG_DIR/headroom-second-stdout.log" 2>&1; then
    pass "(l) headroom --quit ended the second Headroom"
else
    fail "(l) headroom --quit failed"
fi
expect_clean_exit "(l) and the second Headroom itself ended" "$SECOND_JOB"
if kill -0 "$HR_PID" 2>/dev/null; then
    pass "(l) and left the first one running"
else
    fail "(l) --quit with the second instance's name ended the first Headroom"
fi

# (m) The first Headroom has been through a PipeWire restart by now. Quitting it must still be orderly and end with status 0.
say "(m) --quit after everything above"
if timeout 30 "$BIN" --quit >>"$LOG_DIR/headroom-stdout.log" 2>&1; then
    pass "(m) headroom --quit returned 0"
else
    fail "(m) headroom --quit failed"
fi
expect_clean_exit "(m) and Headroom itself ended" "$HR_JOB"

# (n) SIGTERM is an orderly quit too. `systemctl --user stop` (logout, shutdown, install.sh) sends it and JUCE does not handle
#     it, so the last change used to be lost when it came within the second before the autosave. A test hook stores a routing
#     rule a few seconds after start-up; SIGTERM follows within a tenth of a second of the log line that says so, which is
#     long before the 1 s autosave. The rule must be in state.xml, the nodes gone, and Headroom's own status 0.
say "(n) SIGTERM saves the state and quits cleanly"
export HEADROOM_TEST_STORE_RULE_AFTER_MS=8000
if start_headroom; then
    unset HEADROOM_TEST_STORE_RULE_AFTER_MS
    check "(n) Headroom nodes exist" "${ANALYSE[@]}" nodes "${NODE_NAMES[@]}" --timeout 45

    STORED=0
    for _ in $(seq 1 400); do
        if log_has "test hook: stored the routing rule hr-it-sigterm"; then
            STORED=1
            break
        fi
        sleep 0.05
    done

    if [ "$STORED" -eq 1 ]; then
        PRE_SAVED=0
        if grep -q 'pattern="hr-it-sigterm"' "$STATE_FILE" 2>/dev/null; then PRE_SAVED=1; fi
        kill -TERM "$HR_PID" 2>/dev/null
        expect_clean_exit "(n) Headroom ended after SIGTERM" "$HR_JOB"

        if grep -q 'pattern="hr-it-sigterm"' "$STATE_FILE" 2>/dev/null; then
            pass "(n) the routing rule stored a moment before SIGTERM is in state.xml"
        else
            fail "(n) the routing rule is not in state.xml: SIGTERM did not save the state"
        fi
        if [ "$PRE_SAVED" -eq 1 ]; then
            note "(n) the autosave had already written the rule when SIGTERM was sent, so this run did not prove the save"
        fi
        if log_has "Got SIGTERM: saving and quitting"; then
            pass "(n) the log says the quit came from SIGTERM"
        else
            fail "(n) no 'Got SIGTERM' line in the log"
        fi
        check "(n) headroom.* nodes are gone" "${ANALYSE[@]}" gone --prefix headroom. --timeout 15
    else
        fail "(n) the test hook never stored its routing rule"
        kill -9 "$HR_PID" 2>/dev/null
    fi
fi
unset HEADROOM_TEST_STORE_RULE_AFTER_MS

# ------------------------------------------------------------------------------------------------
# (o) and (p): other devices than the Wave:3. The fakes have no USB device behind them, so the model is
#     recognised by the udev-style node name, which is how the table knows a Wave:1 ("Elgato_Systems_Elgato_Wave_1_").

node_id() {     # node_id <node.name>: the PipeWire id of a node, from pw-cli
    pw-cli ls Node 2>/dev/null | awk -v name="$1" '/^[ \t]*id [0-9]+,/ { id = $2; sub(",", "", id) } index($0, "node.name = \"" name "\"") { print id; exit }'
}

destroy_fake_devices() {    # destroys the fake pair named by $SINK and $SRC
    local id
    for name in "$SINK" "$SRC"; do
        id="$(node_id "$name")"
        [ -n "$id" ] && pw-cli destroy "$id" >/dev/null 2>&1
    done
}

quit_headroom() {
    timeout 30 "$BIN" --quit >>"$LOG_DIR/headroom-stdout.log" 2>&1
    wait_for 30 hr_gone || kill -9 "$HR_PID" 2>/dev/null
    "${ANALYSE[@]}" gone --prefix headroom. --timeout 15 >/dev/null
}

# (o) A Wave:1 instead of the Wave:3: recognised by name, capture before playback, and a mic that is silent from the start
#     gets the same two recovery attempts.
say "(o) another Elgato model: a fake Wave:1"
stop_feeder
destroy_fake_devices
check "(o) the fake Wave:3 sink is gone" "${ANALYSE[@]}" gone --prefix "alsa_output.usb-Elgato_Systems_Elgato_Wave_3_TEST" --timeout 10
check "(o) the fake Wave:3 mic is gone" "${ANALYSE[@]}" gone --prefix "alsa_input.usb-Elgato_Systems_Elgato_Wave_3_TEST" --timeout 10

SINK="alsa_output.usb-Elgato_Systems_Elgato_Wave_1_TEST-00.analog-stereo"
SRC="alsa_input.usb-Elgato_Systems_Elgato_Wave_1_TEST-00.mono-fallback"
create_fake_devices
check "(o) fake Wave:1 sink and mono source exist" "${ANALYSE[@]}" nodes "$SINK" "$SRC" --timeout 15

if start_feeder; then
    pass "(o) the test tone is playing into the fake Wave:1"
else
    fail "(o) could not connect the tone generator to the fake Wave:1"
fi

if start_headroom; then
    check "(o) Headroom nodes exist" "${ANALYSE[@]}" nodes "${NODE_NAMES[@]}" --timeout 45
    check "(o) the engine is linked to the Wave:1" "${ANALYSE[@]}" graph --sink "$SINK" --source "$SRC" --channels "${CHANNELS[@]}" --timeout 30
    check "(o) the Wave:1's mic is linked before its headphones (capture first)" "${ANALYSE[@]}" order --sink "$SINK" --source "$SRC"

    if wait_for 15 log_has "WaveGuard: input is Elgato Wave:1 (shared clock: reported)"; then
        pass "(o) Headroom knows it is an Elgato Wave:1 and puts WaveGuard on it"
    else
        fail "(o) the log does not say that the input is an Elgato Wave:1 ($(log_since | grep -m 1 'WaveGuard:'))"
    fi

    if wait_for 15 log_has "mic state [A-Za-z]* -> Ok"; then
        pass "(o) headroom reports the Wave:1's mic as working"
    else
        fail "(o) headroom never reported the Wave:1's mic as working"
    fi

    quit_headroom
fi

stop_feeder
if start_headroom; then
    if wait_for 60 log_has "mic state Recovering -> SilentFromStart"; then
        pass "(o) a Wave:1 that is silent from the start is recovered, then reported"
    else
        fail "(o) headroom never reached SilentFromStart for the silent Wave:1"
    fi

    ATTEMPTS="$(log_count 'no signal since capture started')"
    if [ "$ATTEMPTS" -eq 2 ]; then
        pass "(o) exactly two automatic recovery attempts for the Wave:1"
    else
        fail "(o) expected 2 automatic recovery attempts for the Wave:1, saw $ATTEMPTS"
    fi

    quit_headroom
fi

# (p) An ordinary stereo USB mic (nothing Elgato about it): no capture-first ordering, no recovery, no profile change. A mic
#     that is silent is reported, once, and the headphones and the mic stay linked.
say "(p) a generic USB mic gets none of WaveGuard's treatment"
destroy_fake_devices
check "(p) the fake Wave:1 sink is gone" "${ANALYSE[@]}" gone --prefix "alsa_output.usb-Elgato_Systems_Elgato_Wave_1_TEST" --timeout 10
check "(p) the fake Wave:1 mic is gone" "${ANALYSE[@]}" gone --prefix "alsa_input.usb-Elgato_Systems_Elgato_Wave_1_TEST" --timeout 10

SINK="alsa_output.usb-Blue_Microphones_Yeti_Stereo_Microphone_TEST-00.analog-stereo"
SRC="alsa_input.usb-Blue_Microphones_Yeti_Stereo_Microphone_TEST-00.analog-stereo"
pw-cli create-node adapter "{ factory.name=support.null-audio-sink node.name=$SINK node.description=\"Fake Yeti Headphones\" media.class=Audio/Sink audio.position=[FL FR] object.linger=true }" >>"$LOG_DIR/fake-sink.log" 2>&1
pw-cli create-node adapter "{ factory.name=support.null-audio-sink node.name=$SRC node.description=\"Fake Yeti Mic\" media.class=Audio/Source/Virtual audio.position=[FL FR] object.linger=true }" >>"$LOG_DIR/fake-source.log" 2>&1
check "(p) fake stereo mic and headphones exist" "${ANALYSE[@]}" nodes "$SINK" "$SRC" --timeout 15

if start_headroom; then
    check "(p) Headroom nodes exist" "${ANALYSE[@]}" nodes "${NODE_NAMES[@]}" --timeout 45
    check "(p) the engine is linked to the stereo mic and its headphones" "${ANALYSE[@]}" graph --sink "$SINK" --source "$SRC" --channels "${CHANNELS[@]}" --timeout 30

    if wait_for 15 log_has "WaveGuard: input is .* (not an Elgato Wave)"; then
        pass "(p) the log says the stereo mic is not an Elgato Wave"
    else
        fail "(p) the log does not say that the stereo mic is not an Elgato Wave"
    fi

    if wait_for 30 log_has "mic state Starting -> SilentFromStart"; then
        pass "(p) a silent mic is reported as silent, straight away"
    else
        fail "(p) headroom never reported the silent stereo mic"
    fi

    sleep 4
    if [ "$(log_count 'no signal since capture started')" -eq 0 ] && ! log_has "-> Recovering" && ! log_has "set profile"; then
        pass "(p) no recovery attempt and no profile change for a mic that is not an Elgato Wave"
    else
        fail "(p) headroom treated an ordinary mic like a Wave: $(log_since | grep -m 3 -E 'no signal since capture started|Recovering|set profile')"
    fi

    check "(p) the headphones and the mic stay linked" "${ANALYSE[@]}" graph --sink "$SINK" --source "$SRC" --channels "${CHANNELS[@]}" --timeout 20

    quit_headroom
fi

# ------------------------------------------------------------------------------------------------
# (q), (r) and (s): Wave Link 3 style routing: a second mic as an input channel of its own, and the mixes playing to more
#     than one output. The main mic and the headphones are the fake Wave:3 again; the second mic (660 Hz) and the desk speakers are
#     other devices. The model is edited in state.xml between runs (analyse.py state-inputs / state-outputs), and a test hook
#     (HEADROOM_TEST_OUTPUTS) changes what the mixes play to while Headroom runs, which is how an output is removed live.

MIC2="alsa_input.usb-Fake_Second_Mic_TEST-00.mono-fallback"
SINK2="alsa_output.usb-Fake_Desk_Speakers_TEST-00.analog-stereo"
W1SINK="alsa_output.usb-Elgato_Systems_Elgato_Wave_1_TEST-00.analog-stereo"
W1SRC="alsa_input.usb-Elgato_Systems_Elgato_Wave_1_TEST-00.mono-fallback"
FEEDER2_PID=""

create_mic2() {
    pw-cli create-node adapter "{ factory.name=support.null-audio-sink node.name=$MIC2 node.description=\"Fake Second Mic\" media.class=Audio/Source/Virtual audio.position=[MONO] object.linger=true }" >>"$LOG_DIR/fake-mic2.log" 2>&1
}

create_sink2() {
    pw-cli create-node adapter "{ factory.name=support.null-audio-sink node.name=$SINK2 node.description=\"Fake Desk Speakers\" media.class=Audio/Sink audio.position=[FL FR] object.linger=true }" >>"$LOG_DIR/fake-sink2.log" 2>&1
}

destroy_node() {    # destroy_node <node.name>
    local id
    id="$(node_id "$1")"
    [ -n "$id" ] && pw-cli destroy "$id" >/dev/null 2>&1
}

link_feeder2() {
    local outputs inputs out in
    outputs="$(pw-link -o 2>/dev/null)" || return 1
    inputs="$(pw-link -i 2>/dev/null)" || return 1
    out="$(grep -m1 '^hr-it-mic2-feeder:' <<<"$outputs")" || return 1
    in="$(grep -m1 "^$MIC2:input" <<<"$inputs")" || return 1
    pw-link "$out" "$in"
}

start_feeder2() {
    pw-play -P "node.name=hr-it-mic2-feeder node.autoconnect=false" "$WORK/mic660.wav" >>"$LOG_DIR/mic2-feeder.log" 2>&1 &
    FEEDER2_PID=$!
    PIDS+=("$FEEDER2_PID")
    wait_for 20 link_feeder2
}

stop_feeder2() {
    [ -n "$FEEDER2_PID" ] || return 0
    kill "$FEEDER2_PID" 2>/dev/null
    wait "$FEEDER2_PID" 2>/dev/null
    FEEDER2_PID=""
}

# record_for <seconds> <wav> <target> [<wav> <target> ...]: records one or several sinks / sources at the same time.
record_for() {
    local seconds="$1" pids=() props=() wav target
    shift
    while [ "$#" -ge 2 ]; do
        wav="$1"
        target="$2"
        shift 2
        props=(-P stream.capture.sink=true)     # a sink is recorded through its monitor; Headroom's own sources (headroom.*) as they are
        case "$target" in headroom.*) props=() ;; esac
        pw-record --target "$target" "${props[@]}" --rate 48000 --channels 2 --format s16 "$WORK/$wav" >>"$LOG_DIR/pw-record-$wav.log" 2>&1 &
        pids+=("$!")
    done
    sleep "$seconds"
    kill -INT "${pids[@]}" 2>/dev/null
    wait "${pids[@]}" 2>/dev/null
}

"${ANALYSE[@]}" gen "$WORK/mic660.wav" --freq 660 --amp 0.25 --seconds 170 --channels 1 >/dev/null

# (q) The second mic is an input channel: its sound is in the Headphones mix and the Stream mix at its own faders, the main mic is
#     as it was (it alone feeds Headroom Mic, and it is not in the headphones), and the channel follows the device coming and going.
say "(q) a second mic as an input channel"
destroy_fake_devices
check "(q) the fake stereo mic from (p) is gone" "${ANALYSE[@]}" gone --prefix "alsa_input.usb-Blue_Microphones_Yeti" --timeout 10
SINK="alsa_output.usb-Elgato_Systems_Elgato_Wave_3_TEST-00.analog-stereo"
SRC="alsa_input.usb-Elgato_Systems_Elgato_Wave_3_TEST-00.mono-fallback"
create_fake_devices
create_mic2
check "(q) fake Wave:3, second mic exist" "${ANALYSE[@]}" nodes "$SINK" "$SRC" "$MIC2" --timeout 15

if start_feeder && start_feeder2; then
    pass "(q) 440 Hz is playing into the main mic and 660 Hz into the second mic"
else
    fail "(q) could not connect the tone generators"
fi

"${ANALYSE[@]}" state-inputs "$STATE_FILE" add --id mic2 --name "Second mic" --device "$MIC2" --channels mono >/dev/null
"${ANALYSE[@]}" state-outputs "$STATE_FILE" --headphones auto --stream "" >/dev/null

if start_headroom; then
    check "(q) Headroom nodes exist" "${ANALYSE[@]}" nodes "${NODE_NAMES[@]}" --timeout 45
    check "(q) the main mic and the headphones are linked as before" "${ANALYSE[@]}" graph --sink "$SINK" --source "$SRC" --channels "${CHANNELS[@]}" --timeout 30
    check "(q) the second mic is linked to the engine" "${ANALYSE[@]}" linked "$MIC2" headroom.engine --timeout 20
    check "(q) a mono device feeds both sides of its slot" "${ANALYSE[@]}" inports --source "$MIC2" --ports in0_L in0_R --timeout 20

    record_for 4 q-headphones.wav "$SINK"
    check "(q) the second mic's 660 Hz reaches the headphones, and the main mic (440 Hz) does not" \
        "${ANALYSE[@]}" tone "$WORK/q-headphones.wav" --freq 660 --expect-dbfs -15.05 --tol 3

    record_for 4 q-stream.wav headroom.stream
    check "(q) the Stream mix has the second mic (660 Hz)" "${ANALYSE[@]}" tone "$WORK/q-stream.wav" --freq 660 --expect-dbfs -15.05 --tol 3 --min-fraction 0.1
    check "(q) and the main mic as always (440 Hz)" "${ANALYSE[@]}" tone "$WORK/q-stream.wav" --freq 440 --expect-dbfs -9.03 --tol 3 --min-fraction 0.1

    record_for 4 q-mic.wav headroom.mic
    check "(q) Headroom Mic is still the main mic alone" "${ANALYSE[@]}" tone "$WORK/q-mic.wav" --freq 440 --expect-dbfs -9.03 --tol 3

    # The second mic is unplugged: the channel goes quiet and Headroom carries on; plugged in again it is linked again.
    stop_feeder2
    destroy_node "$MIC2"
    check "(q) the second mic is gone from the graph" "${ANALYSE[@]}" gone --prefix "alsa_input.usb-Fake_Second_Mic" --timeout 10
    sleep 1
    record_for 3 q-unplugged.wav "$SINK"
    check "(q) without the mic its channel is silent" "${ANALYSE[@]}" silent "$WORK/q-unplugged.wav"
    if kill -0 "$HR_PID" 2>/dev/null; then
        pass "(q) Headroom is still running"
    else
        fail "(q) Headroom died when the second mic was unplugged"
    fi
    check "(q) the main mic and the headphones are still linked" "${ANALYSE[@]}" graph --sink "$SINK" --source "$SRC" --channels "${CHANNELS[@]}" --timeout 10

    create_mic2
    start_feeder2
    check "(q) plugged in again, the second mic is linked again" "${ANALYSE[@]}" inports --source "$MIC2" --ports in0_L in0_R --timeout 20
    record_for 4 q-replugged.wav "$SINK"
    check "(q) and its 660 Hz is back in the headphones" "${ANALYSE[@]}" tone "$WORK/q-replugged.wav" --freq 660 --expect-dbfs -15.05 --tol 3

    quit_headroom
fi
stop_feeder2

# (r) The Headphones mix plays to two devices at once; taking one out of the list stops it there and nowhere else; the Stream mix can
#     play to a device too. The mixes are switched by the test hook while Headroom runs: both fake sinks, then one, then the Stream mix
#     to the speakers.
say "(r) the Headphones mix plays to two outputs at once"
create_sink2
check "(r) the fake desk speakers exist" "${ANALYSE[@]}" nodes "$SINK2" --timeout 15
"${ANALYSE[@]}" state-inputs "$STATE_FILE" clear >/dev/null
"${ANALYSE[@]}" state-outputs "$STATE_FILE" --headphones auto --stream "" >/dev/null

export HEADROOM_TEST_OUTPUTS="16000,$SINK|$SINK2,;34000,$SINK,;52000,$SINK,$SINK2"
if start_headroom; then
    unset HEADROOM_TEST_OUTPUTS
    check "(r) Headroom nodes exist" "${ANALYSE[@]}" nodes "${NODE_NAMES[@]}" --timeout 45
    check "(r) the engine is linked to the headphones as before" "${ANALYSE[@]}" graph --sink "$SINK" --source "$SRC" --channels "${CHANNELS[@]}" --timeout 30
    check "(r) the desk speakers get nothing while only one output is chosen" "${ANALYSE[@]}" unlinked headroom.engine "$SINK2" --timeout 2

    pw-play --target headroom.ch.music "$WORK/long1k.wav" >"$LOG_DIR/pw-play-r-music.log" 2>&1 &
    MUSIC_PID=$!
    PIDS+=("$MUSIC_PID")

    # Both outputs: the same mix on both at the same time.
    if wait_for 40 log_has "test hook 1: the Headphones mix plays to $SINK|$SINK2"; then
        pass "(r) the Headphones mix was switched to both outputs"
    else
        fail "(r) the test hook never switched the Headphones mix to both outputs"
    fi
    check "(r) the engine is linked to the headphones" "${ANALYSE[@]}" linked headroom.engine "$SINK" --timeout 20
    check "(r) and to the desk speakers" "${ANALYSE[@]}" linked headroom.engine "$SINK2" --timeout 20
    record_for 3.5 r-a1.wav "$SINK" r-b1.wav "$SINK2"
    check "(r) the Music tone plays on the first output" "${ANALYSE[@]}" tone "$WORK/r-a1.wav" --freq 1000 --expect-dbfs -23.01 --tol 3
    check "(r) and on the second at the same time" "${ANALYSE[@]}" tone "$WORK/r-b1.wav" --freq 1000 --expect-dbfs -23.01 --tol 3

    # One output taken out: it stops there, the other goes on.
    if wait_for 40 log_has "test hook 2: the Headphones mix plays to $SINK"; then
        pass "(r) the speakers were taken out of the Headphones mix"
    else
        fail "(r) the test hook never took the speakers out of the list"
    fi
    check "(r) the desk speakers are not linked any more" "${ANALYSE[@]}" unlinked headroom.engine "$SINK2" --timeout 20
    check "(r) the first output still is" "${ANALYSE[@]}" linked headroom.engine "$SINK" --timeout 10
    record_for 3.5 r-a2.wav "$SINK" r-b2.wav "$SINK2"
    check "(r) the first output still plays" "${ANALYSE[@]}" tone "$WORK/r-a2.wav" --freq 1000 --expect-dbfs -23.01 --tol 3
    check "(r) the removed output gets nothing" "${ANALYSE[@]}" silent "$WORK/r-b2.wav"

    # The Stream mix to the speakers: the mic and the Music tone, not just what the headphones get.
    if wait_for 40 log_has "test hook 3: the Headphones mix plays to $SINK, the Stream mix also to $SINK2"; then
        pass "(r) the Stream mix was switched to the speakers"
    else
        fail "(r) the test hook never switched the Stream mix to the speakers"
    fi
    check "(r) the speakers are linked to the Stream mix" "${ANALYSE[@]}" linked headroom.engine "$SINK2" --timeout 20
    record_for 3.5 r-a3.wav "$SINK" r-b3.wav "$SINK2"
    check "(r) the speakers play the Stream mix: the main mic's 440 Hz is in it" "${ANALYSE[@]}" tone "$WORK/r-b3.wav" --freq 440 --expect-dbfs -9.03 --tol 3 --min-fraction 0.3
    check "(r) the headphones are still the Headphones mix: the Music tone, no mic" "${ANALYSE[@]}" tone "$WORK/r-a3.wav" --freq 1000 --expect-dbfs -23.01 --tol 3

    if kill -0 "$HR_PID" 2>/dev/null; then
        pass "(r) Headroom is still running"
    else
        fail "(r) Headroom died while the outputs changed"
    fi
    kill "$MUSIC_PID" 2>/dev/null
    quit_headroom
fi
unset HEADROOM_TEST_OUTPUTS

# (s) An Elgato device as an input channel that also plays a mix: its capture is linked before its playback, and nothing of it
#     is cycled when it is silent: recovery and profile changes are for the main mic only.
say "(s) an Elgato Wave as an input channel that is also an output"
pw-cli create-node adapter "{ factory.name=support.null-audio-sink node.name=$W1SINK node.description=\"Fake Wave:1 Headphones\" media.class=Audio/Sink audio.position=[FL FR] object.linger=true }" >>"$LOG_DIR/fake-sink.log" 2>&1
pw-cli create-node adapter "{ factory.name=support.null-audio-sink node.name=$W1SRC node.description=\"Fake Wave:1 Mic\" media.class=Audio/Source/Virtual audio.position=[MONO] object.linger=true }" >>"$LOG_DIR/fake-source.log" 2>&1
check "(s) fake Wave:1 sink and mic exist" "${ANALYSE[@]}" nodes "$W1SINK" "$W1SRC" --timeout 15
"${ANALYSE[@]}" state-inputs "$STATE_FILE" add --id wave1 --name "Wave 1" --device "$W1SRC" --channels mono >/dev/null
"${ANALYSE[@]}" state-outputs "$STATE_FILE" --headphones "$SINK|$W1SINK" --stream "" >/dev/null

if start_headroom; then
    check "(s) Headroom nodes exist" "${ANALYSE[@]}" nodes "${NODE_NAMES[@]}" --timeout 45
    check "(s) the Wave:1 is linked as an input channel" "${ANALYSE[@]}" inports --source "$W1SRC" --ports in0_L in0_R --timeout 30
    check "(s) and the Headphones mix plays to it" "${ANALYSE[@]}" linked headroom.engine "$W1SINK" --timeout 30
    check "(s) and to the Wave:3 headphones" "${ANALYSE[@]}" linked headroom.engine "$SINK" --timeout 10
    check "(s) the Wave:1's capture was linked before its headphones (capture first)" "${ANALYSE[@]}" order --sink "$W1SINK" --source "$W1SRC"
    check "(s) the Wave:3's mic is linked before its headphones, as ever" "${ANALYSE[@]}" order --sink "$SINK" --source "$SRC"
    sleep 4
    if [ "$(log_count 'no signal since capture started')" -eq 0 ] && ! log_has "-> Recovering" && ! log_has "set profile"; then
        pass "(s) a silent input channel is never recovered or cycled: that is for the main mic"
    else
        fail "(s) headroom treated an input channel like the main mic: $(log_since | grep -m 3 -E 'no signal since capture started|Recovering|set profile')"
    fi
    quit_headroom
fi

echo
if [ "$FAILS" -eq 0 ]; then
    echo "ALL INTEGRATION CHECKS PASSED"
    exit 0
fi
echo "$FAILS INTEGRATION CHECK(S) FAILED (logs in $LOG_DIR)"
exit 1
