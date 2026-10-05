// The small controls of the setup wizard: a tick box, a one-of-several row and a switch with a caption. In a header so that the
// screenshot run can find and click them like a person would.
#pragma once

#include "Juce.h"
#include "ui/HeadroomLookAndFeel.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/widgets/Buttons.h"
#include "ui/widgets/HoverFade.h"

#include <functional>

namespace hr
{
// A tick box: 18 px, accent when ticked.
class TickBox final : public juce::Button
{
public:
    TickBox() : juce::Button ("Keep")
    {
        setClickingTogglesState (true);
        setMouseClickGrabsKeyboardFocus (false);
        setWantsKeyboardFocus (true);
        HoverFade::attach (*this);
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool) override
    {
        const auto box = getLocalBounds().toFloat().withSizeKeepingCentre (18.0f, 18.0f);
        const bool on = getToggleState();
        const float hover = isEnabled() ? HoverFade::get (*this, highlighted) : 0.0f;
        const float alpha = isEnabled() ? 1.0f : 0.4f;

        g.setColour ((on ? theme::accent : theme::bg2.interpolatedWith (theme::bg3, hover)).withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (box, 5.0f);

        if (! on)
        {
            g.setColour (theme::lineStrong.withMultipliedAlpha (alpha));
            g.drawRoundedRectangle (box.reduced (0.5f), 4.5f, 1.0f);
        }
        else
        {
            Icons::draw (g, "check", box, 14.0f, theme::bg0);
        }

        if (hasKeyboardFocus (false))
            HeadroomLookAndFeel::drawFocusRing (g, box.expanded (3.0f), 8.0f);
    }
};

// One option out of several: a ring with a dot, a name and a line under it. The whole row is the button; the owner decides which
// row is selected (setToggleState).
class ChoiceRow final : public juce::Button
{
public:
    ChoiceRow (const juce::String& title, const juce::String& caption) : juce::Button (title), name (title), line (caption)
    {
        setClickingTogglesState (false);
        setMouseClickGrabsKeyboardFocus (false);
        setWantsKeyboardFocus (true);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        HoverFade::attach (*this);
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool) override
    {
        const auto bounds = getLocalBounds().toFloat();
        const bool on = getToggleState();
        const float hover = isEnabled() ? HoverFade::get (*this, highlighted) : 0.0f;

        if (on)
        {
            g.setColour (theme::bg2);
            g.fillRoundedRectangle (bounds, theme::kControlRadius);
            g.setColour (theme::accent.withAlpha (0.8f));
            g.drawRoundedRectangle (bounds.reduced (0.5f), theme::kControlRadius - 0.5f, 1.0f);
        }
        else if (hover > 0.0f)
        {
            g.setColour (theme::bg2.withAlpha (hover));
            g.fillRoundedRectangle (bounds, theme::kControlRadius);
        }

        if (hasKeyboardFocus (false))
            HeadroomLookAndFeel::drawFocusRing (g, bounds, theme::kControlRadius);

        const auto ring = juce::Rectangle<float> (16.0f, 16.0f).withCentre ({ 24.0f, bounds.getCentreY() });
        g.setColour (on ? theme::accent : theme::lineStrong);
        g.drawEllipse (ring.reduced (0.5f), 1.0f);

        if (on)
        {
            g.setColour (theme::accent);
            g.fillEllipse (ring.reduced (4.5f));
        }

        const float alpha = isEnabled() ? 1.0f : 0.45f;
        g.setColour (theme::text0.withMultipliedAlpha (alpha));
        g.setFont (theme::sans (13.0f, theme::Sans::Medium));
        g.drawText (name, juce::Rectangle<int> (48, 6, getWidth() - 60, 18), juce::Justification::centredLeft, true);

        g.setColour (theme::text2.withMultipliedAlpha (alpha));
        g.setFont (theme::sans (12.0f));
        g.drawText (line, juce::Rectangle<int> (48, 25, getWidth() - 60, 16), juce::Justification::centredLeft, true);
    }

private:
    juce::String name, line;
};

// A switch with a name and a line under it.
class SwitchRow final : public juce::Component
{
public:
    SwitchRow (const juce::String& titleText, const juce::String& captionText, bool initial, std::function<void (bool)> onChange)
        : title (titleText), caption (captionText)
    {
        toggle.setToggleState (initial, juce::dontSendNotification);
        toggle.onClick = [this, onChange = std::move (onChange)] { onChange (toggle.getToggleState()); };
        addAndMakeVisible (toggle);
    }

    static constexpr int kHeight = 60;

    SwitchToggle& getToggle() noexcept          { return toggle; }
    void setIndent (int pixels)                 { indent = pixels; resized(); repaint(); }
    void setCaption (const juce::String& text)  { caption = text; repaint(); }
    void setEnabledLook (bool enabled)          { toggle.setEnabled (enabled); repaint(); }

    void resized() override                     { toggle.setBounds (indent, 6, 32, 18); }

    void paint (juce::Graphics& g) override
    {
        g.setColour (toggle.isEnabled() ? theme::text0 : theme::text2);
        g.setFont (theme::sans (13.0f, theme::Sans::Medium));
        g.drawText (title, juce::Rectangle<int> (indent + 44, 4, getWidth() - indent - 44, 20), juce::Justification::centredLeft, true);

        g.setColour (theme::text2);
        g.setFont (theme::sans (12.0f));
        g.drawFittedText (caption, juce::Rectangle<int> (indent + 44, 25, getWidth() - indent - 44, 32), juce::Justification::topLeft, 2, 1.0f);
    }

private:
    SwitchToggle toggle;
    juce::String title, caption;
    int indent = 0;
};
}
