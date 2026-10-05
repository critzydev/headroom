// SetupSheet: the first-run setup wizard. A centred sheet in the app's own style with a few short steps and plain
// words: welcome, mic and headphones, channels, voice, system, the Wave:3's hardware controls (only with a Wave:3), and a
// closing page with the Discord tip. It is also what "Run setup again" in Settings opens.
//
// It changes the mixer through AppController like the Settings page does (see SetupChoices), and, when this is an AppImage that
// is not installed yet, installs it through InstallManager once the choices are made.
#pragma once

#include "Juce.h"
#include "app/AppController.h"
#include "app/InstallManager.h"
#include "ui/setup/SetupChoices.h"

#include <functional>
#include <memory>
#include <vector>

namespace hr
{
class SetupSheet final : public juce::Component,
                         private juce::Timer
{
public:
    enum class Mode { firstRun, rerun };
    enum class Outcome { notNow, finished };
    enum class Page { welcome, devices, channels, voice, system, hardware, installing, done, failed };

    // `onClose` is called when the person is done (finished, or "Not now"); the owner then removes the sheet.
    SetupSheet (AppController& controller, InstallManager& installManager, Mode mode, std::function<void (Outcome)> onClose);
    ~SetupSheet() override;

    static constexpr int kWidth = 620, kHeight = 552;

    // ---- For the owner, the screenshot run and the tests --------------------------------------------------
    Page getPage() const noexcept                       { return page; }
    const SetupChoices& getChoices() const noexcept     { return choices; }
    SetupChoices& getChoices() noexcept                 { return choices; }
    int getStepNumber() const;                          // 1-based among the pages with a step number (0 on the closing pages)
    int getStepCount() const;
    juce::Component* getPageComponent() noexcept;
    juce::TextButton& getNextButton() noexcept          { return nextButton; }
    juce::TextButton& getBackButton() noexcept          { return backButton; }
    bool installedNow() const noexcept                  { return installDone; }
    const install::Result& getInstallResult() const noexcept { return installResult; }

    void next();                                        // the Next button, Return
    void back();                                        // the Back button
    void showPage (Page newPage);

    void paint (juce::Graphics&) override;
    void resized() override;

    class PageBase;

private:
    friend class PageBase;

    void timerCallback() override;
    std::vector<Page> choicePages() const;
    void updateButtons();
    bool isLastChoicePage() const;                      // the step after which the choices are applied
    juce::String finishLabel() const;                   // "Set up Headroom" when this installs it, else "Save"
    void finish();                                      // apply the choices, install (first run), then the closing page
    void leave (Outcome outcome);

    AppController& controller;
    InstallManager& installManager;
    const Mode mode;
    std::function<void (Outcome)> onClose;

    SetupChoices choices;
    Page page = Page::welcome;
    bool applied = false, installDone = false, installRan = false;
    install::Result installResult;
    int hardwareSeen = 0;

    std::unique_ptr<PageBase> content;
    juce::TextButton nextButton { "Next" }, backButton { "Back" };

    JUCE_DECLARE_WEAK_REFERENCEABLE (SetupSheet)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SetupSheet)
};
}
