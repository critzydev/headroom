// Unit tests for CpuPlacement: the cpulist parser, the cache groups of a machine, the choice of the last L3 group, and (Linux)
// that a thread really ends up there. The parsing and the choice run on every platform with the strings sysfs gives.
#include "Juce.h"

#include "engine/builtin/NoiseRemoval.h"
#include "engine/denoise/CpuPlacement.h"
#include "engine/denoise/DfnRuntime.h"

#include <algorithm>
#include <set>
#include <thread>
#include <vector>

#if JUCE_LINUX
 #include <sched.h>
 #include <stdlib.h>
#endif

namespace hr
{
using namespace denoise::cpu;

class CpuPlacementTests final : public juce::UnitTest
{
public:
    CpuPlacementTests() : juce::UnitTest ("CpuPlacement", "headroom") {}

    void runTest() override
    {
        beginTest ("cpulist text: ranges, commas, the trailing newline sysfs adds");
        testParse();

        beginTest ("cpulist text: anything malformed is refused, and the answer is empty");
        testParseGarbage();

        beginTest ("cpulist text: formatting is the inverse");
        testFormat();

        beginTest ("Ryzen 9 7900X: two CCDs, the workers go to the second one");
        testRyzen();

        beginTest ("one cache group: no pinning");
        testSingleGroup();

        beginTest ("three cache groups: the last one");
        testThreeGroups();

        beginTest ("garbage and missing files: no preference");
        testGarbage();

        beginTest ("offline CPUs are not part of a group");
        testOffline();

        beginTest ("the process's own affinity is respected");
        testAffinity();

        beginTest ("HEADROOM_NO_PIN=1 is the only value that turns pinning off");
        testDisabled();

       #if JUCE_LINUX
        beginTest ("Linux: a thread ends up on the CPUs that were chosen, or is left alone");
        testPinThread();

        beginTest ("Linux: setCurrentThreadCpus restricts only the calling thread, and what it starts inherits that");
        testSetCpus();

        beginTest ("Linux: the noise removal loader and worker threads are pinned like that");
        testNoiseRemovalThreads();
       #else
        beginTest ("elsewhere: pinning does nothing");
        {
            juce::String error;
            expect (pinCurrentThread().empty());
            expect (! setCurrentThreadCpus (CpuList { 0 }, error) && error.isNotEmpty());
        }
       #endif
    }

private:
    // ---- Helpers -----------------------------------------------------------------------------------
    CpuList parse (const char* text)
    {
        CpuList cpus;
        expect (parseCpuList (text, cpus), juce::String ("should parse: ") + text);
        return cpus;
    }

    static CpuList range (int first, int last)
    {
        CpuList cpus;

        for (int cpu = first; cpu <= last; ++cpu)
            cpus.push_back (cpu);

        return cpus;
    }

    static CpuList join (CpuList a, const CpuList& b)
    {
        a.insert (a.end(), b.begin(), b.end());
        return a;
    }

    // What the kernel prints for cpu0..cpu23 of a Ryzen 9 7900X (two CCDs, SMT: cpu N and N + 12 are siblings).
    static std::vector<juce::String> ryzen7900xSharedLists()
    {
        std::vector<juce::String> lists;

        for (int cpu = 0; cpu < 24; ++cpu)
            lists.push_back ((cpu % 12) < 6 ? "0-5,12-17\n" : "6-11,18-23\n");

        return lists;
    }

    static CpuList everything (int count)   { return range (0, count - 1); }

    // ---- Parsing -----------------------------------------------------------------------------------
    void testParse()
    {
        expect (parse ("0-5,12-17") == join (range (0, 5), range (12, 17)));
        expect (parse ("6-11,18-23\n") == join (range (6, 11), range (18, 23)));
        expect (parse ("0-23\n") == everything (24));
        expect (parse ("3") == CpuList { 3 });
        expect (parse ("0") == CpuList { 0 });
        expect (parse ("  7  \n") == CpuList { 7 });
        expect (parse ("5,3-4,4") == CpuList ({ 3, 4, 5 }), "sorted, no duplicates");
        expect (parse ("2,0,1") == CpuList ({ 0, 1, 2 }));
        expect (parse ("4-4") == CpuList { 4 });
        expect (parse ("0-16383").size() == 16384u, "the highest CPU number that is accepted");

        CpuList filled { 99 };
        expect (parseCpuList ("1-2", filled) && filled == CpuList ({ 1, 2 }), "replaces what was there");
    }

    void testParseGarbage()
    {
        const char* bad[] = { "", " ", "\n", "garbage", "cpu0", "1,,2", ",", "1,", ",1", "1-", "-3", "5-2", "1-2-3", "0-5:2/4", "1 2", "1, 2",
                              "0x3", "+3", "-1", "3.5", "0-5;12-17", "16384", "0-99999999999", "999999", "\xEF\xBC\x91" /* a full-width 1 */ };

        for (auto* text : bad)
        {
            CpuList cpus { 1, 2, 3 };
            expect (! parseCpuList (text, cpus), juce::String ("should be refused: \"") + juce::String::fromUTF8 (text).replace ("\n", "\\n") + "\"");
            expect (cpus.empty(), "and leave nothing behind");
        }
    }

    void testFormat()
    {
        expectEquals (formatCpuList (join (range (6, 11), range (18, 23))), juce::String ("6-11,18-23"));
        expectEquals (formatCpuList (join (range (0, 5), range (12, 17))), juce::String ("0-5,12-17"));
        expectEquals (formatCpuList (CpuList {}), juce::String());
        expectEquals (formatCpuList (CpuList { 5 }), juce::String ("5"));
        expectEquals (formatCpuList (CpuList { 0, 1 }), juce::String ("0-1"));
        expectEquals (formatCpuList (CpuList { 1, 3, 5 }), juce::String ("1,3,5"));
        expectEquals (formatCpuList (CpuList { 1, 2, 3, 7, 9, 10 }), juce::String ("1-3,7,9-10"));

        for (auto* text : { "0-5,12-17", "6-11,18-23", "3", "0-23", "1-3,7,9-10" })
            expectEquals (formatCpuList (parse (text)), juce::String (text), "round trip");
    }

    // ---- Machines ----------------------------------------------------------------------------------
    void testRyzen()
    {
        const auto groups = parseCacheGroups (ryzen7900xSharedLists(), "0-23\n");
        expectEquals ((int) groups.size(), 2);

        if (groups.size() == 2)
        {
            expectEquals (formatCpuList (groups[0]), juce::String ("0-5,12-17"));
            expectEquals (formatCpuList (groups[1]), juce::String ("6-11,18-23"));
        }

        const auto placement = chooseWorkerCpus (groups, everything (24));
        expectEquals (formatCpuList (placement.cpus), juce::String ("6-11,18-23"));
        expectEquals (describe (placement), juce::String ("CPUs 6-11,18-23 (last cache group)"));

        // The order the directories came in does not matter.
        auto shuffled = ryzen7900xSharedLists();
        std::reverse (shuffled.begin(), shuffled.end());
        expectEquals (formatCpuList (chooseWorkerCpus (parseCacheGroups (shuffled, "0-23"), everything (24)).cpus), juce::String ("6-11,18-23"));

        // A missing online file filters nothing.
        expectEquals (formatCpuList (chooseWorkerCpus (parseCacheGroups (ryzen7900xSharedLists(), {}), everything (24)).cpus),
                      juce::String ("6-11,18-23"));

        // Nobody has to list a group once per CPU: one entry per group is the same thing.
        expectEquals (formatCpuList (chooseWorkerCpus (parseCacheGroups ({ "0-5,12-17", "6-11,18-23" }, "0-23"), everything (24)).cpus),
                      juce::String ("6-11,18-23"));
    }

    void testSingleGroup()
    {
        // A six-core with one CCD: every CPU shares the one L3.
        std::vector<juce::String> lists (12, juce::String ("0-11\n"));
        const auto groups = parseCacheGroups (lists, "0-11\n");
        expectEquals ((int) groups.size(), 1);

        const auto placement = chooseWorkerCpus (groups, everything (12));
        expect (placement.cpus.empty());
        expectEquals (describe (placement), juce::String ("no pinning (single cache group)"));
    }

    void testThreeGroups()
    {
        std::vector<juce::String> lists;

        for (int cpu = 0; cpu < 24; ++cpu)
            lists.push_back ((cpu % 12) < 4 ? "0-3,12-15\n" : ((cpu % 12) < 8 ? "4-7,16-19\n" : "8-11,20-23\n"));

        const auto groups = parseCacheGroups (lists, "0-23\n");
        expectEquals ((int) groups.size(), 3);

        const auto placement = chooseWorkerCpus (groups, everything (24));
        expectEquals (describe (placement), juce::String ("CPUs 8-11,20-23 (last cache group)"));
    }

    void testGarbage()
    {
        // Nothing readable at all (a container without /sys/devices/system/cpu, a kernel that does not say).
        for (const auto& lists : { std::vector<juce::String> {},
                                   std::vector<juce::String> { "", "", "" },
                                   std::vector<juce::String> { "garbage", "1,,2", "7-3", "\n" } })
        {
            const auto groups = parseCacheGroups (lists, "0-23");
            expect (groups.empty());

            const auto placement = chooseWorkerCpus (groups, everything (24));
            expect (placement.cpus.empty());
            expectEquals (describe (placement), juce::String ("no pinning (cache topology unknown)"));
        }

        // Some readable entries among unreadable ones: the readable ones count.
        const auto mixed = parseCacheGroups ({ "garbage", "0-5,12-17", "", "6-11,18-23", "x-y" }, "0-23");
        expectEquals ((int) mixed.size(), 2);

        // Groups that overlap without being equal are no topology at all.
        expect (parseCacheGroups ({ "0-5", "3-8" }, "0-23").empty());

        // A malformed online list is as good as none.
        expectEquals ((int) parseCacheGroups (ryzen7900xSharedLists(), "garbage").size(), 2);

        // chooseWorkerCpus copes with what it is given.
        expect (chooseWorkerCpus ({}, {}).cpus.empty());
        expect (chooseWorkerCpus ({ CpuList {}, CpuList {} }, everything (4)).cpus.empty());
        expectEquals (describe (chooseWorkerCpus ({ CpuList {}, CpuList { 3 } }, everything (4))), juce::String ("no pinning (single cache group)"));
    }

    void testOffline()
    {
        // SMT off in the firmware: the siblings 12-23 are offline, their entries are gone, and the survivors' lists may still
        // name them.
        const auto smtOff = parseCacheGroups (ryzen7900xSharedLists(), "0-11\n");
        expectEquals ((int) smtOff.size(), 2);
        expectEquals (formatCpuList (chooseWorkerCpus (smtOff, everything (12)).cpus), juce::String ("6-11"));

        // The whole second CCD is offline: one group is left.
        const auto oneCcd = parseCacheGroups (ryzen7900xSharedLists(), "0-5,12-17\n");
        expectEquals ((int) oneCcd.size(), 1);
        expectEquals (describe (chooseWorkerCpus (oneCcd, everything (24))), juce::String ("no pinning (single cache group)"));

        // Part of the last group is offline.
        const auto partly = parseCacheGroups (ryzen7900xSharedLists(), "0-9,12-21\n");
        expectEquals (formatCpuList (chooseWorkerCpus (partly, everything (24)).cpus), juce::String ("6-9,18-21"));
    }

    void testAffinity()
    {
        const auto groups = parseCacheGroups (ryzen7900xSharedLists(), "0-23\n");

        // taskset -c 0-5,12-17 headroom: nothing of the last group is allowed, so nothing is done.
        const auto first = chooseWorkerCpus (groups, join (range (0, 5), range (12, 17)));
        expect (first.cpus.empty());
        expectEquals (describe (first), juce::String ("no pinning (the process may not run on the last cache group)"));

        // ... or nothing is known about it.
        expect (chooseWorkerCpus (groups, {}).cpus.empty());

        // Part of the last group is allowed: that part, and only that.
        const auto part = chooseWorkerCpus (groups, { 23, 7, 6, 18, 2 });
        expectEquals (describe (part), juce::String ("CPUs 6-7,18,23 (last cache group)"));

        // All of it and more: the whole group.
        expectEquals (formatCpuList (chooseWorkerCpus (groups, everything (32)).cpus), juce::String ("6-11,18-23"));
    }

    void testDisabled()
    {
        expect (isPinningDisabled ("1"));
        expect (isPinningDisabled (" 1\n"));
        expect (! isPinningDisabled (""));
        expect (! isPinningDisabled ("0"));
        expect (! isPinningDisabled ("10"));
        expect (! isPinningDisabled ("true"));
        expect (! isPinningDisabled ("yes"));
    }

   #if JUCE_LINUX
    // ---- Linux: the real thing ---------------------------------------------------------------------
    static CpuList affinityOf (pid_t thread)
    {
        cpu_set_t set;
        CPU_ZERO (&set);
        CpuList cpus;

        if (sched_getaffinity (thread, sizeof (set), &set) == 0)
            for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
                if (CPU_ISSET (cpu, &set))
                    cpus.push_back (cpu);

        return cpus;
    }

    struct Pinned
    {
        CpuList before, applied, after;
    };

    // pinCurrentThread() on a thread of its own (so that the test runner's threads keep their CPUs).
    static Pinned pinOnAThread()
    {
        Pinned result;

        std::thread ([&]
        {
            result.before = affinityOf (0);
            result.applied = pinCurrentThread();
            result.after = affinityOf (0);
        }).join();

        return result;
    }

    void testPinThread()
    {
        const auto pinned = pinOnAThread();
        expect (! pinned.before.empty());

        if (pinned.applied.empty())
        {
            expect (pinned.after == pinned.before, "not pinned: the thread keeps its CPUs");
            logMessage ("this machine: no pinning (one cache group, unknown topology or a restricted process)");
        }
        else
        {
            expect (pinned.after == pinned.applied, "pinned: the thread is on exactly the CPUs it was told");
            expect (std::includes (pinned.before.begin(), pinned.before.end(), pinned.applied.begin(), pinned.applied.end()),
                    "and they were CPUs the process may use");
            logMessage ("this machine: pinned to " + formatCpuList (pinned.applied));
        }

        // The escape hatch.
        setenv ("HEADROOM_NO_PIN", "1", 1);
        const auto notPinned = pinOnAThread();
        unsetenv ("HEADROOM_NO_PIN");

        expect (notPinned.applied.empty(), "HEADROOM_NO_PIN=1: nothing is applied");
        expect (notPinned.after == notPinned.before, "HEADROOM_NO_PIN=1: the thread keeps its CPUs");

        // The thread that ran the test was never touched: a new thread starts with its CPUs, and they are what it had before.
        expect (pinned.before == affinityOf (0), "the calling thread keeps its CPUs");
    }

    // The call that does the pinning, on a machine of any size (a CI runner has one cache group, so pinCurrentThread() itself
    // does nothing there): the thread ends up on exactly what it was given, and refusals leave it alone.
    void testSetCpus()
    {
        CpuList before, afterOne, afterTwo, afterRefused, inherited;
        bool okOne = false, okTwo = true, refusedEmpty = false, refusedHigh = false;
        juce::String error;

        std::thread ([&]
        {
            before = affinityOf (0);

            if (before.empty())
                return;

            okOne = setCurrentThreadCpus ({ before.back() }, error);
            afterOne = affinityOf (0);
            std::thread ([&] { inherited = affinityOf (0); }).join();

            if (before.size() >= 2)
            {
                okTwo = setCurrentThreadCpus ({ before.front(), before.back() }, error);
                afterTwo = affinityOf (0);
            }

            refusedEmpty = ! setCurrentThreadCpus ({}, error);
            refusedHigh = ! setCurrentThreadCpus ({ 5000, -1 }, error);     // beyond cpu_set_t, and no CPU at all
            afterRefused = affinityOf (0);
        }).join();

        expect (! before.empty());
        expect (okOne && afterOne == CpuList { before.back() }, "one CPU: the thread is on that CPU only");
        expect (inherited == afterOne, "a thread started by a pinned thread starts on its CPUs");

        if (before.size() >= 2)
            expect (okTwo && afterTwo == CpuList ({ before.front(), before.back() }), "two CPUs: exactly those");

        expect (refusedEmpty && refusedHigh, "an empty list and numbers that fit nowhere are refused");
        expect (afterRefused == (before.size() >= 2 ? afterTwo : afterOne), "a refusal leaves the thread where it was");
    }

    static std::set<int> threadsOfThisProcess()
    {
        std::set<int> threads;

        for (const auto& entry : juce::File ("/proc/self/task").findChildFiles (juce::File::findDirectories, false, "*"))
            threads.insert (entry.getFileName().getIntValue());

        return threads;
    }

    void testNoiseRemovalThreads()
    {
        if (! dfn::isAvailable (dfn::Model::best))
        {
            logMessage ("the DeepFilterNet models are not here: skipped");
            return;
        }

        const auto reference = pinOnAThread();          // where a new thread of this process is expected to end up
        const auto expected = reference.applied.empty() ? reference.before : reference.applied;

        // Light first, then Best: the loader thread builds the network, which starts the worker thread.
        const auto threadsBefore = threadsOfThisProcess();
        NoiseRemoval nr;
        auto* mode = nr.apvts.getParameter ("mode");
        mode->setValueNotifyingHost (mode->convertTo0to1 ((float) (int) NoiseRemoval::Mode::light));
        nr.prepareToPlay (48000.0, 480);
        mode->setValueNotifyingHost (mode->convertTo0to1 ((float) (int) NoiseRemoval::Mode::best));
        expect (nr.waitUntilReady (30000), "Best became ready");

        std::vector<int> created;

        for (auto thread : threadsOfThisProcess())
            if (threadsBefore.count (thread) == 0)
                created.push_back (thread);

        expect (created.size() >= 2, "a loader and a worker thread exist (" + juce::String ((int) created.size()) + " new threads)");

        // The threads that are not where they should be (one that has already gone has no affinity to read and does not count).
        auto misplaced = [&]
        {
            std::vector<int> wrong;

            for (auto thread : created)
                if (const auto where = affinityOf ((pid_t) thread); ! where.empty() && where != expected)
                    wrong.push_back (thread);

            return wrong;
        };

        // The worker pins itself when it starts running, which can be a moment after the network is ready.
        const auto deadline = juce::Time::getMillisecondCounter() + 3000;

        while (! misplaced().empty() && juce::Time::getMillisecondCounter() < deadline)
            juce::Thread::sleep (10);

        for (auto thread : misplaced())
            expect (false, "thread " + juce::String (thread) + " is on " + formatCpuList (affinityOf ((pid_t) thread))
                           + ", expected " + formatCpuList (expected));

        nr.releaseResources();
    }
   #endif
};

static CpuPlacementTests cpuPlacementTests;
}
