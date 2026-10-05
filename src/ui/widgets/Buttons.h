// Small custom buttons: icon, mute, link, power / LED toggle, switch, FX button, tab, and the segmented control.
#pragma once

#include "Juce.h"

#include <functional>

namespace hr
{
// A flat icon-only button: transparent until hovered.
class IconButton final : public juce::Button
{
public:
    IconButton (const juce::String& iconName, float iconSizePx = 16.0f);

    void setIconName (const juce::String& name)     { icon = name; repaint(); }
    void setIconColour (juce::Colour c)             { colour = c; hasColour = true; repaint(); }

    void paintButton (juce::Graphics&, bool highlighted, bool down) override;

private:
    juce::String icon;
    float size;
    juce::Colour colour;
    bool hasColour = false;
};

// 32 px square speaker button. Toggle state = muted: the icon and a 14 % tint go `bad`.
class MuteButton final : public juce::Button
{
public:
    MuteButton();
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;
};

// 32 px square chain button. Toggle state = linked (the fader moves both mixes).
class LinkToggle final : public juce::Button
{
public:
    LinkToggle();
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;
};

// Power-style toggle for effect slots ("on" = ok green) or a dot + label toggle ("Protect my ears").
class LedToggle final : public juce::Button
{
public:
    enum class Style { power, dot };

    explicit LedToggle (Style s = Style::power);
    void setLabel (const juce::String& newLabel)    { label = newLabel; repaint(); }
    static int idealWidth (const juce::String& labelText);
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;

private:
    Style style;
    juce::String label;
};

// 32 x 18 switch for the Settings toggles. Accent when on.
class SwitchToggle final : public juce::Button
{
public:
    SwitchToggle();
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;
};

// Full-width "FX 3" button on each strip. Amber outline when at least one effect is running.
class FxButton final : public juce::Button
{
public:
    FxButton();

    void setState (int activeEffects, bool drawerOpen);
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;

private:
    int count = 0;
    bool open = false;
};

// Top-bar tab: text with a 2 px accent underline when selected.
class TabButton final : public juce::Button
{
public:
    explicit TabButton (const juce::String& text);
    int getIdealWidth() const;
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;
};

// Two to seven mutually exclusive options in one rounded bar.
class SegmentedControl final : public juce::Component
{
public:
    SegmentedControl();

    void setLabels (const juce::StringArray& newLabels, bool equalWidths = true);
    void setSelected (int index, bool notify = false);
    int getSelected() const noexcept                { return selected; }
    int getIdealWidth() const;
    void setSegmentEnabled (int index, bool enabled);

    std::function<void (int)> onChange;

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override   { hover = -1; repaint(); }
    void mouseUp (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusGained (FocusChangeType) override         { repaint(); }
    void focusLost (FocusChangeType) override           { repaint(); }

private:
    juce::Rectangle<float> segmentBounds (int index) const;
    int indexAt (juce::Point<int>) const;

    juce::StringArray labels;
    std::vector<bool> disabled;
    int selected = 0, hover = -1;
    bool equalWidth = true;
};
}
