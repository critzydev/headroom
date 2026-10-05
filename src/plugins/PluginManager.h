// PluginManager: plugin formats, the known-plugin list, out-of-process scanning, instantiation and editor windows.
#pragma once

#include "Juce.h"
#include "plugins/PluginWindow.h"
#include "plugins/ScanRunner.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace hr
{
class PluginManager : private juce::ChangeListener
{
public:
    // pluginsFile is where the known-plugin list (and the blacklist) is cached (plugins.xml); empty = don't persist.
    // Window positions live next to it in plugin-windows.xml. deadMansFile is JUCE's dead-man's-pedal file: the
    // plugin file being scanned right now, so that a scan that took the whole app down blacklists it next time
    // (Paths::getScanDeadMansFile(), in the state dir); empty = none.
    explicit PluginManager (const juce::File& pluginsFile = {}, const juce::File& deadMansFile = {});
    ~PluginManager() override;

    // VST3 (+ LV2, LADSPA on Linux) via juce::addDefaultFormatsToManager.
    juce::AudioPluginFormatManager& getFormatManager() noexcept         { return formatManager; }
    juce::KnownPluginList& getKnownPluginList() noexcept                { return knownPlugins; }

    // ---- The list ---------------------------------------------------------------------------------
    juce::Array<juce::PluginDescription> getKnownPlugins() const;
    // What the "Add effect" menu should offer: everything except synths, which cannot process a channel's signal.
    // createInstance() refuses those anyway.
    juce::Array<juce::PluginDescription> getKnownEffects() const;
    static bool canBeEffect (const juce::PluginDescription& description);
    // Files that crashed or hung the scanner (or were being scanned when Headroom died) and are skipped from now
    // on. startBackgroundScan (true) forgets them.
    juce::StringArray getBlacklistedFiles() const;
    // Writes plugins.xml now (also done after scans, now and then during one, and on destruction).
    void savePluginList();

    // ---- Scanning (out of process) ------------------------------------------------------
    // Message thread. Returns at once; the work happens on a background thread that runs a child process
    // (`headroom --scan-worker`), never on the message thread. rescanAll=false (startup): only files that are not
    // already known or blacklisted. rescanAll=true ("Rescan all"): forgets the list and the blacklist first.
    // Asking while a scan runs queues one more run.
    void startBackgroundScan (bool rescanAll);
    void stopScan();                                                    // abandons a running scan (kills its worker)
    bool isScanning() const noexcept                                    { return scanning.load(); }
    float getScanProgress() const noexcept                              { return scanProgress.load(); }   // 0..1 while scanning
    juce::String getScanStatusText() const;                             // "Checking TDR Nova" (the file's name), any thread

    std::function<void()> onPluginListChanged;                          // message thread: the list, the blacklist or isScanning() changed

    struct ScanStats
    {
        int filesFound = 0;             // candidate files the search turned up
        int filesChecked = 0;           // sent to the scanner (the rest were known or blacklisted)
        int skippedKnown = 0, skippedBlacklisted = 0;
        int pluginsAdded = 0;
        int crashed = 0, timedOut = 0;  // now blacklisted
        juce::StringArray skipped;      // ... which files those are
        bool completed = false, aborted = false;
        juce::String error;             // the scanner could not be run at all
        double seconds = 0.0;
    };

    const ScanStats& getLastScanStats() const noexcept                  { return lastStats; }
    std::function<void (const ScanStats&)> onScanFinished;              // message thread: a scan ran to its end (not one that was stopped)

    // Mostly for tests and the dev tools; the defaults are what the app uses.
    struct ScanSettings
    {
        juce::File workerExecutable;            // empty = this executable
        int itemTimeoutMs = 30000;              // one plugin file: then its worker is killed and the file blacklisted
        bool useDefaultLocations = true;        // false: search only the extra folders
        bool testHooks = false;                 // honour "hr-test:crash" / "hr-test:hang" in extraJobs
        std::vector<ScanJob> extraJobs;         // scanned in addition to what the search finds
    };

    void setScanSettings (const ScanSettings& settings);                // not while scanning
    const ScanSettings& getScanSettings() const noexcept                { return scanSettings; }

    // Folders searched in addition to each format's defaults (Settings > Plugins).
    void setExtraFolders (const juce::StringArray& folders);
    const juce::StringArray& getExtraFolders() const noexcept           { return extraFolders; }
    // Default locations of `format` + /usr/lib64/ladspa for LADSPA on Linux + the extra folders (each once).
    juce::FileSearchPath getSearchPaths (juce::AudioPluginFormat& format) const;

    // ---- Instantiation (message thread) -----------------------------------------------------------
    // Synchronous, in-process. Returns nullptr and fills `error` on failure (missing file, refused by the plugin,
    // or a synth, which cannot be an effect - see canBeEffect). The caller does the layout / prepare /
    // setStateInformation dance (FxSlot::configure) and wraps this in a CrashGuard::ScopedUiSlot.
    std::unique_ptr<juce::AudioPluginInstance> createInstance (const juce::PluginDescription& description,
                                                               double sampleRate, int blockSize, juce::String& error);

    // ---- Editors (message thread) -----------------------------------------------------------------
    // One window per slot, titled `title` (empty = the processor's name), reopened where it last was. Opening again
    // brings the existing window to the front. crashId (the slot's CrashGuard id) is what a crash while the editor is
    // being created or destroyed is blamed on. closeEditor() must be called before the processor is destroyed.
    void openEditor (const juce::String& slotUid, juce::AudioProcessor& processor,
                     const juce::String& title = {}, uint32_t crashId = 0);
    void closeEditor (const juce::String& slotUid);
    void closeAllEditors();
    bool isEditorOpen (const juce::String& slotUid) const               { return windows.find (slotUid) != windows.end(); }
    PluginWindow* getEditorWindow (const juce::String& slotUid) const;
    std::optional<juce::Rectangle<int>> getRememberedEditorBounds (const juce::String& slotUid) const;

private:
    class ScanThread;
    struct ScanPlan;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    void loadPluginList();
    void loadWindowPositions();
    void saveWindowPositions() const;

    void runScan (ScanPlan& plan, juce::Thread& thread, uint32_t generation);   // scan thread
    void setScanStatus (const juce::String& text);
    void applyScanOutcome (uint32_t generation, const juce::String& format, const juce::String& identifier,
                           ScanRunner::Outcome outcome, const juce::Array<juce::PluginDescription>& found);
    void makeKnownToLv2World (const juce::String& bundle, const juce::Array<juce::PluginDescription>& found);
    void finishScan (uint32_t generation, const ScanStats& stats, bool searchWasComplete);
    void removeVanishedPlugins();
    void installCustomScanner();

    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;
    const juce::File pluginsFile, deadMansFile, windowsFile;
    juce::StringArray extraFolders;
    ScanSettings scanSettings;

    // Scanning state. The atomics / the locked text are read from any thread; everything else is message thread only.
    std::atomic<bool> scanning { false };
    std::atomic<float> scanProgress { 0.0f };
    mutable juce::CriticalSection statusLock;
    juce::String scanStatusText;
    std::unique_ptr<ScanThread> scanThread;
    uint32_t scanGeneration = 0;                    // results of an abandoned scan are ignored
    bool scanQueued = false, queuedRescanAll = false;
    ScanStats lastStats;
    std::map<juce::String, juce::int64> scannedBundles;     // "format|id" -> signature, for formats JUCE can't judge (LV2)
    bool listDirty = false;
    juce::uint32 lastSaveMs = 0;

    struct OpenWindow
    {
        std::unique_ptr<PluginWindow> window;
        uint32_t crashId = 0;
    };

    struct RememberedWindow
    {
        juce::Rectangle<int> bounds;
        juce::int64 lastUsed = 0;
    };

    std::map<juce::String, OpenWindow> windows;
    std::map<juce::String, RememberedWindow> rememberedBounds;      // where each slot's window last was

    JUCE_DECLARE_WEAK_REFERENCEABLE (PluginManager)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginManager)
};
}
