// Unit tests for plugin hosting: search paths, plugins.xml, CrashGuard, the out-of-process scanner, editors, FxSlot layouts.
#include "Juce.h"

#include "app/AppController.h"
#include "app/Paths.h"
#include "engine/FxChain.h"
#include "plugins/CrashGuard.h"
#include "plugins/PluginManager.h"
#include "plugins/ScanProtocol.h"
#include "plugins/ScanRunner.h"

#if JUCE_WINDOWS
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <vector>

// JUCE 8+ leaves MessageManager::runDispatchLoopUntil() out unless JUCE_MODAL_LOOPS_PERMITTED is on, and we don't want that
// set for the whole app. The tests only need to let async callbacks run, so they call the platform pump directly (it has
// external linkage; this is a link error, not a silent change, if JUCE ever moves it).
namespace juce::detail
{
bool dispatchNextMessageOnSystemQueue (bool returnIfNoPendingMessages);
}

namespace hr
{
namespace
{
    struct TempDir
    {
        TempDir() : dir (juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("headroom-plugintest", "", false))
        {
            dir.createDirectory();
        }

        ~TempDir()      { dir.deleteRecursively(); }

        juce::File dir;
    };

    // Points Paths at a throw-away root for the duration of a test.
    struct ScopedRoot
    {
        explicit ScopedRoot (const juce::File& root)    { Paths::setRootOverride (root); }
        ~ScopedRoot()                                   { Paths::setRootOverride ({}); }
    };

    // The scanner worker is the real headroom executable, which sits next to headroom_tests in the build directory.
    juce::File workerExecutable()
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

   #if JUCE_WINDOWS
    // A plugin's own editor is a native child window, which Component::createComponentSnapshot cannot see (the result is just the
    // window background). Ask Windows to paint the whole window, frame included, into a bitmap instead.
    juce::Image captureNativeWindow (juce::Component& window)
    {
        auto* peer = window.getPeer();

        if (peer == nullptr)
            return {};

        const auto hwnd = (HWND) peer->getNativeHandle();
        RECT rect {};
        GetWindowRect (hwnd, &rect);
        const int width = rect.right - rect.left, height = rect.bottom - rect.top;

        if (width <= 0 || height <= 0)
            return {};

        HDC screen = GetDC (nullptr);
        HDC memory = CreateCompatibleDC (screen);
        HBITMAP bitmap = CreateCompatibleBitmap (screen, width, height);
        const auto previous = SelectObject (memory, bitmap);

        PrintWindow (hwnd, memory, 2 /* PW_RENDERFULLCONTENT */);

        BITMAPINFO info {};
        info.bmiHeader.biSize = sizeof (BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;          // top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        juce::Image image (juce::Image::ARGB, width, height, true);

        {
            juce::Image::BitmapData pixels (image, juce::Image::BitmapData::writeOnly);
            GetDIBits (memory, bitmap, 0, (UINT) height, pixels.data, &info, DIB_RGB_COLORS);

            for (int y = 0; y < height; ++y)
            {
                auto* line = reinterpret_cast<juce::uint32*> (pixels.getLinePointer (y));

                for (int x = 0; x < width; ++x)
                    line[x] |= 0xff000000u;         // GetDIBits leaves alpha at 0
            }
        }

        SelectObject (memory, previous);
        DeleteObject (bitmap);
        DeleteDC (memory);
        ReleaseDC (nullptr, screen);
        return image;
    }
   #endif

    bool canShowWindows()
    {
       #if JUCE_LINUX
        return juce::SystemStats::getEnvironmentVariable ("DISPLAY", {}).isNotEmpty();
       #else
        return true;
       #endif
    }

    juce::PluginDescription makeDescription (const juce::String& name, const juce::String& format, const juce::String& file,
                                             bool instrument = false)
    {
        juce::PluginDescription d;
        d.name = name;
        d.descriptiveName = name;
        d.pluginFormatName = format;
        d.manufacturerName = "Test Audio";
        d.version = "1.0";
        d.fileOrIdentifier = file;
        d.uniqueId = d.deprecatedUid = (int) name.hashCode();
        d.isInstrument = instrument;
        d.numInputChannels = instrument ? 0 : 2;
        d.numOutputChannels = 2;
        return d;
    }

    // A stand-in plugin with a few bus layouts, to check what FxSlot does with each.
    class FakePlugin final : public juce::AudioProcessor
    {
    public:
        enum class Kind { stereo, monoOnly, sidechain, instrument };

        explicit FakePlugin (Kind k, float gainToApply = 0.5f)
            : juce::AudioProcessor (makeBuses (k)), kind (k), gain (gainToApply) {}

        const juce::String getName() const override                            { return "Fake plugin"; }
        void prepareToPlay (double, int) override                               {}
        void releaseResources() override                                        {}
        bool acceptsMidi() const override                                       { return false; }
        bool producesMidi() const override                                      { return false; }
        double getTailLengthSeconds() const override                            { return 0.0; }
        bool hasEditor() const override                                         { return false; }
        juce::AudioProcessorEditor* createEditor() override                     { return nullptr; }
        int getNumPrograms() override                                           { return 1; }
        int getCurrentProgram() override                                        { return 0; }
        void setCurrentProgram (int) override                                   {}
        const juce::String getProgramName (int) override                        { return {}; }
        void changeProgramName (int, const juce::String&) override              {}
        void getStateInformation (juce::MemoryBlock&) override                  {}
        void setStateInformation (const void*, int) override                    {}

        bool isBusesLayoutSupported (const BusesLayout& layout) const override
        {
            const auto& in = layout.getMainInputChannelSet();
            const auto& out = layout.getMainOutputChannelSet();
            const auto mono = juce::AudioChannelSet::mono();
            const auto stereo = juce::AudioChannelSet::stereo();

            switch (kind)
            {
                case Kind::monoOnly:    return in == mono && out == mono;
                case Kind::instrument:  return in.isDisabled() && (out == mono || out == stereo);

                case Kind::stereo:
                case Kind::sidechain:
                {
                    const bool mainOk = (in == mono || in == stereo) && in == out;
                    const bool auxOk = layout.inputBuses.size() < 2 || layout.inputBuses[1].isDisabled() || layout.inputBuses[1] == stereo;
                    return mainOk && auxOk;
                }
            }

            return false;
        }

        void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            seenRtSlot = CrashGuard::currentRtSlot.load();
            lastNumChannels = buffer.getNumChannels();

            if (kind == Kind::instrument)
            {
                for (int c = 0; c < buffer.getNumChannels(); ++c)
                    juce::FloatVectorOperations::fill (buffer.getWritePointer (c), 0.25f, buffer.getNumSamples());

                return;
            }

            for (int c = 0; c < getMainBusNumInputChannels(); ++c)
                buffer.applyGain (c, 0, buffer.getNumSamples(), gain);
        }

        std::atomic<uint32_t> seenRtSlot { 0 };
        int lastNumChannels = 0;

    private:
        static BusesProperties makeBuses (Kind k)
        {
            const auto mono = juce::AudioChannelSet::mono();
            const auto stereo = juce::AudioChannelSet::stereo();

            switch (k)
            {
                case Kind::stereo:      return BusesProperties().withInput ("Input", stereo).withOutput ("Output", stereo);
                case Kind::monoOnly:    return BusesProperties().withInput ("Input", mono).withOutput ("Output", mono);
                case Kind::sidechain:   return BusesProperties().withInput ("Input", stereo).withInput ("Sidechain", stereo).withOutput ("Output", stereo);
                case Kind::instrument:  return BusesProperties().withOutput ("Output", stereo);
            }

            return {};
        }

        const Kind kind;
        const float gain;
    };
}

// ---------------------------------------------------------------------------------------------------
// Search paths, plugins.xml, effects
// ---------------------------------------------------------------------------------------------------

class PluginListTests final : public juce::UnitTest
{
public:
    PluginListTests() : juce::UnitTest ("Plugin list and search paths", "headroom") {}

    void runTest() override
    {
        beginTest ("search paths: format defaults + extra folders, each once, no junk");
        {
            TempDir a, b;
            PluginManager manager;
            manager.setExtraFolders ({ a.dir.getFullPathName(), "", "relative/folder", b.dir.getFullPathName(), a.dir.getFullPathName() });

            bool sawVst3 = false;

            for (auto* format : manager.getFormatManager().getFormats())
            {
                const auto paths = manager.getSearchPaths (*format);
                const auto defaults = format->getDefaultLocationsToSearch();
                sawVst3 = sawVst3 || format->getName() == "VST3";

                for (int i = 0; i < defaults.getNumPaths(); ++i)
                    expect (contains (paths, defaults[i]), format->getName() + " lost its default " + defaults[i].getFullPathName());

                expectEquals (count (paths, a.dir), 1);
                expectEquals (count (paths, b.dir), 1);
                expect (! contains (paths, juce::File::getCurrentWorkingDirectory().getChildFile ("relative/folder")));

               #if JUCE_LINUX
                if (format->getName() == "LADSPA")
                    expect (contains (paths, juce::File ("/usr/lib64/ladspa")), "LADSPA must search /usr/lib64/ladspa");
               #endif
            }

            expect (sawVst3);

            // With the defaults off (tests, dev tools) only the extra folders remain.
            auto settings = manager.getScanSettings();
            settings.useDefaultLocations = false;
            manager.setScanSettings (settings);

            for (auto* format : manager.getFormatManager().getFormats())
            {
                const auto paths = manager.getSearchPaths (*format);
                expectEquals (paths.getNumPaths(), 2);
            }
        }

        beginTest ("plugins.xml round trip keeps plugins and the blacklist");
        {
            TempDir dir;
            const auto file = dir.dir.getChildFile ("plugins.xml");
            const auto bad = dir.dir.getChildFile ("Broken.vst3").getFullPathName();

            {
                PluginManager manager (file);
                manager.getKnownPluginList().addType (makeDescription ("Alpha Comp", "VST3", dir.dir.getChildFile ("Alpha.vst3").getFullPathName()));
                manager.getKnownPluginList().addType (makeDescription ("Beta Verb", "LV2", "urn:test:beta"));
                manager.getKnownPluginList().addToBlacklist (bad);
                manager.savePluginList();
            }

            expect (file.existsAsFile());

            if (auto xml = juce::XmlDocument::parse (file))
            {
                expect (xml->hasTagName ("KNOWNPLUGINS"));
                expect (xml->getChildByName ("BLACKLISTED") != nullptr);
            }
            else
            {
                expect (false, "plugins.xml is not valid XML");
            }

            PluginManager reloaded (file);
            const auto plugins = reloaded.getKnownPlugins();
            expectEquals (plugins.size(), 2);

            juce::StringArray names;

            for (const auto& p : plugins)
                names.add (p.name + "/" + p.pluginFormatName + "/" + p.manufacturerName);

            expect (names.contains ("Alpha Comp/VST3/Test Audio"));
            expect (names.contains ("Beta Verb/LV2/Test Audio"));
            expect (reloaded.getBlacklistedFiles().contains (bad));

            // A file that is not our XML is survived (the next scan rebuilds it), and saving repairs it.
            file.replaceWithText ("<<< this is not xml");
            PluginManager damaged (file);
            expectEquals (damaged.getKnownPlugins().size(), 0);
            damaged.savePluginList();
            expect (juce::XmlDocument::parse (file) != nullptr);
        }

        beginTest ("LV2 scan records survive a save / load");
        {
            TempDir dir;
            const auto file = dir.dir.getChildFile ("plugins.xml");

            juce::XmlElement seeded ("KNOWNPLUGINS");
            auto* record = seeded.createNewChildElement ("SCANNED");
            record->setAttribute ("format", "LV2");
            record->setAttribute ("id", dir.dir.getChildFile ("a.lv2").getFullPathName());
            record->setAttribute ("sig", "1234567");
            expect (seeded.writeTo (file));

            {
                PluginManager manager (file);
                manager.savePluginList();
            }

            auto xml = juce::XmlDocument::parse (file);
            expect (xml != nullptr);

            if (xml != nullptr)
            {
                auto* kept = xml->getChildByName ("SCANNED");
                expect (kept != nullptr);

                if (kept != nullptr)
                {
                    expectEquals (kept->getStringAttribute ("format"), juce::String ("LV2"));
                    expectEquals (kept->getStringAttribute ("sig"), juce::String ("1234567"));
                }
            }
        }

        beginTest ("synths are not offered as effects and cannot be instantiated as one");
        {
            PluginManager manager;
            manager.getKnownPluginList().addType (makeDescription ("A Compressor", "VST3", "C:\\nowhere\\comp.vst3"));
            manager.getKnownPluginList().addType (makeDescription ("A Synth", "VST3", "C:\\nowhere\\synth.vst3", true));

            // What VST3's moduleinfo.json fast path reports: no channel counts at all, for effects too.
            auto unknownChannels = makeDescription ("Fast Path EQ", "VST3", "C:\\nowhere\\eq.vst3");
            unknownChannels.numInputChannels = unknownChannels.numOutputChannels = 0;
            manager.getKnownPluginList().addType (unknownChannels);

            juce::StringArray effects;

            for (const auto& d : manager.getKnownEffects())
                effects.add (d.name);

            expect (effects.contains ("A Compressor"));
            expect (effects.contains ("Fast Path EQ"));
            expect (! effects.contains ("A Synth"));

            juce::String error;
            auto instance = manager.createInstance (makeDescription ("A Synth", "VST3", "C:\\nowhere\\synth.vst3", true), 48000.0, 512, error);
            expect (instance == nullptr);
            expect (error.contains ("synth"), "the error should say why: " + error);

            error.clear();
            instance = manager.createInstance (makeDescription ("A Compressor", "VST3", "C:\\nowhere\\comp.vst3"), 48000.0, 512, error);
            expect (instance == nullptr);
            expect (error.isNotEmpty() && ! error.contains ("synth"), "a missing file is reported as such: " + error);
        }
    }

private:
    static bool contains (const juce::FileSearchPath& paths, const juce::File& folder)
    {
        return count (paths, folder) > 0;
    }

    static int count (const juce::FileSearchPath& paths, const juce::File& folder)
    {
        int n = 0;

        for (int i = 0; i < paths.getNumPaths(); ++i)
            if (paths[i] == folder)
                ++n;

        return n;
    }
};

// ---------------------------------------------------------------------------------------------------
// CrashGuard + startup handling in AppController
// ---------------------------------------------------------------------------------------------------

class CrashGuardTests final : public juce::UnitTest
{
public:
    CrashGuardTests() : juce::UnitTest ("Plugin crash guard", "headroom") {}

    void runTest() override
    {
        beginTest ("crash records are formatted without libc and read back");
        {
            const auto format = [] (uint32_t rt, uint32_t ui)
            {
                char buffer[CrashGuard::kMaxRecordChars];
                const auto length = CrashGuard::formatCrashRecord (buffer, rt, ui);
                return juce::String::fromUTF8 (buffer, (int) length);
            };

            expectEquals (format (0, 0), juce::String ("0 0\n"));
            expectEquals (format (17, 0), juce::String ("17 0\n"));
            expectEquals (format (0, 42), juce::String ("0 42\n"));
            expectEquals (format (4294967295u, 1234567890u), juce::String ("4294967295 1234567890\n"));

            TempDir dir;
            const auto file = dir.dir.getChildFile ("crash.txt");
            file.replaceWithText (format (17, 0) + format (17, 4) + format (0, 0) + "garbage 9\n");

            const auto ids = CrashGuard::readAndClearLastCrash (file);
            expectEquals ((int) ids.size(), 3);                     // 17, 4, 9: de-duplicated, zeros and garbage dropped
            expect (ids.size() == 3 && ids[0] == 17 && ids[1] == 4 && ids[2] == 9);
            expect (! file.existsAsFile(), "reading crash.txt consumes it");
            expect (CrashGuard::readAndClearLastCrash (file).empty());
        }

        beginTest ("the UI slot scope nests and restores");
        {
            expectEquals ((int) CrashGuard::currentUiSlot.load(), 0);

            {
                CrashGuard::ScopedUiSlot outer (5);
                expectEquals ((int) CrashGuard::currentUiSlot.load(), 5);

                {
                    CrashGuard::ScopedUiSlot inner (6);
                    expectEquals ((int) CrashGuard::currentUiSlot.load(), 6);
                }

                expectEquals ((int) CrashGuard::currentUiSlot.load(), 5);
            }

            expectEquals ((int) CrashGuard::currentUiSlot.load(), 0);
        }

        beginTest ("after a crash the named slot is disabled, a banner offers to turn it back on");
        {
            TempDir root;
            ScopedRoot scope (root.dir);

            AppController::Options options;
            options.demo = true;
            options.startBackend = false;
            options.stateFile = root.dir.getChildFile ("state.xml");

            juce::String crashyUid, innocentUid;
            uint32_t crashyId = 0;

            {
                AppController controller (options);
                controller.start();

                crashyUid = controller.addPluginFx ("music", makeDescription ("Crashy Comp", "VST3", root.dir.getChildFile ("crashy.vst3").getFullPathName()));
                innocentUid = controller.addPluginFx ("music", makeDescription ("Fine EQ", "VST3", root.dir.getChildFile ("fine.vst3").getFullPathName()));

                expect (crashyUid.isNotEmpty() && innocentUid.isNotEmpty());
                crashyId = AppState::describeSlot (controller.getState().findSlot (crashyUid)).crashId;
                expect (crashyId != 0);
                expect (controller.getNotices().empty(), "a clean start posts no notices");
            }

            // The previous run died inside "Crashy Comp" (RT thread) - and a slot id nobody has any more.
            Paths::getCrashFile().replaceWithText (juce::String (crashyId) + " 0\n9999 0\n");

            AppController controller (options);
            controller.start();
            auto& state = controller.getState();

            expect (! Paths::getCrashFile().existsAsFile(), "crash.txt is consumed");
            expect (AppState::describeSlot (state.findSlot (crashyUid)).disabled);
            expect (! AppState::describeSlot (state.findSlot (innocentUid)).disabled);
            expectEquals ((int) controller.getNotices().size(), 1);

            if (! controller.getNotices().empty())
            {
                const auto notice = controller.getNotices().front();
                expectEquals (notice.text, juce::String ("Crashy Comp crashed and was turned off."));
                expectEquals (notice.actionLabel, juce::String ("Turn back on"));
                expect (notice.action != nullptr);
                expect (notice.severity == Notice::Severity::warning);

                controller.runNoticeAction (notice.id);
                expect (! AppState::describeSlot (state.findSlot (crashyUid)).disabled);
                expect (controller.getNotices().empty(), "the banner goes away once acted on");
            }
        }

       #if JUCE_LINUX
        beginTest ("the signal handler records both slot ids and the process still dies of the signal");
        {
            const auto executable = workerExecutable();

            if (! executable.existsAsFile())
            {
                expect (false, "headroom executable not found next to the tests: " + executable.getFullPathName());
            }
            else
            {
                TempDir dir;
                const auto file = dir.dir.getChildFile ("crash.txt");

                juce::StringArray args;
                args.add (executable.getFullPathName());
                args.add ("--dev-crash-selftest");
                args.add (file.getFullPathName());
                args.add ("17");
                args.add ("42");

                juce::ChildProcess child;
                expect (child.start (args, 0));
                expect (child.waitForProcessToFinish (20000));

                expectEquals (file.loadFileAsString(), juce::String ("17 42\n"));
                const auto ids = CrashGuard::readAndClearLastCrash (file);
                expect (ids.size() == 2 && ids[0] == 17 && ids[1] == 42);
            }
        }
       #endif
    }
};

// ---------------------------------------------------------------------------------------------------
// The out-of-process scanner
// ---------------------------------------------------------------------------------------------------

class PluginScannerTests final : public juce::UnitTest
{
public:
    PluginScannerTests() : juce::UnitTest ("Plugin scanner", "headroom") {}

    void runTest() override
    {
        beginTest ("job files round trip, including empty fields");
        {
            std::vector<scan::WireJob> jobs;
            jobs.push_back ({ scan::WireJob::Kind::search, "VST3", "C:\\a b;C:\\c", true });
            jobs.push_back ({ scan::WireJob::Kind::search, "LV2", "", false });
            jobs.push_back ({ scan::WireJob::Kind::scan, "LADSPA", "/usr/lib64/ladspa/amp_1181.so", true });

            std::vector<scan::WireJob> decoded;
            bool hooks = true;
            expect (scan::decodeJobs (scan::encodeJobs (jobs, false), decoded, hooks));
            expect (! hooks);
            expectEquals ((int) decoded.size(), 3);

            if (decoded.size() == 3)
            {
                expect (decoded[0].kind == scan::WireJob::Kind::search && decoded[0].argument == "C:\\a b;C:\\c" && decoded[0].recursive);
                expect (decoded[1].kind == scan::WireJob::Kind::search && decoded[1].argument.isEmpty() && ! decoded[1].recursive);
                expect (decoded[2].kind == scan::WireJob::Kind::scan && decoded[2].format == "LADSPA" && decoded[2].argument.endsWith ("amp_1181.so"));
            }

            expect (scan::decodeJobs (scan::encodeJobs (jobs, true), decoded, hooks) && hooks);
            expect (! scan::decodeJobs ("something else\n", decoded, hooks));
            expect (! scan::isWireSafe ("a\tb") && ! scan::isWireSafe ("a\nb") && scan::isWireSafe ("C:\\Program Files\\x.vst3"));
        }

        const auto worker = workerExecutable();

        if (! worker.existsAsFile())
        {
            beginTest ("scanner tests need the headroom executable");
            expect (false, "headroom executable not found next to the tests: " + worker.getFullPathName());
            return;
        }

        TempDir dir;
        const auto missing = dir.dir.getChildFile ("missing.vst3");
        const auto garbage = dir.dir.getChildFile ("garbage.vst3");
        const auto after = dir.dir.getChildFile ("sub").getChildFile ("after.vst3");
        garbage.replaceWithText ("this is not a plugin, just some text");
        after.getParentDirectory().createDirectory();
        after.replaceWithText ("neither is this");

        ScanRunner::Settings settings;
        settings.workerExecutable = worker;
        settings.itemTimeoutMs = 3000;
        settings.testHooks = true;

        beginTest ("search finds candidate files in the extra folder, recursively");
        {
            ScanRunner scanner (settings);
            const auto result = scanner.search ({ { "VST3", juce::FileSearchPath (dir.dir.getFullPathName()), true } });

            expect (result.complete, result.error);
            const auto found = result.files.find ("VST3");
            expect (found != result.files.end());

            if (found != result.files.end())
            {
                expect (found->second.contains (garbage.getFullPathName()));
                expect (found->second.contains (after.getFullPathName()));
                expect (! found->second.contains (missing.getFullPathName()));
            }

            // An unknown format or an empty path is an empty answer, not an error.
            const auto nothing = scanner.search ({ { "NoSuchFormat", juce::FileSearchPath (dir.dir.getFullPathName()), true },
                                                  { "VST3", juce::FileSearchPath(), true } });
            expect (nothing.complete);
        }

        beginTest ("scanning a missing file and garbage returns nothing, quickly, and blacklists nothing");
        {
            ScanRunner scanner (settings);
            std::vector<ScanRunner::Outcome> outcomes;
            int plugins = 0;

            ScanRunner::Callbacks callbacks;
            callbacks.finished = [&] (size_t, const ScanJob&, ScanRunner::Outcome outcome, ScanRunner::Found& found)
            {
                outcomes.push_back (outcome);
                plugins += found.size();
            };

            const auto started = juce::Time::getMillisecondCounter();
            const auto report = scanner.scan ({ { "VST3", missing.getFullPathName() },
                                               { "VST3", garbage.getFullPathName() },
                                               { "VST3", "" },
                                               { "NoSuchFormat", "whatever" } },
                                             callbacks);
            const auto elapsed = juce::Time::getMillisecondCounter() - started;

            expect (report.completed, report.error);
            expectEquals ((int) outcomes.size(), 4);
            expect (std::all_of (outcomes.begin(), outcomes.end(), [] (auto o) { return o == ScanRunner::Outcome::scanned; }));
            expectEquals (plugins, 0);
            expect (elapsed < 15000, "took " + juce::String (elapsed) + " ms");
        }

        beginTest ("a crash is retried alone, then reported; a hang is killed at the timeout; the scan goes on");
        {
            ScanRunner scanner (settings);
            std::vector<juce::String> outcomes (5);
            std::vector<int> starts (5, 0);

            const std::vector<ScanJob> jobs { { "VST3", missing.getFullPathName() },
                                              { "VST3", "hr-test:crash" },
                                              { "VST3", garbage.getFullPathName() },
                                              { "VST3", "hr-test:hang" },
                                              { "VST3", after.getFullPathName() } };

            ScanRunner::Callbacks callbacks;
            callbacks.started = [&] (size_t index, const ScanJob&) { ++starts[index]; };
            callbacks.finished = [&] (size_t index, const ScanJob&, ScanRunner::Outcome outcome, ScanRunner::Found&)
            {
                outcomes[index] = outcome == ScanRunner::Outcome::scanned ? "scanned"
                                : outcome == ScanRunner::Outcome::crashed ? "crashed" : "timedOut";
            };

            const auto started = juce::Time::getMillisecondCounter();
            const auto report = scanner.scan (jobs, callbacks);
            const auto elapsed = juce::Time::getMillisecondCounter() - started;

            expect (report.completed, report.error);
            expectEquals (outcomes[0], juce::String ("scanned"));
            expectEquals (outcomes[1], juce::String ("crashed"));
            expectEquals (outcomes[2], juce::String ("scanned"));
            expectEquals (outcomes[3], juce::String ("timedOut"));
            expectEquals (outcomes[4], juce::String ("scanned"));
            expectEquals (starts[1], 2);        // the crash was retried once, alone
            expectEquals (starts[3], 1);        // a hang is not retried
            expect (elapsed >= 3000, "the hang cannot have been cut short: " + juce::String (elapsed));
            expect (elapsed < 30000, "took " + juce::String (elapsed) + " ms");
        }

        beginTest ("no scanner executable, or one that is not a scanner, ends the scan with an error instead of looping");
        {
            ScanRunner::Settings broken = settings;
            broken.workerExecutable = dir.dir.getChildFile ("not-there.exe");

            {
                ScanRunner scanner (broken);
                int finished = 0;
                ScanRunner::Callbacks callbacks;
                callbacks.finished = [&] (size_t, const ScanJob&, ScanRunner::Outcome, ScanRunner::Found&) { ++finished; };

                const auto report = scanner.scan ({ { "VST3", garbage.getFullPathName() } }, callbacks);
                expect (! report.completed && report.error.isNotEmpty());
                expectEquals (finished, 0);
            }

           #if JUCE_WINDOWS
            broken.workerExecutable = juce::File::getSpecialLocation (juce::File::windowsSystemDirectory).getChildFile ("hostname.exe");
           #else
            broken.workerExecutable = juce::File ("/bin/true");
           #endif

            if (broken.workerExecutable.existsAsFile())
            {
                ScanRunner scanner (broken);
                int finished = 0;
                ScanRunner::Callbacks callbacks;
                callbacks.finished = [&] (size_t, const ScanJob&, ScanRunner::Outcome, ScanRunner::Found&) { ++finished; };

                const auto report = scanner.scan ({ { "VST3", garbage.getFullPathName() } }, callbacks);
                expect (! report.completed && report.error.isNotEmpty(), "gave up: " + report.error);
                expectEquals (finished, 0);
            }
        }

        beginTest ("KnownPluginList::scanAndAddFile goes through the same worker");
        {
            juce::KnownPluginList list;
            list.setCustomScanner (std::make_unique<OutOfProcessScanner> (settings));

            juce::AudioPluginFormatManager formats;
            juce::addDefaultFormatsToManager (formats);
            juce::AudioPluginFormat* vst3 = nullptr;

            for (auto* format : formats.getFormats())
                if (format->getName() == "VST3")
                    vst3 = format;

            expect (vst3 != nullptr);

            if (vst3 != nullptr)
            {
                juce::OwnedArray<juce::PluginDescription> found;
                expect (! list.scanAndAddFile (garbage.getFullPathName(), false, found, *vst3));
                expectEquals (found.size(), 0);
                expect (list.getBlacklistedFiles().isEmpty(), "garbage is not a crash");

                list.scanAndAddFile ("hr-test:crash", false, found, *vst3);
                expect (list.getBlacklistedFiles().contains ("hr-test:crash"), "a crash blacklists the file");
            }
        }

        beginTest ("PluginManager: background scan, blacklist, persistence, rescan");
        {
            TempDir state;
            const auto listFile = state.dir.getChildFile ("plugins.xml");
            const auto pedal = state.dir.getChildFile ("plugin-scan-deadmans.txt");

            PluginManager::ScanSettings managerSettings;
            managerSettings.workerExecutable = worker;
            managerSettings.itemTimeoutMs = 20000;
            managerSettings.useDefaultLocations = false;
            managerSettings.testHooks = true;
            managerSettings.extraJobs = { { "VST3", "hr-test:crash" } };

            {
                PluginManager manager (listFile, pedal);
                manager.setScanSettings (managerSettings);
                manager.setExtraFolders ({ dir.dir.getFullPathName() });

                int listChanges = 0;
                manager.onPluginListChanged = [&] { ++listChanges; };

                manager.startBackgroundScan (true);
                expect (manager.isScanning());
                expect (waitUntil ([&] { return ! manager.isScanning(); }, 60000), "the scan should finish");

                const auto& stats = manager.getLastScanStats();
                expect (stats.completed, stats.error);
                expectEquals (stats.filesFound, 2);                     // garbage.vst3 and sub/after.vst3
                expectEquals (stats.filesChecked, 3);                   // + the crash job
                expectEquals (stats.crashed, 1);
                expectEquals (stats.pluginsAdded, 0);
                expectEquals (manager.getScanProgress(), 1.0f);
                expect (manager.getScanStatusText().isEmpty());
                expect (listChanges > 0);
                expect (manager.getBlacklistedFiles().contains ("hr-test:crash"));
                expect (! pedal.existsAsFile(), "the pedal is cleared once the file is dealt with");
                expect (listFile.existsAsFile(), "the list is saved after a scan");
            }

            {
                // A new run remembers the blacklist and does not send blacklisted files to the scanner again.
                PluginManager manager (listFile, pedal);
                manager.setScanSettings ({ worker, 20000, false, true, {} });
                manager.setExtraFolders ({ dir.dir.getFullPathName() });
                expect (manager.getBlacklistedFiles().contains ("hr-test:crash"));

                manager.getKnownPluginList().addToBlacklist (garbage.getFullPathName());
                manager.startBackgroundScan (false);
                expect (waitUntil ([&] { return ! manager.isScanning(); }, 60000));

                const auto& stats = manager.getLastScanStats();
                expectEquals (stats.skippedBlacklisted, 1);
                expectEquals (stats.filesChecked, 1);                   // only sub/after.vst3

                // "Rescan all" forgets the blacklist.
                manager.startBackgroundScan (true);
                expect (waitUntil ([&] { return ! manager.isScanning(); }, 60000));
                expect (manager.getBlacklistedFiles().isEmpty());
                expectEquals (manager.getLastScanStats().filesChecked, 2);
            }

            {
                // The pedal left by a run that died mid-scan: logged and retried, not blacklisted (see startBackgroundScan).
                PluginManager manager (listFile, pedal);
                manager.setScanSettings ({ worker, 20000, false, true, {} });
                manager.setExtraFolders ({ dir.dir.getFullPathName() });
                pedal.replaceWithText (garbage.getFullPathName());

                manager.startBackgroundScan (false);
                expect (waitUntil ([&] { return ! manager.isScanning(); }, 60000));
                expect (manager.getBlacklistedFiles().isEmpty());
                expect (! pedal.existsAsFile());
            }
        }

        beginTest ("stopping a scan kills a hung worker promptly");
        {
            TempDir state;
            PluginManager manager (state.dir.getChildFile ("plugins.xml"), state.dir.getChildFile ("pedal.txt"));

            PluginManager::ScanSettings managerSettings;
            managerSettings.workerExecutable = worker;
            managerSettings.itemTimeoutMs = 60000;
            managerSettings.useDefaultLocations = false;
            managerSettings.testHooks = true;
            managerSettings.extraJobs = { { "VST3", "hr-test:hang" } };
            manager.setScanSettings (managerSettings);

            manager.startBackgroundScan (false);
            expect (waitUntil ([&] { return manager.getScanStatusText().startsWith ("Checking"); }, 30000), "the worker should reach the hung job");

            const auto started = juce::Time::getMillisecondCounter();
            manager.stopScan();
            expect (juce::Time::getMillisecondCounter() - started < 8000, "stopScan must not wait for the plugin");
            expect (! manager.isScanning());
            expect (manager.getBlacklistedFiles().isEmpty(), "an abandoned scan blames nobody");
        }
    }
};

// ---------------------------------------------------------------------------------------------------
// FxSlot layouts and editor windows
// ---------------------------------------------------------------------------------------------------

class PluginHostingTests final : public juce::UnitTest
{
public:
    PluginHostingTests() : juce::UnitTest ("Plugin hosting", "headroom") {}

    // New slots play dry for their latency + 40 ms and then crossfade in (FxSlot warm-up), so feed ~270 ms of the
    // same constant input before looking at the output.
    static void processSettled (FxSlot& slot, std::vector<float>& l, std::vector<float>& r, float lIn, float rIn)
    {
        for (int block = 0; block < 200; ++block)
        {
            std::fill (l.begin(), l.end(), lIn);
            std::fill (r.begin(), r.end(), rIn);
            slot.process (l.data(), r.data(), (int) l.size());
        }
    }

    void runTest() override
    {
        beginTest ("stereo effect: both channels processed, CrashGuard sees the slot during processBlock");
        {
            auto plugin = std::make_shared<FakePlugin> (FakePlugin::Kind::stereo, 0.5f);
            FxSlot slot (plugin, "uid-stereo", 41);
            slot.configure (48000.0, kMaxBlock);

            expectEquals (slot.getNumInputChannels(), 2);
            expectEquals (slot.getNumOutputChannels(), 2);

            std::vector<float> l (64, 1.0f), r (64, -0.5f);
            processSettled (slot, l, r, 1.0f, -0.5f);
            expectWithinAbsoluteError (l[10], 0.5f, 1.0e-6f);
            expectWithinAbsoluteError (r[10], -0.25f, 1.0e-6f);
            expectEquals ((int) plugin->seenRtSlot.load(), 41);
            expectEquals ((int) CrashGuard::currentRtSlot.load(), 0);
        }

        beginTest ("mono-only effect: L is processed, the result feeds both channels");
        {
            auto plugin = std::make_shared<FakePlugin> (FakePlugin::Kind::monoOnly, 0.5f);
            FxSlot slot (plugin, "uid-mono", 42);
            slot.configure (48000.0, kMaxBlock);

            expectEquals (slot.getNumInputChannels(), 1);
            expectEquals (slot.getNumOutputChannels(), 1);

            std::vector<float> l (64, 1.0f), r (64, 0.25f);
            processSettled (slot, l, r, 1.0f, 0.25f);
            expectWithinAbsoluteError (l[3], 0.5f, 1.0e-6f);
            expectWithinAbsoluteError (r[3], 0.5f, 1.0e-6f);
        }

        beginTest ("a sidechain input is switched off, the plugin still sees only the main bus");
        {
            auto plugin = std::make_shared<FakePlugin> (FakePlugin::Kind::sidechain, 0.5f);
            expectEquals (plugin->getBusCount (true), 2);
            FxSlot slot (plugin, "uid-side", 43);
            slot.configure (48000.0, kMaxBlock);

            expect (! plugin->getBus (true, 1)->isEnabled(), "the sidechain bus should be disabled");
            expectEquals (slot.getNumInputChannels(), 2);

            std::vector<float> l (64, 1.0f), r (64, 1.0f);
            processSettled (slot, l, r, 1.0f, 1.0f);
            expectWithinAbsoluteError (l[0], 0.5f, 1.0e-6f);
            expectEquals (plugin->lastNumChannels, 2);
        }

        beginTest ("a synth leaves the channel's signal alone");
        {
            auto plugin = std::make_shared<FakePlugin> (FakePlugin::Kind::instrument);
            FxSlot slot (plugin, "uid-synth", 44);
            slot.configure (48000.0, kMaxBlock);
            expectEquals (slot.getNumInputChannels(), 0);

            std::vector<float> l (64, 0.7f), r (64, 0.7f);
            slot.process (l.data(), r.data(), 64);
            expectWithinAbsoluteError (l[5], 0.7f, 1.0e-6f);
            expectWithinAbsoluteError (r[5], 0.7f, 1.0e-6f);
        }

        beginTest ("re-configuring at another sample rate works (rate change)");
        {
            auto plugin = std::make_shared<FakePlugin> (FakePlugin::Kind::stereo, 0.5f);
            FxSlot slot (plugin, "uid-rate", 45);
            slot.configure (48000.0, kMaxBlock);
            slot.configure (44100.0, kMaxBlock);
            expect (slot.isConfigured());
            expectEquals (slot.getNumInputChannels(), 2);
        }

        beginTest ("editor windows: generic fallback, title, remembered position, closed before the processor");
        {
            if (! canShowWindows())
            {
                logMessage ("skipped: no display");
            }
            else
            {
                TempDir state;
                const auto listFile = state.dir.getChildFile ("plugins.xml");
                auto plugin = std::make_shared<FakePlugin> (FakePlugin::Kind::stereo);

                {
                    PluginManager manager (listFile);
                    manager.openEditor ("uid-1", *plugin, "Fake plugin - Mic", 7);

                    expect (manager.isEditorOpen ("uid-1"));
                    auto* window = manager.getEditorWindow ("uid-1");
                    expect (window != nullptr);

                    if (window != nullptr)
                    {
                        expectEquals (window->getName(), juce::String ("Fake plugin - Mic"));
                        expect (window->isShowingGenericEditor());
                        expect (window->isUsingNativeTitleBar());
                        expectEquals (window->getDesktopScaleFactor(), 1.0f);
                        expect (window->isVisible());

                        // Opening again just raises it.
                        manager.openEditor ("uid-1", *plugin, "ignored", 7);
                        expect (manager.getEditorWindow ("uid-1") == window);

                        window->setTopLeftPosition (120, 90);
                    }

                    manager.closeEditor ("uid-1");
                    expect (! manager.isEditorOpen ("uid-1"));
                    expectEquals ((int) CrashGuard::currentUiSlot.load(), 0);

                    const auto remembered = manager.getRememberedEditorBounds ("uid-1");
                    expect (remembered.has_value());

                    if (remembered.has_value())
                        expect (remembered->getX() == 120 && remembered->getY() == 90, "remembered " + remembered->toString());

                    // Left open: the manager destructor closes it before the plugin goes.
                    manager.openEditor ("uid-2", *plugin);
                    expect (manager.isEditorOpen ("uid-2"));
                }

                expect (listFile.getSiblingFile ("plugin-windows.xml").existsAsFile());

                PluginManager next (listFile);
                const auto restored = next.getRememberedEditorBounds ("uid-1");
                expect (restored.has_value() && restored->getX() == 120 && restored->getY() == 90);

                next.openEditor ("uid-1", *plugin);

                if (auto* window = next.getEditorWindow ("uid-1"))
                    expect (std::abs (window->getX() - 120) <= 2 && std::abs (window->getY() - 90) <= 2,
                            "reopened at " + window->getPosition().toString());

                next.closeAllEditors();
                expect (! next.isEditorOpen ("uid-1"));
            }
        }

        beginTest ("a remembered position that is off every display is not used");
        {
            expect (PluginWindow::isPositionUsable ({ 100, 100, 400, 300 }) || ! canShowWindows());
            expect (! PluginWindow::isPositionUsable ({ 400000, 400000, 400, 300 }));
            expect (! PluginWindow::isPositionUsable ({}));
        }
    }
};

// ---------------------------------------------------------------------------------------------------
// Real plugins on this machine. Skipped unless HEADROOM_TEST_REAL_PLUGINS is set: it loads whatever the user has
// installed (HEADROOM_TEST_SNAPSHOT_DIR says where the editor screenshot goes). A manual check, not part of CI.
// ---------------------------------------------------------------------------------------------------

class RealPluginTests final : public juce::UnitTest
{
public:
    RealPluginTests() : juce::UnitTest ("Real plugins", "headroom-real") {}

    void runTest() override
    {
        if (juce::SystemStats::getEnvironmentVariable ("HEADROOM_TEST_REAL_PLUGINS", {}).isEmpty())
            return;

        beginTest ("scan, chain, process, editor, snapshot, clean shutdown");

        AppController::Options options;
        options.demo = true;
        options.persist = false;
        options.startBackend = false;

        AppController controller (options);
        controller.start();

        auto& plugins = controller.getPlugins();
        PluginManager::ScanSettings settings;
        settings.workerExecutable = workerExecutable();
        plugins.setScanSettings (settings);

        plugins.startBackgroundScan (true);
        expect (waitUntil ([&] { return ! plugins.isScanning(); }, 180000), "scan should finish");

        const auto& stats = plugins.getLastScanStats();
        logMessage ("scan: " + juce::String (stats.filesFound) + " files, " + juce::String (stats.pluginsAdded) + " plugins, "
                    + juce::String (stats.crashed) + " crashed, " + juce::String (stats.timedOut) + " timed out, "
                    + juce::String (stats.seconds, 2) + " s");

        for (const auto& d : plugins.getKnownPlugins())
            logMessage ("  found " + d.pluginFormatName + " " + d.name + " [" + d.manufacturerName + "] "
                        + (d.isInstrument ? "instrument" : "effect") + " in=" + juce::String (d.numInputChannels)
                        + " out=" + juce::String (d.numOutputChannels));

        for (const auto& file : plugins.getBlacklistedFiles())
            logMessage ("  skipped " + file);

        const auto effects = plugins.getKnownEffects();

        if (effects.isEmpty())
        {
            logMessage ("no effect plugins installed: nothing more to check");
            return;
        }

        // Up to three, straight into the "music" channel's chain (engine slot 2).
        juce::StringArray uids;

        for (int i = 0; i < juce::jmin (3, effects.size()); ++i)
        {
            const auto uid = controller.addPluginFx ("music", effects[i]);
            uids.add (uid);
            const auto error = controller.getFxError (uid);
            logMessage ("  chain slot " + juce::String (i) + ": " + effects[i].name + (error.isEmpty() ? " loaded" : " FAILED: " + error));

            if (auto* processor = controller.getFxProcessor (uid))
                logMessage ("      layout in=" + juce::String (processor->getTotalNumInputChannels()) + " out="
                            + juce::String (processor->getTotalNumOutputChannels()) + " buses in/out="
                            + juce::String (processor->getBusCount (true)) + "/" + juce::String (processor->getBusCount (false))
                            + " editor=" + (processor->hasEditor() ? "yes" : "no") + " latency=" + juce::String (processor->getLatencySamples()));
        }

        auto chain = controller.getEngine().getChain (2);
        expect (chain != nullptr);
        logMessage ("chain has " + juce::String (chain != nullptr ? (int) chain->getSlots().size() : 0) + " live plugin(s)");

        // "The RT thread": a test thread pushing a tone through the engine at real-time pace.
        struct Pump final : juce::Thread
        {
            explicit Pump (AudioEngine& e) : juce::Thread ("test RT"), engine (e) {}

            void run() override
            {
                constexpr int frames = 480;
                std::vector<float> in (frames), monL (frames), monR (frames), strL (frames), strR (frames);
                juce::Random noise;
                double phase = 0.0;

                while (! threadShouldExit())
                {
                    double inSum = 0.0, outSum = 0.0;

                    for (int i = 0; i < frames; ++i)
                    {
                        phase += 6.283185307179586 * 440.0 / 48000.0;
                        in[(size_t) i] = 0.25f * (float) std::sin (phase) + 0.02f * (noise.nextFloat() - 0.5f);
                        inSum += (double) in[(size_t) i] * (double) in[(size_t) i];
                    }

                    EngineIO io;
                    io.numFrames = frames;
                    io.sampleRate = 48000.0;
                    io.appIn[2][0] = in.data();
                    io.appIn[2][1] = in.data();
                    io.monitorOut[0] = monL.data();  io.monitorOut[1] = monR.data();
                    io.streamOut[0] = strL.data();   io.streamOut[1] = strR.data();
                    engine.process (io);

                    for (int i = 0; i < frames; ++i)
                    {
                        const auto v = monL[(size_t) i];
                        outSum += (double) v * (double) v;

                        if (! std::isfinite (v))
                            nonFinite = true;
                    }

                    inEnergy = inEnergy + inSum;
                    outEnergy = outEnergy + outSum;
                    ++blocks;
                    wait (10);
                }
            }

            AudioEngine& engine;
            std::atomic<double> inEnergy { 0.0 }, outEnergy { 0.0 };
            std::atomic<int> blocks { 0 };
            std::atomic<bool> nonFinite { false };
        };

        Pump pump (controller.getEngine());
        pump.startThread (juce::Thread::Priority::high);

        // The first plugin that has an editor of its own gets a window; a generic one otherwise.
        juce::String editorUid = uids.isEmpty() ? juce::String() : uids[0];

        for (const auto& uid : uids)
            if (auto* processor = controller.getFxProcessor (uid); processor != nullptr && processor->hasEditor())
            {
                editorUid = uid;
                break;
            }

        if (canShowWindows() && editorUid.isNotEmpty() && controller.getFxProcessor (editorUid) != nullptr)
        {
            controller.openFxEditor ("music", editorUid);
            expect (plugins.isEditorOpen (editorUid));
            waitUntil ([] { return false; }, 2500);         // let the editor come up and paint

            if (auto* window = plugins.getEditorWindow (editorUid))
            {
                logMessage ("editor window \"" + window->getName() + "\" " + window->getBounds().toString()
                            + (window->isShowingGenericEditor() ? " (generic)" : " (the plugin's own)"));

                const auto image = window->createComponentSnapshot (window->getLocalBounds(), true, 1.0f);
                auto directory = juce::File (juce::SystemStats::getEnvironmentVariable ("HEADROOM_TEST_SNAPSHOT_DIR", {}));

                if (directory == juce::File() || ! directory.isDirectory())
                    directory = juce::File::getSpecialLocation (juce::File::tempDirectory);

                const auto png = directory.getChildFile ("headroom-plugin-editor.png");
                png.deleteFile();

                if (juce::FileOutputStream stream (png); stream.openedOk())
                    juce::PNGImageFormat().writeImageToStream (image, stream);

                logMessage ("snapshot " + juce::String (image.getWidth()) + "x" + juce::String (image.getHeight()) + " -> " + png.getFullPathName());
                expect (image.isValid() && image.getWidth() > 50 && image.getHeight() > 50);

               #if JUCE_WINDOWS
                if (! window->isShowingGenericEditor())
                {
                    const auto native = captureNativeWindow (*window);
                    const auto nativePng = directory.getChildFile ("headroom-plugin-editor-native.png");
                    nativePng.deleteFile();

                    if (native.isValid())
                        if (juce::FileOutputStream stream (nativePng); stream.openedOk())
                            juce::PNGImageFormat().writeImageToStream (native, stream);

                    logMessage ("native snapshot " + juce::String (native.getWidth()) + "x" + juce::String (native.getHeight()) + " -> " + nativePng.getFullPathName());
                }
               #endif
            }
        }

        // Toggle bypass and save the plugin states while audio runs (what the 5 s state capture does).
        if (! uids.isEmpty())
        {
            controller.setFxBypassed ("music", uids[0], true);
            waitUntil ([] { return false; }, 200);
            controller.setFxBypassed ("music", uids[0], false);
        }

        controller.captureFxStates (false);
        waitUntil ([] { return false; }, 1500);

        pump.stopThread (5000);

        const auto blocks = juce::jmax (1, pump.blocks.load());
        const auto inRms = std::sqrt (pump.inEnergy.load() / (480.0 * blocks));
        const auto outRms = std::sqrt (pump.outEnergy.load() / (480.0 * blocks));
        logMessage ("audio: " + juce::String (pump.blocks.load()) + " blocks, RMS in " + juce::String (inRms, 4) + " out " + juce::String (outRms, 4));
        expect (pump.blocks.load() > 20);
        expect (! pump.nonFinite.load(), "the chain produced NaN / inf");

        // Everything closes cleanly: editors first, then the processors.
        plugins.closeAllEditors();

        for (const auto& uid : uids)
            controller.removeFx ("music", uid);

        chain = nullptr;
        waitUntil ([] { return false; }, 400);      // the engine's garbage collection releases the processors
        expect (! plugins.isEditorOpen (editorUid));
    }
};

static PluginListTests pluginListTests;
static CrashGuardTests crashGuardTests;
static PluginScannerTests pluginScannerTests;
static PluginHostingTests pluginHostingTests;
static RealPluginTests realPluginTests;
}
