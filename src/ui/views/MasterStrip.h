// MasterStrip: the fader, meter, mute and "Protect my ears" toggle for the mix currently shown.
#pragma once

#include "Juce.h"
#include "app/AppController.h"
#include "ui/widgets/Buttons.h"
#include "ui/widgets/LevelFader.h"
#include "ui/widgets/Meter.h"

namespace hr
{
class MasterStrip final : public juce::Component
{
public:
    explicit MasterStrip (AppController& controller);

    // `deviceLabel`: what the Headphones mix plays through, as the "Play through" control says it; `extraDevices`: how many more the
    // mix plays to (the Stream mix: the devices besides "Headroom Stream Mix").
    void sync (Mix mix, const juce::String& deviceLabel, int extraDevices);
    void pushLevels (const PeakPair& peaks, double dt);
    const juce::String& getLineText() const noexcept    { return lineText; }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    AppController& controller;
    Mix mix = Mix::Monitor;
    juce::String lineText;

    LedToggle protectToggle { LedToggle::Style::dot };
    LevelFader fader;
    MuteButton muteButton;
    StereoMeter meter;

    juce::Rectangle<int> capsArea, nameArea, lineArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterStrip)
};
}
