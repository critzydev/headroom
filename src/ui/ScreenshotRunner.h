// ScreenshotRunner: `headroom --screenshot <dir>`. Runs the demo backend, walks the UI through its
// views and writes PNGs at 1x and 2x. It also doubles as the UI smoke test: every view and the drawer are exercised.
#pragma once

#include "Juce.h"
#include "app/AppController.h"
#include "app/Install.h"
#include "app/InstallManager.h"
#include "ui/MainComponent.h"

#include <functional>
#include <memory>
#include <vector>

namespace hr
{
class ScreenshotRunner final : private juce::Timer
{
public:
    ScreenshotRunner (juce::File outputDirectory, std::function<void (bool ok)> whenFinished);
    ~ScreenshotRunner() override;

    void start();

private:
    struct Step
    {
        Step (juce::String name, int settle, std::function<void()> action, juce::Point<int> windowSize = { 1120, 700 },
              std::function<bool()> ready = {})
            : file (std::move (name)), settleMs (settle), setUp (std::move (action)), size (windowSize), readyWhen (std::move (ready)) {}

        juce::String file;                      // "mixer" -> mixer.png and mixer@2x.png; empty = no capture
        int settleMs;                           // meters keep running this long before the capture
        std::function<void()> setUp;
        juce::Point<int> size;
        std::function<bool()> readyWhen;        // after the settle time, wait (up to 6 s) for this to become true
    };

    void timerCallback() override;
    void buildPlan();
    void beginRun (bool micMuted);
    void endRun();
    bool capture (const juce::String& name, juce::Point<int> size);
    void finish();

    // The interaction smoke test that runs after the pictures (see buildPlan).
    void check (bool ok, const juce::String& what);
    void checkMixerControls();
    void checkNoisePanel();
    void checkDrawer();
    void checkCompressorPanel (const juce::String& compSlotUid);
    void checkEq (FxDrawer& drawer, const juce::String& eqSlotUid);
    void measureEqPaint();
    void checkDrawerAfter();
    void checkChannelsAndSheets();
    void addDemoInputs();
    void openPlayThrough (Mix mix);
    void checkHeadphonePlayThrough();
    void checkStreamPlayThrough();
    void removeDemoInputs();
    void checkSetupWizardStart();
    void checkSetupWizardFinish();
    void checkUpdateAndUninstall();
    void checkAfterRemoval();

    juce::File directory;
    std::function<void (bool)> finished;

    std::unique_ptr<AppController> controller;
    std::unique_ptr<MainComponent> ui;

    // The setup wizard and the sheets around installing run for real, in a scratch home folder with a runner that only records
    // what it is asked (no systemctl, no files outside it).
    std::unique_ptr<InstallManager> installManager;
    std::shared_ptr<install::RecordingRunner> installRunner;
    juce::File setupHome, openedProgram;
    juce::String demoWebcamChannel, demoMissingChannel;        // the input channels the picture of the inputs adds
    bool uninstallAsked = false, uninstallDeletes = false;
    void wizardNext();
    void setInstalled (bool installed);

    std::vector<Step> plan;
    size_t stepIndex = 0;
    double stepElapsedMs = 0.0, lastMs = 0.0;
    bool stepStarted = false, mutedRun = false, allOk = true;
    int warmUpMs = 1500;
    int failedChecks = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScreenshotRunner)
};
}
