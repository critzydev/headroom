// TopBar: wordmark, the three tabs, and the mic status pill.
#pragma once

#include "Juce.h"
#include "ui/widgets/Buttons.h"
#include "ui/widgets/StatusPill.h"

#include <functional>

namespace hr
{
enum class Tab { mixer, apps, settings };

class TopBar final : public juce::Component
{
public:
    TopBar();

    void setTab (Tab tab, bool notify = false);
    Tab getTab() const noexcept                 { return current; }
    void setMicStatus (juce::Colour dot, const juce::String& text, const juce::String& tooltip);

    std::function<void (Tab)> onTabChanged;
    std::function<void()> onPillClicked;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    TabButton mixerTab { "Mixer" }, appsTab { "Apps" }, settingsTab { "Settings" };
    StatusPill pill;
    Tab current = Tab::mixer;
};
}
