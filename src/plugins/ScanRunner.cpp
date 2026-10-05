// ScanRunner implementation: worker process management, frame file polling, crash / timeout handling.
#include "plugins/ScanRunner.h"

#include "app/Log.h"

#include <string>

#if JUCE_LINUX
 #include <unistd.h>
#endif

namespace hr
{
namespace
{
    double nowMs()
    {
        return juce::Time::getMillisecondCounterHiRes();
    }

    juce::String currentProcessId()
    {
       #if JUCE_LINUX
        return juce::String ((int) ::getpid());
       #else
        return "0";     // the worker only uses it to notice a vanished parent (POSIX)
       #endif
    }

    juce::File workerExecutableFor (const ScanRunner::Settings& settings)
    {
        return settings.workerExecutable != juce::File() ? settings.workerExecutable
                                                          : juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    }

    // Turns the <LIST> a worker printed for one file into descriptions. Garbage yields an empty list, never a crash.
    void parseDescriptions (const juce::String& xmlText, ScanRunner::Found& found)
    {
        if (auto xml = juce::XmlDocument::parse (xmlText))
        {
            for (auto* child : xml->getChildIterator())
            {
                auto description = std::make_unique<juce::PluginDescription>();

                if (description->loadFromXml (*child))
                    found.add (std::move (description));
            }
        }
    }

    void parseFileList (const juce::String& xmlText, juce::StringArray& files)
    {
        if (auto xml = juce::XmlDocument::parse (xmlText))
            for (auto* child : xml->getChildWithTagNameIterator ("FILE"))
                if (const auto id = child->getStringAttribute ("id"); id.isNotEmpty())
                    files.add (id);
    }
}

// ---------------------------------------------------------------------------------------------------
// One worker process, from launch to "done" / death / timeout.
// ---------------------------------------------------------------------------------------------------

ScanRunner::Session ScanRunner::runSession (const std::vector<scan::WireJob>& jobs,
                                            const std::function<void (size_t)>& onBegin,
                                            const std::function<void (size_t, char, const juce::String&)>& onResult)
{
    Session session;

    juce::TemporaryFile jobFile (".hrjobs"), outFile (".hrout");

    if (! jobFile.getFile().replaceWithText (scan::encodeJobs (jobs, settings.testHooks))
         || ! outFile.getFile().create().wasOk())
    {
        session.detail = "Could not write the scan job files";
        return session;
    }

    const auto executable = workerExecutableFor (settings);

    if (! executable.existsAsFile())
    {
        session.detail = "Scanner executable not found: " + executable.getFullPathName();
        return session;
    }

    juce::StringArray args;
    args.add (executable.getFullPathName());
    args.add (scan::kWorkerFlag);
    args.add (jobFile.getFile().getFullPathName());
    args.add (outFile.getFile().getFullPathName());
    args.add (currentProcessId());

    juce::ChildProcess process;

    // streamFlags 0: the worker's stdout / stderr go to the null device. Nothing to read, and a plugin that prints a
    // lot can never block on a full pipe.
    if (! process.start (args, 0))
    {
        session.detail = "Could not start the scanner process";
        return session;
    }

    juce::FileInputStream frames (outFile.getFile());

    if (! frames.openedOk())
    {
        process.kill();
        process.waitForProcessToFinish (3000);
        session.detail = "Could not read the scan output";
        return session;
    }

    const auto killWorker = [&]
    {
        process.kill();
        process.waitForProcessToFinish (3000);
    };

    std::string pending;                    // bytes of a not yet complete line
    juce::MemoryBlock chunk (64 * 1024);
    double lastActivity = nowMs();
    double lastPoll = lastActivity;
    double itemStarted = 0.0;               // 0 = no job in flight
    bool done = false;

    const auto handleFrame = [&] (const juce::String& line)
    {
        lastActivity = nowMs();

        if (line.isEmpty())
            return;

        const auto kind = (char) line[0];

        if (kind == 'D')
        {
            done = true;
            return;
        }

        const auto fields = scan::splitFields (line, 3);
        const int index = fields.size() > 1 ? fields[1].getIntValue() : -1;

        if (kind == 'B' && index == (int) session.completed)
        {
            session.inProgress = index;
            itemStarted = lastActivity;

            if (onBegin)
                onBegin ((size_t) index);
        }
        else if ((kind == 'R' || kind == 'F') && index == (int) session.completed)
        {
            session.inProgress = -1;
            itemStarted = 0.0;
            ++session.completed;

            if (onResult)
                onResult ((size_t) index, kind, fields.size() > 2 ? fields[2] : juce::String());
        }
        // Anything else (the "H" hello, a frame we don't know) only counts as a sign of life.
    };

    for (;;)
    {
        if (wantsExit())
        {
            killWorker();
            session.end = Session::End::aborted;
            break;
        }

        // Sample "alive" before reading: if the worker died, everything it wrote before dying is in the file already.
        const bool alive = process.isRunning();

        for (;;)
        {
            const auto numRead = frames.read (chunk.getData(), (int) chunk.getSize());

            if (numRead <= 0)
                break;

            pending.append (static_cast<const char*> (chunk.getData()), (size_t) numRead);
        }

        for (auto newline = pending.find ('\n'); newline != std::string::npos; newline = pending.find ('\n'))
        {
            handleFrame (juce::String::fromUTF8 (pending.data(), (int) newline));
            pending.erase (0, newline + 1);
        }

        if (done)
        {
            if (! process.waitForProcessToFinish (3000))
                killWorker();       // it did all its jobs; a plugin thread keeping the process alive is not our problem

            session.end = Session::End::finished;
            break;
        }

        if (! alive)
        {
            session.end = Session::End::workerDied;
            session.detail = "the scanner process stopped";
            break;
        }

        const auto now = nowMs();

        // The machine slept (or we were stopped in a debugger): that time is nobody's fault, least of all the plugin's.
        if (const auto gap = now - lastPoll; gap > 5000.0)
        {
            lastActivity += gap;

            if (itemStarted > 0.0)
                itemStarted += gap;
        }

        lastPoll = now;
        const bool inFlight = itemStarted > 0.0;

        if (inFlight ? (now - itemStarted > settings.itemTimeoutMs) : (now - lastActivity > settings.startupTimeoutMs))
        {
            killWorker();
            session.end = Session::End::timedOut;
            session.detail = inFlight ? "the scanner took too long" : "the scanner did not start";
            break;
        }

        juce::Thread::sleep (juce::jmax (1, settings.pollMs));
    }

    return session;
}

// ---------------------------------------------------------------------------------------------------
// Step 1: search
// ---------------------------------------------------------------------------------------------------

ScanRunner::SearchResult ScanRunner::search (const std::vector<SearchRequest>& requests)
{
    SearchResult result;
    std::vector<scan::WireJob> jobs;

    for (const auto& request : requests)
    {
        scan::WireJob job;
        job.kind = scan::WireJob::Kind::search;
        job.format = request.format;
        job.recursive = request.recursive;
        job.argument = request.paths.toString();

        if (! scan::isWireSafe (job.argument))
            job.argument.clear();

        jobs.push_back (std::move (job));
    }

    if (jobs.empty())
    {
        result.complete = true;
        return result;
    }

    const auto session = runSession (jobs, {},
                                     [&] (size_t index, char kind, const juce::String& xml)
                                     {
                                         if (kind != 'F' || index >= requests.size())
                                             return;

                                         auto& files = result.files[requests[index].format];
                                         parseFileList (xml, files);
                                         files.removeDuplicates (false);
                                     });

    result.complete = session.end == Session::End::finished;

    if (! result.complete && session.end != Session::End::aborted)
        result.error = session.detail;

    return result;
}

// ---------------------------------------------------------------------------------------------------
// Step 2: scan
// ---------------------------------------------------------------------------------------------------

ScanRunner::ScanReport ScanRunner::scan (const std::vector<ScanJob>& jobs, const Callbacks& callbacks)
{
    ScanReport report;

    size_t next = 0;                // first job without an outcome
    int isolated = -1;              // job being retried alone in a fresh worker after a crash
    int sessionsWithoutProgress = 0;

    while (next < jobs.size())
    {
        if (wantsExit())
        {
            report.aborted = true;
            return report;
        }

        const size_t base = next;
        const size_t end = isolated == (int) base ? base + 1 : jobs.size();

        std::vector<scan::WireJob> wire;

        for (size_t i = base; i < end; ++i)
        {
            scan::WireJob job;
            job.kind = scan::WireJob::Kind::scan;
            job.format = jobs[i].format;
            job.argument = scan::isWireSafe (jobs[i].identifier) ? jobs[i].identifier : juce::String();
            wire.push_back (std::move (job));
        }

        const auto session = runSession (wire,
                                         [&] (size_t i)
                                         {
                                             if (callbacks.started)
                                                 callbacks.started (base + i, jobs[base + i]);
                                         },
                                         [&] (size_t i, char kind, const juce::String& xml)
                                         {
                                             if (kind != 'R' || ! callbacks.finished)
                                                 return;

                                             Found found;
                                             parseDescriptions (xml, found);
                                             callbacks.finished (base + i, jobs[base + i], Outcome::scanned, found);
                                         });

        next = base + session.completed;

        if (session.completed > 0)
            sessionsWithoutProgress = 0;

        switch (session.end)
        {
            case Session::End::finished:
                isolated = -1;

                if (session.completed < wire.size())
                {
                    report.error = "The plugin scanner stopped early";      // cannot happen with our own worker; never loop on it
                    return report;
                }

                break;

            case Session::End::aborted:
                report.aborted = true;
                return report;

            case Session::End::launchFailed:
                report.error = session.detail;
                return report;

            case Session::End::workerDied:
            case Session::End::timedOut:
            {
                if (session.inProgress >= 0)
                {
                    const auto culprit = base + (size_t) session.inProgress;
                    const bool timedOut = session.end == Session::End::timedOut;

                    if (timedOut || isolated == (int) culprit)
                    {
                        // A hang is not retried (30 s again would help nobody); a crash that repeats in a clean worker
                        // is the file's fault.
                        Log::warn ("Plugin scan: " + jobs[culprit].identifier + (timedOut ? " timed out" : " crashed the scanner"));

                        Found nothing;

                        if (callbacks.finished)
                            callbacks.finished (culprit, jobs[culprit], timedOut ? Outcome::timedOut : Outcome::crashed, nothing);

                        next = culprit + 1;
                        isolated = -1;
                    }
                    else
                    {
                        Log::info ("Plugin scan: retrying " + jobs[culprit].identifier + " on its own");
                        isolated = (int) culprit;
                    }
                }
                else if (session.completed == 0 && ++sessionsWithoutProgress >= 2)
                {
                    // Twice in a row it never got going: the executable is broken or missing, not a plugin.
                    report.error = "The plugin scanner does not start (" + session.detail + ")";
                    return report;
                }

                break;
            }
        }
    }

    report.completed = true;
    return report;
}

// ---------------------------------------------------------------------------------------------------
// KnownPluginList adapter
// ---------------------------------------------------------------------------------------------------

bool OutOfProcessScanner::findPluginTypesFor (juce::AudioPluginFormat& format, juce::OwnedArray<juce::PluginDescription>& result,
                                              const juce::String& fileOrIdentifier)
{
    ScanRunner runner (settings);
    runner.setShouldExit ([this] { return shouldExit(); });

    bool survived = true;

    ScanRunner::Callbacks callbacks;
    callbacks.finished = [&] (size_t, const ScanJob&, ScanRunner::Outcome outcome, ScanRunner::Found& found)
    {
        survived = outcome == ScanRunner::Outcome::scanned;

        while (! found.isEmpty())
            result.add (found.removeAndReturn (0));
    };

    const auto report = runner.scan ({ { format.getName(), fileOrIdentifier } }, callbacks);

    // "Could not run the scanner at all" is our problem, not the plugin's: don't blacklist it for that.
    return survived && report.error.isEmpty();
}
}
