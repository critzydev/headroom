// The out-of-process plugin scan worker (`--scan-worker`), plus the two developer command-line tools.
#include "plugins/ScanWorker.h"

#include "plugins/CrashGuard.h"
#include "plugins/PluginManager.h"
#include "plugins/ScanProtocol.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <thread>

#if JUCE_WINDOWS
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #ifdef _DEBUG
  #include <crtdbg.h>
 #endif
#elif JUCE_LINUX
 #include <unistd.h>
#endif

namespace hr::PluginCli
{
namespace
{
    // The parent gives up on a file after 30 s (it kills us). If nobody did after this long, the parent is gone.
    constexpr double kSelfDestructMs = 120000.0;

    std::atomic<double> jobStartedAtMs { 0.0 };        // 0 = between jobs

    // The frame file (ScanProtocol.h). Plain C stdio: it shares the file with the polling parent on every OS, and
    // fflush hands each frame to the OS without waiting for the disk.
    class FrameFile
    {
    public:
        explicit FrameFile (const juce::File& file)
        {
           #if JUCE_WINDOWS
            handle = _wfopen (file.getFullPathName().toWideCharPointer(), L"ab");
           #else
            handle = std::fopen (file.getFullPathName().toRawUTF8(), "ab");
           #endif
        }

        ~FrameFile()
        {
            if (handle != nullptr)
                std::fclose (handle);
        }

        FrameFile (const FrameFile&) = delete;
        FrameFile& operator= (const FrameFile&) = delete;

        bool isOpen() const noexcept    { return handle != nullptr; }

        // One write per frame, newline included, so the parent sees whole lines or nothing.
        bool write (const juce::String& frame)
        {
            const auto line = frame + "\n";
            const auto size = line.getNumBytesAsUTF8();
            const auto written = std::fwrite (line.toRawUTF8(), 1, size, handle);
            return written == size && std::fflush (handle) == 0;
        }

    private:
        std::FILE* handle = nullptr;
    };

    // A worker must never linger: it exits when its parent goes away or when a single job has gone on far too long.
    void startWatchdog (int parentPid)
    {
        juce::ignoreUnused (parentPid);

        std::thread ([parentPid]
        {
            for (;;)
            {
                std::this_thread::sleep_for (std::chrono::milliseconds (500));

                const auto started = jobStartedAtMs.load();

                if (started > 0.0 && juce::Time::getMillisecondCounterHiRes() - started > kSelfDestructMs)
                    std::_Exit (4);

               #if JUCE_LINUX
                // Reparented to init / a subreaper = the parent died.
                if (parentPid > 0 && ::getppid() != (pid_t) parentPid)
                    std::_Exit (5);
               #endif
            }
        }).detach();
    }

    void prepareWorkerProcess (int parentPid)
    {
       #if JUCE_WINDOWS
        // A crashing plugin must end the worker quietly (the parent sees the process die), not raise a
        // "has stopped working" dialog nobody can dismiss.
        SetErrorMode (SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        _set_abort_behavior (0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        juce::Process::setPriority (juce::Process::LowPriority);
       #ifdef _DEBUG
        // We leave through _Exit without cleaning up, so the debug CRT would report everything still allocated as a leak.
        _CrtSetDbgFlag (_CrtSetDbgFlag (_CRTDBG_REPORT_FLAG) & ~_CRTDBG_LEAK_CHECK_DF);
       #endif
       #elif JUCE_LINUX
        // Scanning is background work; keep it out of the audio thread's way. (Process::setPriority does nothing here.)
        const int niceResult = ::nice (10);
        juce::ignoreUnused (niceResult);
       #endif

        startWatchdog (parentPid);
    }

    int currentPid()
    {
       #if JUCE_WINDOWS
        return (int) GetCurrentProcessId();
       #elif JUCE_LINUX
        return (int) ::getpid();
       #else
        return 0;
       #endif
    }

    juce::AudioPluginFormat* findFormat (juce::AudioPluginFormatManager& formats, const juce::String& name)
    {
        for (auto* format : formats.getFormats())
            if (format->getName() == name)
                return format;

        return nullptr;
    }

    // Deliberately misbehaving "plugins" so the parent's crash / hang handling can be tested end to end. Only
    // honoured when the job file (written by our own process) says testhooks.
    void obeyTestHook (const juce::String& identifier)
    {
        const auto what = identifier.fromFirstOccurrenceOf ("hr-test:", false, false);

        if (what == "crash")
            std::abort();

        if (what == "hang")
            for (;;)
                juce::Thread::sleep (1000);
    }

    void runJobs (const juce::File& jobFile, const juce::File& outFile)
    {
        FrameFile out (outFile);

        if (! out.isOpen())
            std::_Exit (2);

        out.write ("H\t" + juce::String (currentPid()));

        std::vector<scan::WireJob> jobs;
        bool testHooks = false;

        if (! scan::decodeJobs (jobFile.loadFileAsString(), jobs, testHooks))
            std::_Exit (3);

        juce::AudioPluginFormatManager formats;
        juce::addDefaultFormatsToManager (formats);

        for (size_t i = 0; i < jobs.size(); ++i)
        {
            const auto& job = jobs[i];
            const auto index = juce::String ((int) i);

            out.write ("B\t" + index);
            jobStartedAtMs = juce::Time::getMillisecondCounterHiRes();

            if (testHooks && job.kind == scan::WireJob::Kind::scan && job.argument.startsWith ("hr-test:"))
                obeyTestHook (job.argument);

            auto* format = findFormat (formats, job.format);

            if (job.kind == scan::WireJob::Kind::search)
            {
                juce::XmlElement files ("FILES");

                if (format != nullptr)
                {
                    try
                    {
                        for (const auto& file : format->searchPathsForPlugins (juce::FileSearchPath (job.argument), job.recursive, false))
                            files.createNewChildElement ("FILE")->setAttribute ("id", file);
                    }
                    catch (...) {}
                }

                out.write ("F\t" + index + "\t" + scan::singleLineXml (files));
            }
            else
            {
                juce::OwnedArray<juce::PluginDescription> found;

                if (format != nullptr && job.argument.isNotEmpty())
                {
                    try
                    {
                        format->findAllTypesForFile (found, job.argument);
                    }
                    catch (...)
                    {
                        found.clear();      // a plugin that throws while loading is simply unusable
                    }
                }

                juce::XmlElement list ("LIST");

                for (auto* description : found)
                    if (auto xml = description->createXml())
                        list.addChildElement (xml.release());

                out.write ("R\t" + index + "\t" + scan::singleLineXml (list));
            }

            jobStartedAtMs = 0.0;
        }

        out.write ("D");

        // Don't wind down normally: unloading some plugin DLLs at exit is exactly where they like to crash, and the
        // parent already has everything it needs.
        std::_Exit (0);
    }

    // --dev-scan-report: the real scanner, the real search paths, and a printout of what came out.
    class ScanReportRun final : private juce::Timer
    {
    public:
        explicit ScanReportRun (const juce::File& file) : outFile (file)
        {
            started = juce::Time::getMillisecondCounterHiRes();
            manager = std::make_unique<PluginManager>();
            manager->startBackgroundScan (true);
            startTimerHz (10);
        }

    private:
        void timerCallback() override
        {
            if (manager->isScanning())
                return;

            stopTimer();
            print (buildReport());

            juce::MessageManager::callAsync ([this]
            {
                delete this;
                juce::JUCEApplicationBase::quit();
            });
        }

        juce::String buildReport() const
        {
            juce::String text;
            const auto& stats = manager->getLastScanStats();

            text << "Headroom plugin scan report\n\nSearch paths\n";

            for (auto* format : manager->getFormatManager().getFormats())
                text << "  " << format->getName() << ": " << manager->getSearchPaths (*format).toString() << "\n";

            text << "\nScan: " << stats.filesChecked << " files checked, " << stats.pluginsAdded << " plugins found, "
                 << stats.crashed << " crashed, " << stats.timedOut << " timed out, "
                 << juce::String ((juce::Time::getMillisecondCounterHiRes() - started) / 1000.0, 1) << " s"
                 << (stats.error.isNotEmpty() ? "  ERROR: " + stats.error : juce::String()) << "\n";

            const auto plugins = manager->getKnownPlugins();
            text << "\nPlugins (" << plugins.size() << ")\n";

            for (const auto& p : plugins)
                text << "  " << p.pluginFormatName << "  " << p.name << "  [" << p.manufacturerName << "]  in=" << p.numInputChannels
                     << " out=" << p.numOutputChannels << (p.isInstrument ? "  instrument" : "  effect") << "\n      " << p.fileOrIdentifier << "\n";

            const auto skipped = manager->getBlacklistedFiles();
            text << "\nSkipped (" << skipped.size() << ")\n";

            for (const auto& file : skipped)
                text << "  " << file << "\n";

            return text;
        }

        void print (const juce::String& text) const
        {
            if (outFile != juce::File())
            {
                outFile.replaceWithText (text);
            }
            else
            {
                std::fputs (text.toRawUTF8(), stdout);
                std::fflush (stdout);
            }
        }

        juce::File outFile;
        double started = 0.0;
        std::unique_ptr<PluginManager> manager;
    };
}

bool runIfRequested (const juce::StringArray& args)
{
    if (args.isEmpty())
        return false;

    if (args[0] == scan::kWorkerFlag)
    {
        if (args.size() < 4)
        {
            juce::JUCEApplicationBase::quit();
            return true;
        }

        prepareWorkerProcess (args[3].getIntValue());

        // Run the jobs from the message loop rather than from inside initialise(): plugins expect a live message thread.
        juce::MessageManager::callAsync ([job = juce::File (args[1]), out = juce::File (args[2])]
        {
            runJobs (job, out);
        });

        return true;
    }

    if (args[0] == "--dev-scan-report")
    {
        const auto outFile = args.size() > 1 ? juce::File::getCurrentWorkingDirectory().getChildFile (args[1]) : juce::File();
        new ScanReportRun (outFile);        // deletes itself when done
        return true;
    }

   #if JUCE_LINUX
    if (args[0] == "--dev-crash-selftest" && args.size() >= 4)
    {
        CrashGuard::install (juce::File::getCurrentWorkingDirectory().getChildFile (args[1]));
        CrashGuard::currentRtSlot = (uint32_t) args[2].getIntValue();
        CrashGuard::currentUiSlot = (uint32_t) args[3].getIntValue();
        std::raise (SIGSEGV);
        std::_Exit (0);         // not reached: the handler re-raises with the default action
    }
   #endif

    return false;
}
}
