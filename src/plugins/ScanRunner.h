// ScanRunner: runs plugin scan jobs in a child process (`headroom --scan-worker`) and survives whatever the plugins do.
#pragma once

#include "Juce.h"
#include "plugins/ScanProtocol.h"

#include <functional>
#include <map>
#include <vector>

namespace hr
{
struct ScanJob
{
    juce::String format;            // "VST3", "LV2", "LADSPA"
    juce::String identifier;        // file path, or LV2 bundle path / URI
};

struct SearchRequest
{
    juce::String format;
    juce::FileSearchPath paths;
    bool recursive = true;
};

// Blocking, for a background thread only (never the message thread). One worker process handles many files, so
// the LV2 world is loaded once, not once per bundle. If the worker crashes on a file it is restarted, the file is
// retried alone in a fresh worker (a plugin's own thread can crash the worker while it scans a different file),
// and only if that crashes too the file is reported as crashed. A file that takes longer than itemTimeoutMs gets
// the worker killed and is reported as timed out. Our own kill, not the plugin's cooperation: a worker stuck in a
// plugin never answers anything.
class ScanRunner
{
public:
    struct Settings
    {
        juce::File workerExecutable;    // empty = the running executable (the tests point this at headroom)
        int itemTimeoutMs = 30000;      // one file, from "started" to "answered"
        int startupTimeoutMs = 30000;   // worker launch (and building the plugin formats) until its first job
        int pollMs = 20;
        bool testHooks = false;         // let the worker obey "hr-test:crash" / "hr-test:hang" identifiers
    };

    explicit ScanRunner (Settings settingsToUse) : settings (std::move (settingsToUse)) {}
    ScanRunner() = default;

    // Polled while waiting; true abandons the run (the worker is killed, nothing is reported for the file in flight).
    void setShouldExit (std::function<bool()> callback)     { shouldExit = std::move (callback); }

    // ---- Step 1: which files might be plugins ------------------------------------------------------
    struct SearchResult
    {
        bool complete = false;                                  // every requested format answered
        std::map<juce::String, juce::StringArray> files;        // by format name, duplicates removed
        juce::String error;
    };

    SearchResult search (const std::vector<SearchRequest>& requests);

    // ---- Step 2: what is in them ---------------------------------------------------------------------
    enum class Outcome { scanned, crashed, timedOut };
    using Found = juce::OwnedArray<juce::PluginDescription>;

    struct Callbacks
    {
        std::function<void (size_t index, const ScanJob&)> started;
        // `found` is empty for crashed / timed out files, and for files that simply hold no usable plugin.
        std::function<void (size_t index, const ScanJob&, Outcome, Found& found)> finished;
    };

    struct ScanReport
    {
        bool completed = false;         // every job got an outcome
        bool aborted = false;           // shouldExit() said stop
        juce::String error;             // the worker could not be run at all
    };

    ScanReport scan (const std::vector<ScanJob>& jobs, const Callbacks& callbacks);

private:
    struct Session
    {
        enum class End { finished, workerDied, timedOut, launchFailed, aborted };

        End end = End::launchFailed;
        size_t completed = 0;           // jobs answered (always a prefix of the session's list)
        int inProgress = -1;            // job that was started but never answered, -1 = none
        juce::String detail;
    };

    Session runSession (const std::vector<scan::WireJob>& jobs,
                        const std::function<void (size_t)>& onBegin,
                        const std::function<void (size_t, char, const juce::String&)>& onResult);

    bool wantsExit() const                                  { return shouldExit && shouldExit(); }

    Settings settings;
    std::function<bool()> shouldExit;
};

// Keeps KnownPluginList::scanAndAddFile() out of process too: anything that asks the list to scan a single file
// (drag and drop, a future "Add plugin file") goes through the same worker. findPluginTypesFor blocks, so call it
// from a background thread. Returns false (= blacklist the file) when the worker crashed or timed out.
class OutOfProcessScanner final : public juce::KnownPluginList::CustomScanner
{
public:
    explicit OutOfProcessScanner (ScanRunner::Settings settingsToUse) : settings (std::move (settingsToUse)) {}

    bool findPluginTypesFor (juce::AudioPluginFormat& format, juce::OwnedArray<juce::PluginDescription>& result,
                             const juce::String& fileOrIdentifier) override;

private:
    ScanRunner::Settings settings;
};
}
