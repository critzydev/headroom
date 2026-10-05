#!/usr/bin/env python3
"""Helpers for the Headroom PipeWire integration test (standard library only).

Sub-commands (all print a one-line result and exit 0 on success, 1 on failure):
  gen        write a sine test WAV
  tone       measure a tone in a recorded WAV (Goertzel + RMS)
  nodes      wait until named nodes exist in pw-dump
  gone       wait until no node with a given name prefix is left
  linked     wait until an output of node A is linked to node B (through any ports)
  unlinked   wait until no output of node A is linked to node B any more
  inports    check that a source feeds the named ports of the engine (an input channel's in0_L and in0_R)
  silent     check that a recording holds silence (or nothing at all)
  state-inputs   edit the input channels in a state.xml (add one, or take them all away)
  state-outputs  edit what the mixes play to in a state.xml
  graph      check the engine wiring (channel sinks, mic, headphones)
  ready      wait until WirePlumber is up
  order      check that the mic link was created before the headphone link
  snapshot   record Headroom's node and link ids as JSON
  compare    check two snapshots taken around a mic recovery
  defaults   check the default.configured.audio.* metadata
  lint-conf  cheap structural check of a WirePlumber config fragment
"""

import argparse
import array
import json
import math
import struct
import subprocess
import sys
import time
import wave
import xml.etree.ElementTree as ET


# ------------------------------------------------------------------------------------------------
# pw-dump access

def run_pw_dump():
    result = subprocess.run(["pw-dump"], capture_output=True, text=True, timeout=20)
    if result.returncode != 0:
        raise RuntimeError("pw-dump failed: " + result.stderr.strip())
    return json.loads(result.stdout)


class Graph:
    def __init__(self, objects):
        self.nodes = {}
        self.ports = {}
        self.links = []
        self.metadata = {}
        for obj in objects:
            kind = obj.get("type", "")
            info = obj.get("info") or {}
            props = info.get("props") or obj.get("props") or {}
            if kind == "PipeWire:Interface:Node":
                self.nodes[obj["id"]] = {"id": obj["id"], "props": props, "state": info.get("state"),
                                         "serial": int(props.get("object.serial", -1))}
            elif kind == "PipeWire:Interface:Port":
                self.ports[obj["id"]] = {"id": obj["id"], "node": props.get("node.id"),
                                         "name": props.get("port.name"),
                                         "direction": props.get("port.direction")}
            elif kind == "PipeWire:Interface:Link":
                self.links.append({"out_node": info.get("output-node-id"), "in_node": info.get("input-node-id"),
                                   "out_port": info.get("output-port-id"), "in_port": info.get("input-port-id"),
                                   "state": info.get("state")})
            elif kind == "PipeWire:Interface:Metadata":
                name = props.get("metadata.name")
                entries = {}
                for item in obj.get("metadata") or []:
                    if item.get("subject") == 0:
                        entries[item.get("key")] = item.get("value")
                self.metadata[name] = entries

    def node_ids(self, name):
        return [n["id"] for n in self.nodes.values() if n["props"].get("node.name") == name]

    def names(self):
        return sorted(n["props"].get("node.name", "") for n in self.nodes.values())

    def linked(self, out_name, in_name):
        outs = set(self.node_ids(out_name))
        ins = set(self.node_ids(in_name))
        return [l for l in self.links if l["out_node"] in outs and l["in_node"] in ins]


def poll(timeout, interval, check):
    """Calls check() until it returns a truthy value or the timeout passes. Returns the last value."""
    deadline = time.time() + timeout
    value = None
    while True:
        try:
            value = check()
        except Exception as error:  # pw-dump can fail while the graph is changing
            value = None
            last_error = error
        if value:
            return value
        if time.time() >= deadline:
            return value
        time.sleep(interval)


# ------------------------------------------------------------------------------------------------
# WAV helpers

def write_wav(path, rate, channels, frames_bytes):
    with wave.open(path, "wb") as out:
        out.setnchannels(channels)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(frames_bytes)


def read_wav(path):
    """Returns (rate, [channel0 samples, channel1 samples, ...]) as floats in -1..1."""
    with open(path, "rb") as handle:
        data = handle.read()
    if len(data) < 44 or data[0:4] not in (b"RIFF", b"RF64") or data[8:12] != b"WAVE":
        raise ValueError("not a WAV file: %s" % path)

    pos = 12
    fmt = None
    pcm = b""
    while pos + 8 <= len(data):
        chunk_id = data[pos:pos + 4]
        size = struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = pos + 8
        if chunk_id == b"fmt ":
            tag, channels, rate, _, _, bits = struct.unpack("<HHIIHH", data[body:body + 16])
            if tag == 0xFFFE and size >= 26:
                tag = struct.unpack("<H", data[body + 24:body + 26])[0]
            fmt = (tag, channels, rate, bits)
        elif chunk_id == b"data":
            # A recording that was interrupted may carry a bogus size: use what is there.
            if size == 0 or size == 0xFFFFFFFF or body + size > len(data):
                size = len(data) - body
            pcm = data[body:body + size]
            break
        pos = body + size + (size & 1)

    if fmt is None:
        raise ValueError("no fmt chunk in %s" % path)
    tag, channels, rate, bits = fmt
    width = bits // 8
    frame_bytes = width * channels
    count = len(pcm) // frame_bytes
    pcm = pcm[:count * frame_bytes]

    if tag == 1 and bits == 16:
        raw = struct.unpack("<%dh" % (count * channels), pcm)
        scale = 1.0 / 32768.0
    elif tag == 1 and bits == 32:
        raw = struct.unpack("<%di" % (count * channels), pcm)
        scale = 1.0 / 2147483648.0
    elif tag == 1 and bits == 24:
        raw = []
        for i in range(0, len(pcm), 3):
            raw.append(int.from_bytes(pcm[i:i + 3], "little", signed=True))
        scale = 1.0 / 8388608.0
    elif tag == 3 and bits == 32:
        raw = struct.unpack("<%df" % (count * channels), pcm)
        scale = 1.0
    elif tag == 3 and bits == 64:
        raw = struct.unpack("<%dd" % (count * channels), pcm)
        scale = 1.0
    else:
        raise ValueError("unsupported WAV format tag=%d bits=%d" % (tag, bits))

    return rate, [[raw[i * channels + c] * scale for i in range(count)] for c in range(channels)]


# ------------------------------------------------------------------------------------------------
# gen

def cmd_gen(args):
    # One second of a whole-Hz tone repeats exactly, so build a second and repeat it.
    rate = args.rate
    twopi = 2.0 * math.pi * args.freq / rate
    block = array.array("h")
    for i in range(rate):
        value = int(round(max(-1.0, min(1.0, args.amp * math.sin(twopi * i))) * 32767))
        for _ in range(args.channels):
            block.append(value)
    if sys.byteorder == "big":
        block.byteswap()
    whole, rest = divmod(args.seconds, 1.0)
    data = block.tobytes() * int(whole) + block.tobytes()[:int(rest * rate) * args.channels * 2]
    write_wav(args.output, rate, args.channels, data)
    print("wrote %s (%.1f s, %g Hz, %d ch, amp %.3f)" % (args.output, args.seconds, args.freq, args.channels, args.amp))
    return 0


# ------------------------------------------------------------------------------------------------
# tone

def db(value):
    return 20.0 * math.log10(value) if value > 1e-12 else -240.0


def tone_amplitude(window, rate, freq):
    """Peak amplitude of `freq` in `window` by quadrature correlation (exact for whole cycles)."""
    w = 2.0 * math.pi * freq / rate
    i_sum = 0.0
    q_sum = 0.0
    for n, x in enumerate(window):
        angle = w * n
        i_sum += x * math.cos(angle)
        q_sum += x * math.sin(angle)
    return 2.0 * math.hypot(i_sum, q_sum) / len(window)


def cmd_tone(args):
    rate, channels = read_wav(args.wav)
    samples = channels[0]
    if args.tail > 0:
        samples = samples[-int(rate * args.tail):]
    # Whole cycles keep the estimate exact: one second at 48 kHz holds 440 or 1000 cycles.
    win = int(rate * args.window)
    if len(samples) < win:
        print("FAIL: recording is only %.2f s long" % (len(samples) / rate))
        return 1

    # Loudest window (by energy) = where the tone is playing.
    step = max(1, rate // 8)
    prefix = [0.0]
    for x in samples:
        prefix.append(prefix[-1] + x * x)
    best_start, best_energy = 0, -1.0
    for start in range(0, len(samples) - win + 1, step):
        energy = prefix[start + win] - prefix[start]
        if energy > best_energy:
            best_start, best_energy = start, energy
    window = samples[best_start:best_start + win]

    mean_square = best_energy / win
    amp = tone_amplitude(window, rate, args.freq)
    tone_dbfs = db(amp / math.sqrt(2.0))
    total_dbfs = db(math.sqrt(mean_square))
    fraction = (amp * amp / 2.0) / mean_square if mean_square > 0 else 0.0

    message = "%.0f Hz at %.1f dBFS (rms %.1f dBFS, %.0f%% of the energy) at t=%.2fs" % (
        args.freq, tone_dbfs, total_dbfs, fraction * 100.0, best_start / rate)

    if mean_square <= 0 or total_dbfs < -80.0:
        print("FAIL: silence (%s)" % message)
        return 1
    if fraction < args.min_fraction:
        print("FAIL: not the expected tone (%s)" % message)
        return 1
    if abs(tone_dbfs - args.expect_dbfs) > args.tol:
        print("FAIL: level off by %.1f dB, expected %.1f +-%.1f (%s)" % (
            tone_dbfs - args.expect_dbfs, args.expect_dbfs, args.tol, message))
        return 1
    print("OK: " + message)
    return 0


# ------------------------------------------------------------------------------------------------
# graph queries

def cmd_nodes(args):
    def check():
        graph = Graph(run_pw_dump())
        missing = [n for n in args.names if not graph.node_ids(n)]
        return not missing, missing, graph.names()

    ok = False
    deadline = time.time() + args.timeout
    missing = args.names
    while True:
        try:
            ok, missing, _ = check()
        except Exception:
            ok = False
        if ok or time.time() >= deadline:
            break
        time.sleep(0.5)
    if ok:
        print("OK: nodes present: " + ", ".join(args.names))
        return 0
    print("FAIL: missing nodes: " + ", ".join(missing))
    return 1


def cmd_gone(args):
    def check():
        graph = Graph(run_pw_dump())
        left = [n for n in graph.names() if n.startswith(args.prefix)]
        return None if left else True, left

    deadline = time.time() + args.timeout
    left = ["?"]
    while True:
        try:
            done, left = check()
        except Exception:
            done = None
        if done:
            print("OK: no '%s*' nodes left" % args.prefix)
            return 0
        if time.time() >= deadline:
            print("FAIL: nodes still present: " + ", ".join(left))
            return 1
        time.sleep(0.5)


def cmd_linked(args):
    def check():
        return Graph(run_pw_dump()).linked(args.source, args.target)

    found = poll(args.timeout, 0.5, check)
    if found:
        print("OK: %s -> %s linked (%d links)" % (args.source, args.target, len(found)))
        return 0
    print("FAIL: %s is not linked to %s" % (args.source, args.target))
    return 1


def cmd_unlinked(args):
    def check():
        return not Graph(run_pw_dump()).linked(args.source, args.target)

    if poll(args.timeout, 0.5, check):
        print("OK: %s is not linked to %s" % (args.source, args.target))
        return 0
    print("FAIL: %s is still linked to %s" % (args.source, args.target))
    return 1


def cmd_inports(args):
    """A capture device that is an input channel feeds the engine's pair of input ports for its slot: a mono device both of them."""
    state = {"fed": set()}

    def check():
        graph = Graph(run_pw_dump())
        engine = set(graph.node_ids("headroom.engine"))
        source = set(graph.node_ids(args.source))
        fed = set()
        for link in graph.links:
            if link["out_node"] in source and link["in_node"] in engine:
                port = graph.ports.get(link["in_port"])
                if port:
                    fed.add(port["name"])
        state["fed"] = fed
        return all(name in fed for name in args.ports)

    if poll(args.timeout, 0.5, check):
        print("OK: %s feeds the engine's %s" % (args.source, ", ".join(args.ports)))
        return 0
    print("FAIL: %s feeds %s, not all of %s" % (args.source, sorted(state["fed"]) or "no engine port", ", ".join(args.ports)))
    return 1


def cmd_silent(args):
    """A removed output gets nothing: a recording of it is either empty (an idle sink may not deliver any buffer) or all zeros."""
    try:
        rate, channels = read_wav(args.wav)
    except (ValueError, OSError):
        print("OK: nothing recorded (no usable file)")
        return 0
    frames = len(channels[0]) if channels else 0
    if frames < int(rate * 0.2):
        print("OK: nothing recorded (%d frames)" % frames)
        return 0
    mean_square = sum(x * x for x in channels[0]) / frames
    level = db(math.sqrt(mean_square))
    if level < -80.0:
        print("OK: silence (%.1f dBFS over %.1f s)" % (level, frames / rate))
        return 0
    print("FAIL: there is sound (%.1f dBFS over %.1f s)" % (level, frames / rate))
    return 1


def cmd_state_inputs(args):
    """Edits the input channels of a state.xml: `add` puts one in (the first free slot) after removing none, `clear` takes all away.
    Headroom fills in everything else (levels, colour, icon) when it reads the file."""
    tree = ET.parse(args.file)
    channels = tree.getroot().find("CHANNELS")
    if channels is None:
        print("FAIL: %s has no CHANNELS" % args.file)
        return 1
    if args.action == "clear":
        for channel in list(channels):
            if channel.get("kind") == "input":
                channels.remove(channel)
    else:
        used = {int(c.get("slot", "-1")) for c in channels if c.get("kind") == "input"}
        slot = next(i for i in range(8) if i not in used)
        # Heard in both mixes on purpose: a new input channel made in the app starts silent in the Headphones mix.
        ET.SubElement(channels, "CHANNEL", {"id": args.id, "kind": "input", "name": args.name, "device": args.device,
                                            "channels": args.channels, "slot": str(slot), "monitorDb": "0", "streamDb": "0"})
    tree.write(args.file, encoding="utf-8")
    print("OK: %s input channels in %s: %s" % (args.action, args.file,
                                               [c.get("id") for c in channels if c.get("kind") == "input"]))
    return 0


def cmd_state_outputs(args):
    """Sets what the Headphones and Stream mixes play to in a state.xml (node.names joined with |)."""
    tree = ET.parse(args.file)
    root = tree.getroot()
    mixes = root.find("MIXES")
    if mixes is None:
        print("FAIL: %s has no MIXES" % args.file)
        return 1
    for mix in mixes:
        if mix.get("id") == "monitor":
            mix.set("outputs", args.headphones)
            mix.set("outputDevice", args.headphones.split("|")[0])
            settings = root.find("SETTINGS")
            if settings is not None:
                settings.set("outputDevice", args.headphones.split("|")[0])
        elif mix.get("id") == "stream":
            mix.set("outputs", args.stream)
    tree.write(args.file, encoding="utf-8")
    print("OK: Headphones play to '%s', Stream also to '%s'" % (args.headphones, args.stream))
    return 0


def cmd_graph(args):
    """The engine must have: every channel sink feeding it, the fake mic feeding it,
    it feeding the fake headphones, Headroom Mic and Headroom Stream Mix."""
    def check():
        graph = Graph(run_pw_dump())
        problems = []
        if not graph.node_ids("headroom.engine"):
            return ["headroom.engine is missing"]
        for channel in args.channels:
            if not graph.linked("headroom.ch." + channel, "headroom.engine"):
                problems.append("headroom.ch.%s -> engine" % channel)
        if not graph.linked(args.source, "headroom.engine"):
            problems.append("%s -> engine (mic)" % args.source)
        if not graph.linked("headroom.engine", args.sink):
            problems.append("engine -> %s (headphones)" % args.sink)
        if not graph.linked("headroom.engine", "headroom.mic"):
            problems.append("engine -> headroom.mic")
        if not graph.linked("headroom.engine", "headroom.stream"):
            problems.append("engine -> headroom.stream")
        return problems

    deadline = time.time() + args.timeout
    problems = ["?"]
    while True:
        try:
            problems = check()
        except Exception as error:
            problems = ["pw-dump: %s" % error]
        if not problems or time.time() >= deadline:
            break
        time.sleep(0.5)
    if not problems:
        print("OK: engine is wired: %d channel sinks, mic, headphones, Headroom Mic and Stream Mix" % len(args.channels))
        return 0
    print("FAIL: missing links: " + "; ".join(problems))
    return 1


def configured_name(entries, key):
    value = entries.get(key)
    if isinstance(value, dict):
        return value.get("name", "")
    if isinstance(value, str):
        try:
            return json.loads(value).get("name", "")
        except ValueError:
            return value
    return ""


def cmd_ready(args):
    """Waits until WirePlumber is up. It exports "sm-settings" at start-up; the "default" metadata only
    shows up once the first device appears, so that one is checked separately (defaults)."""
    state = {"seen": None, "error": None}

    def check():
        try:
            text = subprocess.run(["pw-dump"], capture_output=True, text=True, timeout=20).stdout
            state["seen"] = sorted(Graph(json.loads(text)).metadata.keys())
        except Exception as error:
            state["error"] = "%s: %s" % (type(error).__name__, error)
            return False
        return "sm-settings" in state["seen"]

    if poll(args.timeout, 0.5, check):
        print("OK: WirePlumber is up (settings published)")
        return 0
    print("FAIL: WirePlumber did not publish its settings within %.0f s (metadata seen: %s, last error: %s)" % (
        args.timeout, state["seen"], state["error"]))
    return 1


def cmd_order(args):
    """The whole point of WaveGuard: the mic is linked before the headphones. Link serials grow with creation order."""
    objects = run_pw_dump()
    graph = Graph(objects)
    engine = set(graph.node_ids("headroom.engine"))
    if not engine:
        print("FAIL: headroom.engine is missing")
        return 1

    source_ids = set(graph.node_ids(args.source))
    sink_ids = set(graph.node_ids(args.sink))
    capture, playback = [], []
    for obj in objects:
        if obj.get("type") != "PipeWire:Interface:Link":
            continue
        info = obj.get("info") or {}
        if info.get("output-node-id") in source_ids and info.get("input-node-id") in engine:
            capture.append(serial_of(obj))
        if info.get("output-node-id") in engine and info.get("input-node-id") in sink_ids:
            playback.append(serial_of(obj))

    if not capture or not playback:
        print("FAIL: mic links %s, headphone links %s" % (capture, playback))
        return 1
    if min(capture) < min(playback):
        print("OK: mic link (serial %d) was created before headphone link (serial %d)" % (min(capture), min(playback)))
        return 0
    print("FAIL: headphone link (serial %d) was created before mic link (serial %d)" % (min(playback), min(capture)))
    return 1


def serial_of(obj):
    """object.serial never repeats, unlike global ids, which PipeWire reuses."""
    props = (obj.get("info") or {}).get("props") or {}
    return int(props.get("object.serial", -1))


def take_snapshot(source, sink, keeper):
    objects = run_pw_dump()
    graph = Graph(objects)
    names = ["headroom.engine", "headroom.mic", "headroom.stream", "headroom.ch.system", "headroom.ch.game",
             "headroom.ch.music", "headroom.ch.chat", keeper]
    nodes = {name: sorted([n["id"], n["serial"]] for n in graph.nodes.values()
                          if n["props"].get("node.name") == name) for name in names}
    ours = {pair[0] for name, pairs in nodes.items() if name.startswith("headroom.") for pair in pairs}
    engine = set(graph.node_ids("headroom.engine"))
    mic_out = set(graph.node_ids("headroom.mic"))
    keeper_ids = set(graph.node_ids(keeper))
    source_ids = set(graph.node_ids(source))
    sink_ids = set(graph.node_ids(sink))
    snap = {"nodes": nodes, "internal_links": [], "keeper_links": [], "mic_links": [], "playback_links": []}
    for obj in objects:
        if obj.get("type") != "PipeWire:Interface:Link":
            continue
        info = obj.get("info") or {}
        out_node, in_node = info.get("output-node-id"), info.get("input-node-id")
        entry = [obj["id"], serial_of(obj)]
        if out_node in ours and in_node in ours:
            snap["internal_links"].append(entry)
        if out_node in mic_out and in_node in keeper_ids:
            snap["keeper_links"].append(entry)
        if out_node in source_ids and in_node in engine:
            snap["mic_links"].append(entry)
        if out_node in engine and in_node in sink_ids:
            snap["playback_links"].append(entry)
    for key in ("internal_links", "keeper_links", "mic_links", "playback_links"):
        snap[key].sort()
    return snap


def cmd_snapshot(args):
    print(json.dumps(take_snapshot(args.source, args.sink, args.keeper)))
    return 0


def cmd_compare(args):
    """Headroom's own nodes and links, and the listener on Headroom Mic, must have survived a mic recovery
    untouched (same id and same serial), while the mic and headphone links (the Wave side) were recreated
    (a serial higher than any before: ids are recycled by PipeWire, serials never are)."""
    with open(args.before) as handle:
        before = json.load(handle)
    with open(args.after) as handle:
        after = json.load(handle)

    problems = []
    if before["nodes"] != after["nodes"] or any(not pairs for pairs in after["nodes"].values()):
        problems.append("node ids/serials changed: %s -> %s" % (before["nodes"], after["nodes"]))
    if before["internal_links"] != after["internal_links"] or not after["internal_links"]:
        problems.append("Headroom's internal links changed: %s -> %s" % (before["internal_links"], after["internal_links"]))
    if before["keeper_links"] != after["keeper_links"] or not after["keeper_links"]:
        problems.append("the listener on Headroom Mic was relinked: %s -> %s" % (before["keeper_links"], after["keeper_links"]))
    for key, label in (("mic_links", "mic"), ("playback_links", "headphone")):
        if not before[key] or not after[key]:
            problems.append("no %s links: %s -> %s" % (label, before[key], after[key]))
        elif min(entry[1] for entry in after[key]) <= max(entry[1] for entry in before[key]):
            problems.append("the %s links were not recreated: %s -> %s" % (label, before[key], after[key]))
    if problems:
        print("FAIL: " + "; ".join(problems))
        return 1
    print("OK: Headroom's %d nodes, %d internal links and the listener's link are untouched; the mic links %s and headphone links %s were recreated" % (
        sum(len(v) for v in after["nodes"].values()), len(after["internal_links"]),
        [e[0] for e in after["mic_links"]], [e[0] for e in after["playback_links"]]))
    return 0


def cmd_defaults(args):
    """default.configured.audio.sink/source must point at Headroom (or, with --expect-sink, at that node
    for the sink, which is how a deliberate choice by the user is simulated)."""
    def check():
        graph = Graph(run_pw_dump())
        entries = graph.metadata.get("default", {})
        sink = configured_name(entries, "default.configured.audio.sink")
        source = configured_name(entries, "default.configured.audio.source")
        if args.expect_sink:
            good = sink == args.expect_sink
        else:
            good = sink.startswith("headroom.ch.") and source == "headroom.mic"
        return (good, sink, source)

    deadline = time.time() + args.timeout
    result = (False, "", "")
    while True:
        try:
            result = check()
        except Exception:
            pass
        if result[0] or time.time() >= deadline:
            break
        time.sleep(0.5)
    good, sink, source = result
    if good:
        print("OK: default.configured.audio.sink=%s source=%s" % (sink, source))
        return 0
    print("FAIL: default.configured.audio.sink=%r source=%r (wanted %s)" % (
        sink, source, args.expect_sink or "headroom.ch.* and headroom.mic"))
    return 1


# ------------------------------------------------------------------------------------------------
# lint-conf

def cmd_lint_conf(args):
    """Not a WirePlumber parser; catches the mistakes that stop a fragment from loading:
    unbalanced braces/brackets/quotes and a top level that is not `name = [ ... ]` / `name = { ... }`."""
    with open(args.file, encoding="utf-8") as handle:
        text = handle.read()

    stack = []
    in_string = False
    in_comment = False
    line = 1
    depth_zero_tokens = []
    token = ""
    for ch in text:
        if ch == "\n":
            line += 1
            in_comment = False
        if in_comment:
            continue
        if in_string:
            if ch == '"':
                in_string = False
            continue
        if ch == "#":
            in_comment = True
            continue
        if ch == '"':
            in_string = True
            continue
        if ch in "{[":
            if not stack and token:
                depth_zero_tokens.append(token)
                token = ""
            stack.append((ch, line))
            continue
        if ch in "}]":
            if not stack:
                print("FAIL: line %d: '%s' closes nothing" % (line, ch))
                return 1
            opener, opened_at = stack.pop()
            if (opener, ch) not in (("{", "}"), ("[", "]")):
                print("FAIL: line %d: '%s' does not match '%s' from line %d" % (line, ch, opener, opened_at))
                return 1
            continue
        if not stack:
            if ch in " \t\r\n=:,":
                if token:
                    depth_zero_tokens.append(token)
                    token = ""
            else:
                token += ch

    if in_string:
        print("FAIL: unterminated string")
        return 1
    if stack:
        print("FAIL: '%s' from line %d is never closed" % stack[-1])
        return 1
    if token:
        depth_zero_tokens.append(token)
    if not depth_zero_tokens:
        print("FAIL: no sections found")
        return 1
    for name in depth_zero_tokens:
        if not all(part.replace("-", "").replace("_", "").isalnum() for part in name.split(".")):
            print("FAIL: odd section name '%s'" % name)
            return 1
    print("OK: %s looks well-formed (sections: %s)" % (args.file, ", ".join(depth_zero_tokens)))
    return 0


# ------------------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("gen")
    p.add_argument("output")
    p.add_argument("--freq", type=float, required=True)
    p.add_argument("--amp", type=float, default=0.25)
    p.add_argument("--seconds", type=float, default=5.0)
    p.add_argument("--channels", type=int, default=2)
    p.add_argument("--rate", type=int, default=48000)
    p.set_defaults(func=cmd_gen)

    p = sub.add_parser("tone")
    p.add_argument("wav")
    p.add_argument("--freq", type=float, required=True)
    p.add_argument("--expect-dbfs", type=float, required=True)
    p.add_argument("--tol", type=float, default=3.0)
    p.add_argument("--window", type=float, default=1.0)
    p.add_argument("--min-fraction", type=float, default=0.6)
    p.add_argument("--tail", type=float, default=0.0, help="only look at the last N seconds of the recording")
    p.set_defaults(func=cmd_tone)

    p = sub.add_parser("nodes")
    p.add_argument("names", nargs="+")
    p.add_argument("--timeout", type=float, default=30.0)
    p.set_defaults(func=cmd_nodes)

    p = sub.add_parser("gone")
    p.add_argument("--prefix", default="headroom.")
    p.add_argument("--timeout", type=float, default=10.0)
    p.set_defaults(func=cmd_gone)

    p = sub.add_parser("linked")
    p.add_argument("source")
    p.add_argument("target")
    p.add_argument("--timeout", type=float, default=15.0)
    p.set_defaults(func=cmd_linked)

    p = sub.add_parser("unlinked")
    p.add_argument("source")
    p.add_argument("target")
    p.add_argument("--timeout", type=float, default=15.0)
    p.set_defaults(func=cmd_unlinked)

    p = sub.add_parser("inports")
    p.add_argument("--source", required=True)
    p.add_argument("--ports", nargs="+", required=True)
    p.add_argument("--timeout", type=float, default=15.0)
    p.set_defaults(func=cmd_inports)

    p = sub.add_parser("silent")
    p.add_argument("wav")
    p.set_defaults(func=cmd_silent)

    p = sub.add_parser("state-inputs")
    p.add_argument("file")
    p.add_argument("action", choices=["add", "clear"])
    p.add_argument("--id", default="mic2")
    p.add_argument("--name", default="Second mic")
    p.add_argument("--device", default="")
    p.add_argument("--channels", default="mono", choices=["mono", "stereo"])
    p.set_defaults(func=cmd_state_inputs)

    p = sub.add_parser("state-outputs")
    p.add_argument("file")
    p.add_argument("--headphones", default="auto")
    p.add_argument("--stream", default="")
    p.set_defaults(func=cmd_state_outputs)

    p = sub.add_parser("graph")
    p.add_argument("--sink", required=True)
    p.add_argument("--source", required=True)
    p.add_argument("--channels", nargs="+", default=["system", "game", "music", "chat"])
    p.add_argument("--timeout", type=float, default=20.0)
    p.set_defaults(func=cmd_graph)

    p = sub.add_parser("snapshot")
    p.add_argument("--sink", required=True)
    p.add_argument("--source", required=True)
    p.add_argument("--keeper", required=True)
    p.set_defaults(func=cmd_snapshot)

    p = sub.add_parser("compare")
    p.add_argument("before")
    p.add_argument("after")
    p.set_defaults(func=cmd_compare)

    p = sub.add_parser("order")
    p.add_argument("--sink", required=True)
    p.add_argument("--source", required=True)
    p.set_defaults(func=cmd_order)

    p = sub.add_parser("ready")
    p.add_argument("--timeout", type=float, default=40.0)
    p.set_defaults(func=cmd_ready)

    p = sub.add_parser("defaults")
    p.add_argument("--timeout", type=float, default=15.0)
    p.add_argument("--expect-sink", default="")
    p.set_defaults(func=cmd_defaults)

    p = sub.add_parser("lint-conf")
    p.add_argument("file")
    p.set_defaults(func=cmd_lint_conf)

    args = parser.parse_args()
    try:
        return args.func(args)
    except Exception as error:  # a broken test tool must fail the test, not crash silently
        print("FAIL: %s: %s" % (type(error).__name__, error))
        return 1


if __name__ == "__main__":
    sys.exit(main())
