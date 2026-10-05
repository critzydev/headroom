// CpuPlacement: keeps Headroom's own noise-removal worker threads on the last L3 cache group (CCD) so that they stay off
// the cores a game prefers. Everything that decides is pure and tested on every platform;
// only reading /sys and the affinity calls are Linux-only.
#pragma once

#include "Juce.h"

#include <vector>

namespace hr::denoise::cpu
{
// Logical CPU numbers, ascending, no duplicates.
using CpuList = std::vector<int>;

// Largest CPU number a list may contain (the kernel's own limit is 8192): a garbage range must not become a huge vector.
constexpr int kMaxCpu = 16383;

// Parses the kernel's "cpulist" text: "0-5,12-17", "3", "0-5,12-17\n" (what sysfs prints in cache/index3/shared_cpu_list and
// in /sys/devices/system/cpu/online). False, with `out` empty, for empty text and for anything else: an empty item, stray
// characters, a range that runs backwards, a number above kMaxCpu.
bool parseCpuList (const juce::String& text, CpuList& out);

// The inverse, in the kernel's format: {6..11, 18..23} -> "6-11,18-23".
juce::String formatCpuList (const CpuList& cpus);

// The groups of CPUs that share an L3 cache, from the shared_cpu_list text of each CPU (one entry per CPU, so a group
// appears once per member). Entries that are empty (file missing) or malformed are skipped, equal groups are merged, CPUs
// that are not in `onlineList` are dropped (an empty or malformed online list filters nothing), groups left empty vanish,
// and the result is ordered by each group's lowest CPU. Groups that overlap without being equal cannot be a real topology:
// the answer is then empty ("unknown"), the same as when nothing could be read.
std::vector<CpuList> parseCacheGroups (const std::vector<juce::String>& sharedCpuLists, const juce::String& onlineList);

// Where the worker threads should run, or why nowhere in particular.
struct Placement
{
    CpuList cpus;               // empty = leave the threads where the system puts them
    juce::String reason;        // when empty: "single cache group", "cache topology unknown", ...
};

// The last cache group (the one whose lowest CPU is the highest: 6-11,18-23 on a Ryzen 9 7900X, whose first CCD the games
// prefer), cut down to `allowed`, the CPUs the process may run on. No preference when there is not more than one group,
// and when the process's own affinity leaves nothing of the last group (a user who restricted it knows better).
Placement chooseWorkerCpus (const std::vector<CpuList>& groups, const CpuList& allowed);

// One phrase for the log: "CPUs 6-11,18-23 (last cache group)" or "no pinning (single cache group)".
juce::String describe (const Placement& placement);

// HEADROOM_NO_PIN=1 turns pinning off (`environmentValue` is what the variable holds, empty when it is not set).
bool isPinningDisabled (const juce::String& environmentValue) noexcept;

// Linux: restricts the calling thread, and only it, to `cpus` (sched_setaffinity on this thread). False, with the reason in
// `error`, when there is nothing to pin to (empty list, every number beyond what cpu_set_t holds) or the kernel says no.
// Elsewhere it does nothing and returns false. Threads this thread starts afterwards inherit the CPUs.
bool setCurrentThreadCpus (const CpuList& cpus, juce::String& error);

// Linux: call on the thread itself, at its start and before it asks for realtime priority. Reads the cache topology once,
// pins the calling thread (only it) to the chosen CPUs, and returns them; empty when the thread was left alone (single
// L3 group, unknown topology, HEADROOM_NO_PIN=1, the process's affinity, or the kernel said no). The first call in the
// process logs the outcome as "Noise removal thread: ...". Elsewhere: does nothing and returns empty.
//
// Never call it on the PipeWire loop or the JUCE message thread. A thread started by a pinned thread inherits its CPUs.
CpuList pinCurrentThread();
}
