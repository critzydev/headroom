// Unit tests for SingleInstance: election, the command channel, and the real headroom binary as primary and as secondary.
#include "Juce.h"

#include "app/SingleInstance.h"

#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <vector>

// JUCE 8+ leaves MessageManager::runDispatchLoopUntil() out unless JUCE_MODAL_LOOPS_PERMITTED is on, and we don't want that
// set for the whole app. The tests only need to let async callbacks run, so they call the platform pump directly (see
// PluginTests.cpp).
namespace juce::detail
{
bool dispatchNextMessageOnSystemQueue (bool returnIfNoPendingMessages);
}

namespace hr
{
namespace
{
    // The processes under test are the real headroom executable, which sits next to headroom_tests in the build directory.
    juce::File headroomExecutable()
    {
        const auto self = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
       #if JUCE_WINDOWS
        return self.getSiblingFile ("headroom.exe");
       #else
        return self.getSiblingFile ("headroom");
       #endif
    }

    // Runs the message loop (the tests' main thread is the message thread) until `condition` holds.
    template <typename Condition>
    bool waitUntil (Condition&& condition, int timeoutMs)
    {
        const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;

        while (! condition())
        {
            if (juce::Time::getMillisecondCounter() > end)
                return false;

            const auto until = juce::Time::getMillisecondCounter() + 20;

            while (juce::Time::getMillisecondCounter() < until)
                if (! juce::detail::dispatchNextMessageOnSystemQueue (true))
                    juce::Thread::sleep (2);
        }

        return true;
    }

    // A unique name per run, so parallel test runs (CI jobs on one machine) and a real Headroom cannot collide.
    juce::String uniqueName()
    {
        return "headroom-test-" + juce::Uuid().toString().substring (0, 12);
    }

    // The rest of the process's life is the test's business: whatever is still running when the test ends is killed.
    struct Process
    {
        ~Process()
        {
            if (child.isRunning())
                child.kill();
        }

        bool start (std::initializer_list<juce::String> arguments)
        {
            juce::StringArray full;
            full.add (headroomExecutable().getFullPathName());

            for (const auto& argument : arguments)
                full.add (argument);

            return child.start (full, 0);
        }

        // Exit code, or -1 if it did not finish in time (it is killed then).
        int finish (int timeoutMs)
        {
            if (! child.waitForProcessToFinish (timeoutMs))
            {
                child.kill();
                return -1;
            }

            return (int) child.getExitCode();
        }

        juce::ChildProcess child;
    };

    // What the test child writes about itself, one word per line.
    struct EventFile
    {
        EventFile() : file (juce::File::createTempFile (".events")) {}
        ~EventFile()    { file.deleteFile(); }

        int count (const juce::String& word) const
        {
            juce::StringArray lines;
            lines.addLines (file.loadFileAsString());

            int found = 0;

            for (const auto& line : lines)
                if (line == word)
                    ++found;

            return found;
        }

        bool waitFor (const juce::String& word, int howMany, int timeoutMs) const
        {
            const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;

            while (count (word) < howMany)
            {
                if (juce::Time::getMillisecondCounter() > end)
                    return false;

                juce::Thread::sleep (20);
            }

            return true;
        }

        juce::File file;
    };

    // Removes what a killed primary leaves behind (Linux), whatever the test did.
    struct Cleanup
    {
        explicit Cleanup (const juce::String& nameToClean) : name (nameToClean) {}
        ~Cleanup()      { SingleInstance::removeLeftovers (name); }

        juce::String name;
    };

    // HEADROOM_INSTANCE for this process and the processes it starts.
    struct ScopedInstanceVariable
    {
        explicit ScopedInstanceVariable (const juce::String& value)     { set (value.toRawUTF8()); }
        ~ScopedInstanceVariable()                                       { set (nullptr); }

    private:
        static void set (const char* value)
        {
           #if JUCE_WINDOWS
            _putenv_s ("HEADROOM_INSTANCE", value != nullptr ? value : "");
           #else
            if (value != nullptr)
                ::setenv ("HEADROOM_INSTANCE", value, 1);
            else
                ::unsetenv ("HEADROOM_INSTANCE");
           #endif
        }
    };

    // Elections right after a process died can take a moment on some systems to see the lock gone.
    bool becomesPrimaryWithin (SingleInstance& instance, int timeoutMs)
    {
        const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;

        while (! instance.tryBecomePrimary())
        {
            if (juce::Time::getMillisecondCounter() > end)
                return false;

            juce::Thread::sleep (50);
        }

        return true;
    }
}

class SingleInstanceTests final : public juce::UnitTest
{
public:
    SingleInstanceTests() : juce::UnitTest ("Single instance", "headroom") {}

    void runTest() override
    {
        beginTest ("the default name is per user and safe to use as a file name");
        {
            const auto defaultName = SingleInstance::defaultName();
            expect (defaultName.startsWith ("headroom-") && defaultName.length() > 9);
            expect (defaultName.containsOnly ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-"), defaultName);

            {
                const ScopedInstanceVariable instance ("mine");
                expectEquals (SingleInstance::defaultName(), defaultName + "-mine");
            }

            expectEquals (SingleInstance::defaultName(), defaultName);
        }

        beginTest ("commands are answered and shown/quit reach the message thread");
        {
            const auto instanceName = uniqueName();
            const Cleanup cleanup (instanceName);

            SingleInstance primary (instanceName);
            SingleInstance secondary (instanceName);

            expect (! secondary.sendShowToPrimary (300), "nobody answers before there is a primary");
            expect (! primary.isPrimary());
            expect (primary.tryBecomePrimary());
            expect (primary.isPrimary());
            expect (primary.tryBecomePrimary(), "asking again is fine");

            int shown = 0;
            bool quitAsked = false;
            primary.onShowRequested = [&] { ++shown; };
            primary.onQuitRequested = [&] { quitAsked = true; };

            expectEquals (secondary.sendCommand ("ping", 5000), juce::String ("pong"));
            expectEquals (secondary.sendCommand ("PING", 5000), juce::String ("pong"), "commands are not case sensitive");
            expectEquals (secondary.sendCommand ("frobnicate", 5000), juce::String ("unknown"));

            expect (secondary.sendShowToPrimary (5000));
            expect (waitUntil ([&] { return shown == 1; }, 5000), "show reaches the message thread");

            bool allAnswered = true;

            for (int i = 0; i < 25; ++i)
                allAnswered = allAnswered && secondary.sendCommand ("ping", 5000) == "pong";

            expect (allAnswered, "any number of consecutive secondaries is served");
            expectEquals (shown, 1, "only show asked for a window");

            expectEquals (secondary.sendCommand ("quit", 5000), juce::String ("ok"));
            expect (waitUntil ([&] { return quitAsked; }, 5000), "quit reaches the message thread");

            primary.stopListening();
            expect (primary.isPrimary(), "still the primary: the lock is kept while it shuts down");
            expectEquals (secondary.sendCommand ("ping", 500), juce::String(), "but nobody answers any more");
        }

        beginTest ("a sender that dies mid-message or writes junk does not hurt the next one");
        {
            const auto instanceName = uniqueName();
            const Cleanup cleanup (instanceName);

            SingleInstance primary (instanceName);
            SingleInstance secondary (instanceName);
            expect (primary.tryBecomePrimary());

            int shown = 0;
            primary.onShowRequested = [&] { ++shown; };

            const auto send = [&] (const juce::String& text)
            {
                juce::NamedPipe raw;
                const bool opened = raw.openExisting (SingleInstance::pipeNameFor (instanceName));
                expect (opened, "the pipe can be opened");

                if (opened)
                    expect (raw.write (text.toRawUTF8(), (int) text.getNumBytesAsUTF8(), 2000) == (int) text.getNumBytesAsUTF8());
            };      // the pipe is closed here: for the primary, the sender is gone

            send ("HR1 ab");                                                    // half a request, then the sender is gone
            send (juce::String::repeatedString ("x", 400));                     // far too long for a line, no end
            send ("\nnot for us\n");                                            // no prefix
            send ("\nHR1 nothex show\n");                                       // prefix, but not a nonce
            send ("\nHR1 c0ffee01 ping\n");                                     // a whole request whose sender never reads the answer
            send ("\nHR1 c0ffee02 show\n");                                     // ...and one that must still be carried out

            expect (waitUntil ([&] { return shown == 1; }, 5000), "the request after the junk still arrives");
            expectEquals (secondary.sendCommand ("ping", 5000), juce::String ("pong"), "and the next sender is answered, not the old answers");
            expectEquals (shown, 1);
        }

        const auto executable = headroomExecutable();

        if (! executable.existsAsFile())
        {
            beginTest ("the headroom executable is needed");
            expect (false, "headroom executable not found next to the tests: " + executable.getFullPathName());
            return;
        }

        beginTest ("two processes: election, ping, show, quit");
        {
            const auto instanceName = uniqueName();
            const Cleanup cleanup (instanceName);
            EventFile events;

            Process first;
            expect (first.start ({ "--dev-single-instance-child", instanceName, events.file.getFullPathName() }));
            expect (events.waitFor ("primary", 1, 20000), "the child becomes the primary");

            SingleInstance mine (instanceName);
            expect (! mine.tryBecomePrimary(), "a second election under the same name fails while the child lives");
            expect (! mine.isPrimary());
            expect (mine.isPrimaryRunning());

            expectEquals (mine.sendCommand ("ping", 5000), juce::String ("pong"));
            expectEquals (mine.sendCommand ("frobnicate", 5000), juce::String ("unknown"));
            expect (mine.sendShowToPrimary (5000));
            expect (events.waitFor ("show", 1, 5000), "show reaches the child's message thread");

            {
                EventFile duplicateEvents;
                Process duplicate;
                expect (duplicate.start ({ "--dev-single-instance-child", instanceName, duplicateEvents.file.getFullPathName() }));
                expectEquals (duplicate.finish (20000), 3, "a second process cannot become the primary and says so");
                expectEquals (duplicateEvents.count ("not-primary"), 1);
            }

            expectEquals (mine.sendCommand ("quit", 5000), juce::String ("ok"));
            expect (first.child.waitForProcessToFinish (3000), "quit ends the child within 3 s");
            expectEquals ((int) first.child.getExitCode(), 0);
            expectEquals (events.count ("quit"), 1);

            expect (! mine.isPrimaryRunning(), "the lock is gone with it");
            expectEquals (mine.sendCommand ("ping", 300), juce::String(), "and so is the pipe");

            SingleInstance next (instanceName);
            expect (becomesPrimaryWithin (next, 5000), "after a clean quit the next process can become the primary");
        }

        beginTest ("a primary that was killed leaves no stale lock or pipe in the way");
        {
            const auto instanceName = uniqueName();
            const Cleanup cleanup (instanceName);
            EventFile events;

            Process victim;
            expect (victim.start ({ "--dev-single-instance-child", instanceName, events.file.getFullPathName() }));
            expect (events.waitFor ("primary", 1, 20000), "the child becomes the primary");

            SingleInstance probe (instanceName);
            expectEquals (probe.sendCommand ("ping", 5000), juce::String ("pong"));

            victim.child.kill();
            expect (victim.child.waitForProcessToFinish (10000), "the child is dead");

            SingleInstance next (instanceName);
            expect (becomesPrimaryWithin (next, 5000), "a new primary can be elected after a hard kill");
            expectEquals (probe.sendCommand ("ping", 5000), juce::String ("pong"), "and it uses the pipe the dead one left");

            // Another real process now finds the lock taken again.
            EventFile secondEvents;
            Process late;
            expect (late.start ({ "--dev-single-instance-child", instanceName, secondEvents.file.getFullPathName() }));
            expectEquals (late.finish (20000), 3);
        }

        beginTest ("headroom --show, --background and --quit against a running primary");
        {
            const ScopedInstanceVariable instance (uniqueName().fromLastOccurrenceOf ("-", false, false));
            const auto instanceName = SingleInstance::defaultName();       // what the processes below will use
            const Cleanup cleanup (instanceName);
            EventFile events;

            Process primary;
            expect (primary.start ({ "--dev-single-instance-child", instanceName, events.file.getFullPathName() }));
            expect (events.waitFor ("primary", 1, 20000), "the child becomes the primary");

            // --background with a primary running: nothing to do, exits 0 quietly.
            {
                Process background;
                expect (background.start ({ "--background" }));
                expectEquals (background.finish (20000), 0);
                expectEquals (events.count ("show"), 0, "--background does not ask for the window");
            }

            // --show and --demo hand over to the primary and exit 0, quickly.
            for (const char* flag : { "--show", "--demo" })
            {
                const auto before = events.count ("show");
                const auto started = juce::Time::getMillisecondCounter();

                Process secondary;
                expect (secondary.start ({ flag }));
                expectEquals (secondary.finish (20000), 0, flag);
                expect (juce::Time::getMillisecondCounter() - started < 10000, juce::String (flag) + " exits quickly");
                expect (events.waitFor ("show", before + 1, 5000), juce::String (flag) + " reached the primary");
            }

            // No flag at all is the same as --show. Several at once (a double click) are all served.
            {
                const auto before = events.count ("show");
                std::vector<std::unique_ptr<Process>> launches;

                for (int i = 0; i < 4; ++i)
                {
                    launches.push_back (std::make_unique<Process>());
                    expect (launches.back()->start ({}));
                }

                for (auto& launch : launches)
                    expectEquals (launch->finish (30000), 0);

                expect (events.waitFor ("show", before + 4, 10000), "four launches at once all reach the primary");
            }

            // --quit: the primary ends, and --quit itself exits 0 once it is gone.
            {
                Process quit;
                expect (quit.start ({ "--quit" }));
                expectEquals (quit.finish (30000), 0);
                expect (primary.child.waitForProcessToFinish (3000), "the primary ended");
                expectEquals ((int) primary.child.getExitCode(), 0);
                expectEquals (events.count ("quit"), 1);
            }

            // --quit with nothing running is fine too.
            {
                Process quit;
                expect (quit.start ({ "--quit" }));
                expectEquals (quit.finish (30000), 0);
            }
        }
    }
};

static SingleInstanceTests singleInstanceTests;
}
