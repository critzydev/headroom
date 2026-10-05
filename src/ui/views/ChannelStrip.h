// ChannelStrip: one mixer channel (tag, icon, name, routed apps, FX button, fader + meter, mute, link) and its menus.
#pragma once

#include "Juce.h"
#include "app/AppController.h"
#include "ui/widgets/Buttons.h"
#include "ui/widgets/LevelFader.h"
#include "ui/widgets/Meter.h"

#include <functional>

namespace hr
{
class ChannelStrip final : public juce::Component,
                           public juce::SettableTooltipClient
{
public:
    ChannelStrip (AppController& controller, const juce::String& channelId);

    const juce::String& getChannelId() const noexcept   { return channelId; }
    const juce::String& getLineText() const noexcept    { return routedText; }      // the line under the name: apps, device, or "Not connected"

    struct Context
    {
        Mix mix = Mix::Monitor;
        juce::StringArray routedApps;       // names of the apps playing into this channel
        juce::String micLabel;              // the mic strip's device line
        juce::String deviceLabel;           // an input channel's device (model-aware), as the backend last saw it
        bool deviceConnected = false;       // ... and whether it is on the machine
        int activeEffects = 0;
        bool drawerOpen = false;
    };

    // Pulls everything that can change from the model. Cheap when nothing did.
    void sync (const Context& context);

    // 30 fps: `levels` is the engine's peak since the last call.
    void pushLevels (const ChannelLevels& levels, double dt);

    void beginRename();

    // For the UI smoke test in screenshot mode.
    LevelFader& getFader() noexcept             { return fader; }
    MuteButton& getMuteButton() noexcept        { return muteButton; }
    LinkToggle& getLinkButton() noexcept        { return linkButton; }

    std::function<void (const juce::String&)> onToggleFx;
    std::function<void (const juce::String&)> onRemove;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    void showMenu();
    void finishRename (bool commit, const juce::String& text);
    juce::String describeRouted (const juce::StringArray& apps) const;
    void updateRoutedText();
    void refreshTooltip();
    bool isInputConnected() const;

    AppController& controller;
    const juce::String channelId;

    ChannelInfo info;
    Context ctx;
    juce::String routedText;

    FxButton fxButton;
    LevelFader fader;
    MuteButton muteButton;
    LinkToggle linkButton;
    std::unique_ptr<juce::TextEditor> renameEditor;
    StereoMeter meter;

    juce::Rectangle<int> nameArea, iconArea, capsArea, lineArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelStrip)
};

// The dashed ghost strip after the last channel.
class AddChannelStrip final : public juce::Component,
                              public juce::SettableTooltipClient
{
public:
    AddChannelStrip();

    std::function<void()> onClick;

    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override      { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override       { hover = false; repaint(); }
    void mouseUp (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusGained (FocusChangeType) override             { repaint(); }
    void focusLost (FocusChangeType) override               { repaint(); }

private:
    bool hover = false;
};
}
