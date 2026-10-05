// SettingsView: one scrolling column of settings.
#pragma once

#include "Juce.h"
#include "app/AppController.h"
#include "app/InstallManager.h"
#include "ui/widgets/Buttons.h"
#include "ui/widgets/Dropdown.h"
#include "ui/widgets/Meter.h"
#include "ui/widgets/ScrollView.h"

#include <memory>
#include <vector>

namespace hr
{
class SettingsView final : public juce::Component
{
public:
    // `installManager` may be null (then there are no setup or uninstall rows).
    SettingsView (AppController& controller, MeterClock& clock, InstallManager* installManager = nullptr);
    ~SettingsView() override;

    void syncFromModel();
    void scrollToMicrophone();

    // The "Run setup again" and "Uninstall Headroom" buttons (About).
    std::function<void()> onRunSetup, onUninstall;

    // For the smoke test.
    juce::Component* findButton (const juce::String& text);

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    class Content;

    std::unique_ptr<Content> content;
    ScrollView viewport { false, 64 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsView)
};
}
