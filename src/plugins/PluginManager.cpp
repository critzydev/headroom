// PluginManager: format setup, the persisted plugin list, background out-of-process scanning, instantiation and editors.
#include "plugins/PluginManager.h"

#include "app/Log.h"
#include "plugins/CrashGuard.h"

#include <algorithm>
#include <utility>

namespace hr
{
namespace
{
    constexpr juce::uint32 kSaveEveryMs = 5000;         // while a long scan is running
    constexpr size_t kMaxRememberedWindows = 128;

    // The name shown while checking a file: "ZL Equalizer 2.vst3" -> "ZL Equalizer 2". URIs stay as they are.
    juce::String displayNameOf (const juce::String& identifier)
    {
        if (juce::File::isAbsolutePath (identifier))
            return juce::File (identifier).getFileNameWithoutExtension();

        return identifier;
    }

    // JUCE decides "is this file already in the list, and unchanged" from the plugins it found in it. That works for
    // VST3 and LADSPA (identifier = file), but an LV2 bundle is listed under its plugins' URIs, so JUCE always calls it
    // unknown. For LV2 we remember the bundles we scanned instead, together with a signature that changes when the
    // bundle does.
    bool needsSignature (const juce::String& format)
    {
        return format == "LV2";
    }

    juce::int64 signatureOf (const juce::String& identifier)
    {
        if (! juce::File::isAbsolutePath (identifier))
            return 0;

        const juce::File bundle (identifier);

        if (! bundle.exists())
            return 0;

        auto newest = bundle.getLastModificationTime().toMilliseconds();

        if (const auto manifest = bundle.getChildFile ("manifest.ttl"); manifest.existsAsFile())
            newest = juce::jmax (newest, manifest.getLastModificationTime().toMilliseconds());

        return newest;
    }

    juce::String bundleKey (const juce::String& format, const juce::String& identifier)
    {
        return format + "|" + identifier;
    }

    ScanRunner::Settings makeRunnerSettings (const PluginManager::ScanSettings& settings)
    {
        ScanRunner::Settings runner;
        runner.workerExecutable = settings.workerExecutable;
        runner.itemTimeoutMs = settings.itemTimeoutMs;
        runner.testHooks = settings.testHooks;
        return runner;
    }
}

// What the scan thread needs, copied on the message thread when the scan starts: it never touches the format
// objects' search state or the blacklist itself, so it cannot race with the message thread using them.
struct PluginManager::ScanPlan
{
    struct FormatInfo
    {
        juce::String name;
        juce::AudioPluginFormat* format = nullptr;      // only used for KnownPluginList::isListingUpToDate (a file time check)
        juce::FileSearchPath paths;
    };

    bool rescanAll = false;
    ScanSettings settings;
    std::vector<FormatInfo> formats;
    juce::StringArray blacklist;
    std::map<juce::String, juce::int64> scanned;
};

class PluginManager::ScanThread final : public juce::Thread
{
public:
    ScanThread (PluginManager& m, ScanPlan p, uint32_t g)
        : juce::Thread ("Headroom plugin scan"), owner (m), plan (std::move (p)), generation (g) {}

    ~ScanThread() override                  { stopThread (15000); }

    void run() override                     { owner.runScan (plan, *this, generation); }

private:
    PluginManager& owner;
    ScanPlan plan;
    uint32_t generation;
};

// ---------------------------------------------------------------------------------------------------
// Construction, persistence
// ---------------------------------------------------------------------------------------------------

PluginManager::PluginManager (const juce::File& cacheFile, const juce::File& pedalFile)
    : pluginsFile (cacheFile),
      deadMansFile (pedalFile),
      windowsFile (cacheFile != juce::File() ? cacheFile.getSiblingFile ("plugin-windows.xml") : juce::File())
{
    juce::addDefaultFormatsToManager (formatManager);   // addDefaultFormats() was removed in JUCE 8.0.9

    installCustomScanner();
    loadPluginList();
    loadWindowPositions();

    knownPlugins.addChangeListener (this);
}

PluginManager::~PluginManager()
{
    stopScan();
    closeAllEditors();
    knownPlugins.removeChangeListener (this);

    if (listDirty)
        savePluginList();

    saveWindowPositions();
}

void PluginManager::installCustomScanner()
{
    // Whatever asks the list to scan a file (KnownPluginList::scanAndAddFile) gets the out-of-process scanner,
    // never a plugin loaded into this process.
    knownPlugins.setCustomScanner (std::make_unique<OutOfProcessScanner> (makeRunnerSettings (scanSettings)));
}

void PluginManager::setScanSettings (const ScanSettings& settings)
{
    jassert (scanThread == nullptr);
    scanSettings = settings;
    installCustomScanner();
}

void PluginManager::loadPluginList()
{
    if (pluginsFile == juce::File() || ! pluginsFile.existsAsFile())
        return;

    auto xml = juce::XmlDocument::parse (pluginsFile);

    if (xml == nullptr || ! xml->hasTagName ("KNOWNPLUGINS"))
    {
        Log::warn ("The saved plugin list could not be read: it will be rebuilt");
        return;
    }

    knownPlugins.recreateFromXml (*xml);        // plugins and blacklist; ignores the SCANNED elements below

    for (auto* element : xml->getChildWithTagNameIterator ("SCANNED"))
        scannedBundles[bundleKey (element->getStringAttribute ("format"), element->getStringAttribute ("id"))]
            = element->getStringAttribute ("sig").getLargeIntValue();
}

void PluginManager::savePluginList()
{
    if (pluginsFile == juce::File())
        return;

    auto xml = knownPlugins.createXml();

    for (const auto& [key, signature] : scannedBundles)
    {
        auto* element = xml->createNewChildElement ("SCANNED");
        element->setAttribute ("format", key.upToFirstOccurrenceOf ("|", false, false));
        element->setAttribute ("id", key.fromFirstOccurrenceOf ("|", false, false));
        element->setAttribute ("sig", juce::String (signature));
    }

    pluginsFile.getParentDirectory().createDirectory();

    juce::TemporaryFile temporary (pluginsFile);

    if (xml->writeTo (temporary.getFile()) && temporary.overwriteTargetFileWithTemporary())
    {
        listDirty = false;
        lastSaveMs = juce::Time::getMillisecondCounter();
    }
    else
    {
        Log::warn ("Could not save the plugin list to " + pluginsFile.getFullPathName());
    }
}

void PluginManager::changeListenerCallback (juce::ChangeBroadcaster*)
{
    listDirty = true;

    if (onPluginListChanged)
        onPluginListChanged();
}

bool PluginManager::canBeEffect (const juce::PluginDescription& description)
{
    // A synth has nothing to feed a channel's audio into (it would replace the signal with silence). Only trust
    // "no inputs" together with the instrument flag: VST3 descriptions read from moduleinfo.json without loading the
    // plugin report 0 channels for everything, effects included. (FxSlot::process ignores a plugin that really has
    // no inputs once it is loaded.)
    return ! (description.isInstrument && description.numInputChannels == 0);
}

juce::Array<juce::PluginDescription> PluginManager::getKnownPlugins() const
{
    return knownPlugins.getTypes();
}

juce::Array<juce::PluginDescription> PluginManager::getKnownEffects() const
{
    juce::Array<juce::PluginDescription> effects;

    for (const auto& description : knownPlugins.getTypes())
        if (canBeEffect (description))
            effects.add (description);

    return effects;
}

juce::StringArray PluginManager::getBlacklistedFiles() const
{
    return knownPlugins.getBlacklistedFiles();
}

// ---------------------------------------------------------------------------------------------------
// Search paths
// ---------------------------------------------------------------------------------------------------

void PluginManager::setExtraFolders (const juce::StringArray& folders)
{
    extraFolders = folders;
}

juce::FileSearchPath PluginManager::getSearchPaths (juce::AudioPluginFormat& format) const
{
    juce::FileSearchPath paths;

    if (scanSettings.useDefaultLocations)
    {
        paths = format.getDefaultLocationsToSearch();

       #if JUCE_LINUX || JUCE_BSD
        // JUCE's LADSPA default lacks lib64, which is where Fedora puts them.
        if (format.getName() == "LADSPA")
            paths.addIfNotAlreadyThere (juce::File ("/usr/lib64/ladspa"));
       #endif
    }

    for (const auto& folder : extraFolders)
        if (folder.isNotEmpty() && juce::File::isAbsolutePath (folder))
            paths.addIfNotAlreadyThere (juce::File (folder));

    return paths;
}

// ---------------------------------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------------------------------

juce::String PluginManager::getScanStatusText() const
{
    const juce::ScopedLock lock (statusLock);
    return scanStatusText;
}

void PluginManager::setScanStatus (const juce::String& text)
{
    const juce::ScopedLock lock (statusLock);
    scanStatusText = text;
}

void PluginManager::startBackgroundScan (bool rescanAll)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (scanThread != nullptr)
    {
        scanQueued = true;
        queuedRescanAll = queuedRescanAll || rescanAll;
        return;
    }

    ++scanGeneration;
    scanning = true;
    scanProgress = 0.0f;
    setScanStatus ("Looking for plugins");

    if (rescanAll)
    {
        knownPlugins.clear();
        knownPlugins.clearBlacklistedFiles();
        scannedBundles.clear();

        if (deadMansFile != juce::File())
            deadMansFile.deleteFile();
    }
    else if (deadMansFile != juce::File() && deadMansFile.existsAsFile())
    {
        // Headroom ended while this file was being scanned. JUCE would blacklist it now, but the scan runs in a
        // child process: a plugin cannot have taken us down with it, and an unrelated crash or a power cut must not
        // hide a good plugin. It is simply scanned again (out of process, where a real crasher gets blacklisted).
        juce::StringArray lines;
        deadMansFile.readLines (lines);
        lines.removeEmptyStrings();

        for (const auto& line : lines)
            Log::warn ("Headroom stopped while checking " + line + "; checking it again");

        deadMansFile.deleteFile();
    }

    ScanPlan plan;
    plan.rescanAll = rescanAll;
    plan.settings = scanSettings;
    plan.blacklist = knownPlugins.getBlacklistedFiles();
    plan.scanned = scannedBundles;

    for (auto* format : formatManager.getFormats())
        if (format->canScanForPlugins())
            plan.formats.push_back ({ format->getName(), format, getSearchPaths (*format) });

    scanThread = std::make_unique<ScanThread> (*this, std::move (plan), scanGeneration);
    scanThread->startThread (juce::Thread::Priority::low);

    if (onPluginListChanged)
        onPluginListChanged();
}

void PluginManager::stopScan()
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (scanThread == nullptr)
        return;

    ++scanGeneration;                       // whatever the thread still posts is ignored
    scanThread->signalThreadShouldExit();
    scanThread.reset();                     // waits for it: the runner kills the worker within a few polls
    scanQueued = queuedRescanAll = false;
    scanning = false;
    setScanStatus ({});
}

// Runs on the scan thread. Reads only the plan (a snapshot) and posts every change to the message thread, so the
// KnownPluginList's blacklist and our records are only ever touched there.
void PluginManager::runScan (ScanPlan& plan, juce::Thread& thread, uint32_t generation)
{
    const auto startedAt = juce::Time::getMillisecondCounterHiRes();
    const juce::WeakReference<PluginManager> weak (this);

    ScanStats stats;

    ScanRunner runner (makeRunnerSettings (plan.settings));
    runner.setShouldExit ([&thread] { return thread.threadShouldExit(); });

    // 1. Which files might be plugins.
    std::vector<SearchRequest> requests;

    for (const auto& format : plan.formats)
        requests.push_back ({ format.name, format.paths, true });

    const auto search = runner.search (requests);

    // 2. Which of them we actually have to look at.
    std::vector<ScanJob> jobs;

    for (const auto& format : plan.formats)
    {
        const auto found = search.files.find (format.name);

        if (found == search.files.end())
            continue;

        for (const auto& identifier : found->second)
        {
            ++stats.filesFound;

            if (plan.blacklist.contains (identifier))
            {
                ++stats.skippedBlacklisted;
                continue;
            }

            if (! plan.rescanAll)
            {
                bool known = false;

                if (needsSignature (format.name))
                {
                    const auto record = plan.scanned.find (bundleKey (format.name, identifier));
                    known = record != plan.scanned.end() && record->second != 0 && record->second == signatureOf (identifier);
                }
                else
                {
                    // Compares the file's modification time with what the list remembers: no plugin code involved.
                    known = knownPlugins.isListingUpToDate (identifier, *format.format);
                }

                if (known)
                {
                    ++stats.skippedKnown;
                    continue;
                }
            }

            jobs.push_back ({ format.name, identifier });
        }
    }

    for (const auto& extra : plan.settings.extraJobs)
        jobs.push_back (extra);

    // 3. Look at them.
    std::atomic<size_t> finished { 0 };
    const auto total = jobs.size();

    ScanRunner::Callbacks callbacks;

    callbacks.started = [&] (size_t, const ScanJob& job)
    {
        setScanStatus ("Checking " + displayNameOf (job.identifier));

        // The dead-man's pedal: the file being scanned right now (see startBackgroundScan for how a stale one is treated).
        if (deadMansFile != juce::File())
            deadMansFile.replaceWithText (job.identifier);
    };

    callbacks.finished = [&] (size_t, const ScanJob& job, ScanRunner::Outcome outcome, ScanRunner::Found& found)
    {
        if (deadMansFile != juce::File())
            deadMansFile.deleteFile();

        juce::Array<juce::PluginDescription> descriptions;

        for (auto* description : found)
            descriptions.add (*description);

        ++stats.filesChecked;
        stats.pluginsAdded += descriptions.size();
        stats.crashed += outcome == ScanRunner::Outcome::crashed ? 1 : 0;
        stats.timedOut += outcome == ScanRunner::Outcome::timedOut ? 1 : 0;

        if (outcome != ScanRunner::Outcome::scanned)
            stats.skipped.add (job.identifier);

        scanProgress = total > 0 ? (float) ++finished / (float) total : 1.0f;

        juce::MessageManager::callAsync ([weak, generation, format = job.format, identifier = job.identifier, outcome, descriptions]
        {
            if (auto* self = weak.get())
                self->applyScanOutcome (generation, format, identifier, outcome, descriptions);
        });
    };

    const auto report = runner.scan (jobs, callbacks);

    if (deadMansFile != juce::File())
        deadMansFile.deleteFile();          // an abandoned scan (shutdown) is not the file's fault

    stats.completed = report.completed && search.complete;
    stats.aborted = report.aborted || thread.threadShouldExit();
    stats.error = report.error.isNotEmpty() ? report.error : search.error;
    stats.seconds = (juce::Time::getMillisecondCounterHiRes() - startedAt) / 1000.0;

    const bool searchWasComplete = search.complete && ! stats.aborted;

    juce::MessageManager::callAsync ([weak, generation, stats, searchWasComplete]
    {
        if (auto* self = weak.get())
            self->finishScan (generation, stats, searchWasComplete);
    });
}

void PluginManager::applyScanOutcome (uint32_t generation, const juce::String& format, const juce::String& identifier,
                                      ScanRunner::Outcome outcome, const juce::Array<juce::PluginDescription>& found)
{
    if (generation != scanGeneration)
        return;

    switch (outcome)
    {
        case ScanRunner::Outcome::scanned:
            for (const auto& description : found)
                knownPlugins.addType (description);

            if (needsSignature (format))
            {
                scannedBundles[bundleKey (format, identifier)] = signatureOf (identifier);
                makeKnownToLv2World (identifier, found);
            }

            break;

        case ScanRunner::Outcome::crashed:
        case ScanRunner::Outcome::timedOut:
            knownPlugins.addToBlacklist (identifier);
            break;
    }

    if (juce::Time::getMillisecondCounter() - lastSaveMs > kSaveEveryMs)
        savePluginList();
}

// The worker found these LV2 plugins in its own LV2 world. createInstance() looks them up in ours, which was filled when the
// format was created, so a bundle installed (or in an extra folder) since then must be loaded here too. Only bundles the
// worker has already survived get this far.
void PluginManager::makeKnownToLv2World (const juce::String& bundle, const juce::Array<juce::PluginDescription>& found)
{
    if (! juce::File::isAbsolutePath (bundle))
        return;

    for (auto* format : formatManager.getFormats())
    {
        if (format->getName() != "LV2")
            continue;

        for (const auto& description : found)
        {
            if (! format->doesPluginStillExist (description))
            {
                juce::OwnedArray<juce::PluginDescription> ignored;
                format->findAllTypesForFile (ignored, bundle);
                return;
            }
        }
    }
}

void PluginManager::finishScan (uint32_t generation, const ScanStats& stats, bool searchWasComplete)
{
    if (generation != scanGeneration)
        return;

    lastStats = stats;

    if (searchWasComplete && stats.completed)
        removeVanishedPlugins();

    savePluginList();

    scanThread.reset();                     // its run() is over: this just joins
    scanning = false;
    scanProgress = 1.0f;
    setScanStatus ({});

    Log::info ("Plugin scan: " + juce::String (stats.filesChecked) + " files checked, " + juce::String (stats.pluginsAdded)
               + " plugins found, " + juce::String (stats.skippedKnown) + " already known, "
               + juce::String (stats.crashed + stats.timedOut) + " skipped as broken, "
               + juce::String (stats.seconds, 1) + " s" + (stats.error.isNotEmpty() ? ", error: " + stats.error : juce::String()));

    if (onPluginListChanged)
        onPluginListChanged();

    if (onScanFinished)
        onScanFinished (stats);

    if (scanQueued)
    {
        const bool again = queuedRescanAll;
        scanQueued = queuedRescanAll = false;
        startBackgroundScan (again);
    }
}

// Plugins whose file was uninstalled since the last scan leave the list. (LV2 is judged by its own records: whether a
// URI still resolves is a question for the LV2 world, which belongs to the message thread's formats and is not
// current for a bundle installed while we were running.)
void PluginManager::removeVanishedPlugins()
{
    for (const auto& description : knownPlugins.getTypes())
        if (! needsSignature (description.pluginFormatName)
             && juce::File::isAbsolutePath (description.fileOrIdentifier)
             && ! juce::File (description.fileOrIdentifier).exists())
            knownPlugins.removeType (description);

    for (auto it = scannedBundles.begin(); it != scannedBundles.end();)
    {
        const auto identifier = it->first.fromFirstOccurrenceOf ("|", false, false);

        if (juce::File::isAbsolutePath (identifier) && ! juce::File (identifier).exists())
            it = scannedBundles.erase (it);
        else
            ++it;
    }
}

// ---------------------------------------------------------------------------------------------------
// Instantiation
// ---------------------------------------------------------------------------------------------------

std::unique_ptr<juce::AudioPluginInstance> PluginManager::createInstance (const juce::PluginDescription& description,
                                                                          double sampleRate, int blockSize, juce::String& error)
{
    JUCE_ASSERT_MESSAGE_THREAD

    error.clear();

    if (! canBeEffect (description))
    {
        error = description.name + " is a synth or generator, not an effect";
        return nullptr;
    }

    try
    {
        return formatManager.createPluginInstance (description, sampleRate, blockSize, error);
    }
    catch (const std::exception& e)
    {
        error = description.name + " failed to load: " + juce::String::fromUTF8 (e.what());
    }
    catch (...)
    {
        error = description.name + " failed to load";
    }

    return nullptr;
}

// ---------------------------------------------------------------------------------------------------
// Editors
// ---------------------------------------------------------------------------------------------------

void PluginManager::openEditor (const juce::String& slotUid, juce::AudioProcessor& processor,
                                const juce::String& title, uint32_t crashId)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (auto existing = windows.find (slotUid); existing != windows.end())
    {
        existing->second.window->toFront (true);
        return;
    }

    const CrashGuard::ScopedUiSlot crashScope (crashId);

    std::optional<juce::Rectangle<int>> remembered;

    if (auto known = rememberedBounds.find (slotUid); known != rememberedBounds.end())
        remembered = known->second.bounds;

    auto window = std::make_unique<PluginWindow> (processor,
                                                  title.isNotEmpty() ? title : processor.getName(),
                                                  [weak = juce::WeakReference<PluginManager> (this), slotUid]
                                                  {
                                                      // Deleting the window from inside its own close callback is not safe.
                                                      juce::MessageManager::callAsync ([weak, slotUid]
                                                      {
                                                          if (auto* self = weak.get())
                                                              self->closeEditor (slotUid);
                                                      });
                                                  },
                                                  remembered);

    windows[slotUid] = { std::move (window), crashId };
}

void PluginManager::closeEditor (const juce::String& slotUid)
{
    JUCE_ASSERT_MESSAGE_THREAD

    auto it = windows.find (slotUid);

    if (it == windows.end())
        return;

    rememberedBounds[slotUid] = { it->second.window->getBounds(), juce::Time::currentTimeMillis() };

    {
        // Editors crash in their destructors more often than one would hope.
        const CrashGuard::ScopedUiSlot crashScope (it->second.crashId);
        it->second.window.reset();      // the window (and its editor) must go before the processor does
    }

    windows.erase (it);
    saveWindowPositions();
}

void PluginManager::closeAllEditors()
{
    while (! windows.empty())
        closeEditor (windows.begin()->first);
}

PluginWindow* PluginManager::getEditorWindow (const juce::String& slotUid) const
{
    const auto it = windows.find (slotUid);
    return it != windows.end() ? it->second.window.get() : nullptr;
}

std::optional<juce::Rectangle<int>> PluginManager::getRememberedEditorBounds (const juce::String& slotUid) const
{
    if (const auto it = rememberedBounds.find (slotUid); it != rememberedBounds.end())
        return it->second.bounds;

    return std::nullopt;
}

void PluginManager::loadWindowPositions()
{
    if (windowsFile == juce::File() || ! windowsFile.existsAsFile())
        return;

    if (auto xml = juce::XmlDocument::parse (windowsFile); xml != nullptr && xml->hasTagName ("PLUGINWINDOWS"))
        for (auto* element : xml->getChildWithTagNameIterator ("WINDOW"))
            rememberedBounds[element->getStringAttribute ("uid")]
                = { { element->getIntAttribute ("x"), element->getIntAttribute ("y"),
                      element->getIntAttribute ("w"), element->getIntAttribute ("h") },
                    element->getStringAttribute ("used").getLargeIntValue() };
}

void PluginManager::saveWindowPositions() const
{
    if (windowsFile == juce::File())
        return;

    // Keep the most recently used ones: slots come and go, their uids never come back.
    std::vector<std::pair<juce::String, RememberedWindow>> entries (rememberedBounds.begin(), rememberedBounds.end());

    std::sort (entries.begin(), entries.end(), [] (const auto& a, const auto& b) { return a.second.lastUsed > b.second.lastUsed; });

    if (entries.size() > kMaxRememberedWindows)
        entries.resize (kMaxRememberedWindows);

    juce::XmlElement xml ("PLUGINWINDOWS");

    for (const auto& [uid, window] : entries)
    {
        auto* element = xml.createNewChildElement ("WINDOW");
        element->setAttribute ("uid", uid);
        element->setAttribute ("x", window.bounds.getX());
        element->setAttribute ("y", window.bounds.getY());
        element->setAttribute ("w", window.bounds.getWidth());
        element->setAttribute ("h", window.bounds.getHeight());
        element->setAttribute ("used", juce::String (window.lastUsed));
    }

    windowsFile.getParentDirectory().createDirectory();

    juce::TemporaryFile temporary (windowsFile);

    if (xml.writeTo (temporary.getFile()))
        temporary.overwriteTargetFileWithTemporary();
}
}
