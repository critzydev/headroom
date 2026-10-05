// CpuPlacement implementation: cpulist parsing and the choice of CPUs (pure), reading /sys and pinning (Linux only).
#include "engine/denoise/CpuPlacement.h"

#include "app/Log.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>

#if JUCE_LINUX
 #include <sched.h>
#endif

namespace hr::denoise::cpu
{
namespace
{
    // A CPU number: decimal digits only, at most kMaxCpu.
    bool parseNumber (const std::string& text, int& value)
    {
        if (text.empty() || text.size() > 5)
            return false;

        int result = 0;

        for (const char ch : text)
        {
            if (ch < '0' || ch > '9')
                return false;

            result = result * 10 + (ch - '0');
        }

        if (result > kMaxCpu)
            return false;

        value = result;
        return true;
    }

    bool parseItem (const std::string& item, CpuList& into)
    {
        const auto dash = item.find ('-');
        int first = 0, last = 0;

        if (dash == std::string::npos)
        {
            if (! parseNumber (item, first))
                return false;

            last = first;
        }
        else if (! parseNumber (item.substr (0, dash), first) || ! parseNumber (item.substr (dash + 1), last) || last < first)
        {
            return false;
        }

        for (int cpu = first; cpu <= last; ++cpu)
            into.push_back (cpu);

        return true;
    }

    bool disjoint (const std::vector<CpuList>& groups)
    {
        CpuList all;

        for (const auto& group : groups)
            all.insert (all.end(), group.begin(), group.end());

        std::sort (all.begin(), all.end());
        return std::adjacent_find (all.begin(), all.end()) == all.end();
    }
}

// ---------------------------------------------------------------------------------------------------
// Pure part
// ---------------------------------------------------------------------------------------------------

bool parseCpuList (const juce::String& text, CpuList& out)
{
    out.clear();

    const auto whole = text.trim().toStdString();

    if (whole.empty())
        return false;

    CpuList result;
    size_t begin = 0;

    while (true)
    {
        const auto comma = whole.find (',', begin);
        const auto item = whole.substr (begin, comma == std::string::npos ? std::string::npos : comma - begin);

        if (! parseItem (item, result))
            return false;

        if (comma == std::string::npos)
            break;

        begin = comma + 1;
    }

    std::sort (result.begin(), result.end());
    result.erase (std::unique (result.begin(), result.end()), result.end());
    out = std::move (result);
    return true;
}

juce::String formatCpuList (const CpuList& cpus)
{
    juce::String text;

    for (size_t i = 0; i < cpus.size();)
    {
        size_t end = i;

        while (end + 1 < cpus.size() && cpus[end + 1] == cpus[end] + 1)
            ++end;

        if (text.isNotEmpty())
            text << ",";

        text << juce::String (cpus[i]);

        if (end > i)
            text << "-" << juce::String (cpus[end]);

        i = end + 1;
    }

    return text;
}

std::vector<CpuList> parseCacheGroups (const std::vector<juce::String>& sharedCpuLists, const juce::String& onlineList)
{
    CpuList online;
    const bool filterByOnline = parseCpuList (onlineList, online);

    std::vector<CpuList> groups;

    for (const auto& text : sharedCpuLists)
    {
        CpuList group;

        if (! parseCpuList (text, group))
            continue;

        if (filterByOnline)
            group.erase (std::remove_if (group.begin(), group.end(),
                                         [&] (int cpu) { return ! std::binary_search (online.begin(), online.end(), cpu); }),
                         group.end());

        if (! group.empty())
            groups.push_back (std::move (group));
    }

    std::sort (groups.begin(), groups.end());       // lexicographic: by lowest CPU
    groups.erase (std::unique (groups.begin(), groups.end()), groups.end());

    if (! disjoint (groups))
        groups.clear();

    return groups;
}

Placement chooseWorkerCpus (const std::vector<CpuList>& groups, const CpuList& allowed)
{
    std::vector<const CpuList*> real;

    for (const auto& group : groups)
        if (! group.empty())
            real.push_back (&group);

    if (real.empty())
        return { {}, "cache topology unknown" };

    if (real.size() == 1)
        return { {}, "single cache group" };

    const auto& last = **std::max_element (real.begin(), real.end(),
                                           [] (const CpuList* a, const CpuList* b) { return a->front() < b->front(); });

    auto permitted = allowed;
    std::sort (permitted.begin(), permitted.end());

    CpuList chosen;
    std::set_intersection (last.begin(), last.end(), permitted.begin(), permitted.end(), std::back_inserter (chosen));

    if (chosen.empty())
        return { {}, "the process may not run on the last cache group" };

    return { std::move (chosen), {} };
}

juce::String describe (const Placement& placement)
{
    if (placement.cpus.empty())
        return "no pinning (" + placement.reason + ")";

    return "CPUs " + formatCpuList (placement.cpus) + " (last cache group)";
}

bool isPinningDisabled (const juce::String& environmentValue) noexcept
{
    return environmentValue.trim() == "1";
}

// ---------------------------------------------------------------------------------------------------
// Linux: sysfs and the affinity of the calling thread
// ---------------------------------------------------------------------------------------------------

#if JUCE_LINUX

namespace
{
    constexpr const char* kSysCpu = "/sys/devices/system/cpu";

    // shared_cpu_list of the L3 of every CPU (cpuN/cache/index3, on x86 always the L3; `level` is checked where it can be read).
    std::vector<juce::String> readSharedCpuLists()
    {
        std::vector<juce::String> lists;

        for (const auto& dir : juce::File (kSysCpu).findChildFiles (juce::File::findDirectories, false, "cpu*"))
        {
            const auto name = dir.getFileName().fromFirstOccurrenceOf ("cpu", false, false);

            if (name.isEmpty() || ! name.containsOnly ("0123456789"))
                continue;                                   // cpufreq, cpuidle

            const auto index3 = dir.getChildFile ("cache/index3");

            if (const auto level = index3.getChildFile ("level"); level.existsAsFile() && level.loadFileAsString().trim() != "3")
                continue;

            if (const auto shared = index3.getChildFile ("shared_cpu_list"); shared.existsAsFile())
                lists.push_back (shared.loadFileAsString());
        }

        return lists;
    }

    // Decided once for the process, by the first thread that asks (which is not pinned yet, so its affinity is the process's).
    const Placement& workerPlacement()
    {
        static const Placement placement = []() -> Placement
        {
            const auto groups = parseCacheGroups (readSharedCpuLists(), juce::File (kSysCpu).getChildFile ("online").loadFileAsString());

            cpu_set_t mask;
            CPU_ZERO (&mask);

            if (sched_getaffinity (0, sizeof (mask), &mask) != 0)
                return { {}, "the process's CPU affinity could not be read" };

            CpuList allowed;

            for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
                if (CPU_ISSET (cpu, &mask))
                    allowed.push_back (cpu);

            return chooseWorkerCpus (groups, allowed);
        }();

        return placement;
    }

    void logOnce (const juce::String& line, bool problem)
    {
        static std::atomic<bool> logged { false };

        if (logged.exchange (true))
            return;

        if (problem)
            Log::warn ("Noise removal thread: " + line);
        else
            Log::info ("Noise removal thread: " + line);
    }
}

bool setCurrentThreadCpus (const CpuList& cpus, juce::String& error)
{
    cpu_set_t set;
    CPU_ZERO (&set);
    int count = 0;

    for (const int cpu : cpus)
        if (cpu >= 0 && cpu < CPU_SETSIZE)
        {
            CPU_SET (cpu, &set);
            ++count;
        }

    if (count == 0)
    {
        error = "no CPU to pin to";
        return false;
    }

    // pid 0 = the calling thread only.
    if (sched_setaffinity (0, sizeof (set), &set) != 0)
    {
        error = std::strerror (errno);
        return false;
    }

    return true;
}

CpuList pinCurrentThread()
{
    Placement placement;

    if (isPinningDisabled (juce::SystemStats::getEnvironmentVariable ("HEADROOM_NO_PIN", {})))
        placement.reason = "HEADROOM_NO_PIN is set";
    else
        placement = workerPlacement();

    bool problem = false;

    if (! placement.cpus.empty())
    {
        juce::String error;

        if (! setCurrentThreadCpus (placement.cpus, error))
        {
            placement = { {}, "could not set the affinity: " + error };
            problem = true;
        }
    }

    logOnce (describe (placement), problem);
    return placement.cpus;
}

#else

bool setCurrentThreadCpus (const CpuList&, juce::String& error)
{
    error = "CPU pinning is only done on Linux";
    return false;
}

CpuList pinCurrentThread()
{
    return {};
}

#endif
}
