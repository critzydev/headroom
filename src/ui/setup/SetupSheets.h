// The two small sheets around installing: "Update Headroom" (a newer AppImage was started while an older one is installed) and
// "Uninstall Headroom?" (Settings). Same panel style as the other sheets (views/Sheets.h).
#pragma once

#include "Juce.h"
#include "ui/setup/SetupWidgets.h"

#include <functional>

namespace hr
{
class UpdateSheet final : public juce::Component,
                          private juce::Timer
{
public:
    UpdateSheet (const juce::String& newVersion, const juce::String& installedVersion,
                 std::function<void()> onUpdate, std::function<void()> onNotNow);
    ~UpdateSheet() override;

    static constexpr int kWidth = 480, kHeight = 208;

    void setBusy();                                     // "Updating..." with a moving bar, nothing to press
    void showError (const juce::String& plainSentence); // what went wrong, and a Close button
    bool isBusy() const noexcept                        { return busy; }

    juce::TextButton& getUpdateButton() noexcept        { return updateButton; }
    juce::TextButton& getNotNowButton() noexcept        { return notNowButton; }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override       { repaint(); }

    juce::String heading, body, errorText;
    bool busy = false;
    std::function<void()> notNow;
    juce::TextButton updateButton { "Update" }, notNowButton { "Not now" };
};

class UninstallSheet final : public juce::Component
{
public:
    UninstallSheet (std::function<void (bool deleteSettings)> onConfirm, std::function<void()> onCancel);

    static constexpr int kWidth = 460, kHeight = 264;

    bool getDeleteSettings() const noexcept             { return deleteSettings.getToggleState(); }
    TickBox& getDeleteSettingsBox() noexcept            { return deleteSettings; }
    juce::TextButton& getConfirmButton() noexcept       { return confirmButton; }
    juce::TextButton& getCancelButton() noexcept        { return cancelButton; }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    TickBox deleteSettings;
    juce::TextButton cancelButton { "Cancel" }, confirmButton { "Uninstall" };
};
}
