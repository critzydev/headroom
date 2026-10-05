// Unit tests for the experimental-hardware opt-in: off by default, an old state.xml gets that default, switching it
// on reaches the backend (also when it was saved), and the backend interface and the demo backend say nothing of it until then.
#include "Juce.h"

#include "app/AppController.h"
#include "app/Paths.h"
#include "backend/NullBackend.h"

#include <memory>

namespace hr
{
namespace
{
    struct TempDir
    {
        TempDir() : dir (juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("headroom-exptest", "", false))
        {
            dir.createDirectory();
        }

        ~TempDir()      { dir.deleteRecursively(); }

        juce::File dir;
    };

    struct ScopedRoot
    {
        explicit ScopedRoot (const juce::File& root)    { Paths::setRootOverride (root); }
        ~ScopedRoot()                                   { Paths::setRootOverride ({}); }
    };

    AppController::Options demoOptions()
    {
        AppController::Options options;
        options.demo = true;            // NullBackend, never started: no threads
        options.persist = false;
        options.startBackend = false;
        return options;
    }

    NullBackend& nullBackend (AppController& controller)
    {
        auto* backend = dynamic_cast<NullBackend*> (&controller.getBackend());
        jassert (backend != nullptr);
        return *backend;
    }
}

class ExperimentalOptInTests final : public juce::UnitTest
{
public:
    ExperimentalOptInTests() : juce::UnitTest ("Experimental hardware opt-in", "headroom") {}

    void runTest() override
    {
        beginTest ("off by default, and a state.xml from before it gets that default");
        {
            AppState fresh;
            expectEquals ((int) fresh.getSetting (ids::experimentalHardware), 0);

            TempDir root;
            ScopedRoot scope (root.dir);
            const auto file = root.dir.getChildFile ("state.xml");
            file.replaceWithText ("<HEADROOM version=\"1\"><SETTINGS latency=\"128\" micGain=\"0.75\"/></HEADROOM>");

            AppState old (file);
            expect (old.load() == AppState::LoadResult::Loaded);
            expectEquals ((int) old.getSetting (ids::latency), 128);
            expectEquals ((int) old.getSetting (ids::experimentalHardware), 0);

            old.setSetting (ids::experimentalHardware, 1);
            old.saveNow();

            AppState again (file);
            again.load();
            expectEquals ((int) again.getSetting (ids::experimentalHardware), 1);
        }

        beginTest ("the backend hears about it: nothing at first, then what the user chose");
        {
            AppController controller (demoOptions());
            controller.start();
            auto& backend = nullBackend (controller);

            expect (! controller.getExperimentalHardware());
            expect (! backend.getExperimentalHardware());
            expect (controller.getHardwareControls().experimental.model.isEmpty());     // the demo has no model it applies to

            controller.setExperimentalHardware (true);
            expect (controller.getExperimentalHardware());
            expect (backend.getExperimentalHardware());
            expectEquals ((int) controller.getState().getSetting (ids::experimentalHardware), 1);

            controller.setExperimentalHardware (false);
            expect (! controller.getExperimentalHardware());
            expect (! backend.getExperimentalHardware());
        }

        beginTest ("a saved opt-in reaches the backend at start-up");
        {
            TempDir root;
            ScopedRoot scope (root.dir);
            const auto file = root.dir.getChildFile ("state.xml");
            file.replaceWithText ("<HEADROOM version=\"1\"><SETTINGS experimentalHardware=\"1\"/></HEADROOM>");

            auto options = demoOptions();
            options.persist = true;
            options.stateFile = file;
            AppController controller (options);
            controller.start();

            expect (controller.getExperimentalHardware());
            expect (nullBackend (controller).getExperimentalHardware());
        }
    }
};

static ExperimentalOptInTests experimentalOptInTests;
}
