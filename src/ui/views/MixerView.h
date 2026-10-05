// MixerView: mix switch + output picker, the scrolling row of channel strips, and the master strip.
#pragma once

#include "Juce.h"
#include "app/AppController.h"
#include "ui/views/ChannelStrip.h"
#include "ui/views/MasterStrip.h"
#include "ui/widgets/Buttons.h"
#include "ui/widgets/Dropdown.h"
#include "ui/widgets/Meter.h"
#include "ui/widgets/ScrollView.h"
#include "ui/Theme.h"

#include <functional>
#include <memory>
#include <vector>

namespace hr
{
class MixerView final : public juce::Component,
                        private MeterClock::Listener
{
public:
    MixerView (AppController& controller, MeterClock& clock);
    ~MixerView() override;

    // Re-reads channels, levels, routing and devices. Cheap; called for every model or backend change.
    void syncFromModel();

    void setMix (Mix newMix);
    Mix getMix() const noexcept                         { return mix; }

    // Marks the strip whose FX drawer is open.
    void setDrawerChannel (const juce::String& channelId);

    ChannelStrip* findStrip (const juce::String& channelId) const;

    // For the screenshot smoke test: what the ghost strip's menu does ("App channel", "Input..." and one of its devices), and what is on screen.
    void addChannelFromUi()                             { addAppChannelFromMenu(); }
    void addInputFromUi (const juce::String& deviceId)  { addInputFromMenu (deviceId); }
    int getNumStrips() const noexcept;
    ChannelStrip* getLastStrip() const noexcept;
    Dropdown& getOutputDropdown() noexcept              { return outputDropdown; }
    MasterStrip& getMasterStrip() noexcept              { return master; }
    AddChannelStrip& getAddStrip() noexcept;

    std::function<void (const juce::String&)> onToggleFx;
    std::function<void (const juce::String&)> onRemoveChannel;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class StripRow;

    void meterTick (double dt) override;
    void addChannelPressed();                   // the ghost strip: a small menu, "App channel" or "Input..."
    void addAppChannelFromMenu();
    void addInputFromMenu (const juce::String& deviceId);
    void layoutContent();
    int countActiveEffects (const juce::String& channelId) const;

    AppController& controller;
    MeterClock& clock;
    Mix mix = Mix::Monitor;
    juce::String drawerChannel, pendingStrip;
    bool pendingRename = false;                 // the strip that was just added starts its rename (an app channel; an input is named after its device)

    SegmentedControl mixSwitch;
    Dropdown outputDropdown;
    juce::Rectangle<int> streamNoteArea;
    bool showStreamNote = false;

    std::unique_ptr<StripRow> row;
    ScrollView viewport { true, theme::kStripWidth + theme::kGap };
    MasterStrip master;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerView)
};
}
