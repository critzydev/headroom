// StatusPill and BannerRow.
#include "ui/widgets/StatusPill.h"

#include "ui/HeadroomLookAndFeel.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/widgets/HoverFade.h"

namespace hr
{
StatusPill::StatusPill()
{
    setWantsKeyboardFocus (true);
    setMouseClickGrabsKeyboardFocus (false);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setTooltip ("Open microphone settings");
    HoverFade::attach (*this);
}

void StatusPill::set (juce::Colour dotColour, const juce::String& text)
{
    if (dot != dotColour || label != text)
    {
        dot = dotColour;
        label = text;
        repaint();
    }
}

int StatusPill::getIdealWidth() const
{
    return 12 + 8 + 8 + (int) std::ceil (theme::textWidth (theme::sans (12.0f, theme::Sans::Medium), label)) + 12;
}

void StatusPill::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const float radius = bounds.getHeight() * 0.5f;

    const float fade = HoverFade::get (*this, hover);
    g.setColour (theme::bg2.interpolatedWith (theme::bg3, fade));
    g.fillRoundedRectangle (bounds, radius);

    if (hasKeyboardFocus (false))
    {
        HeadroomLookAndFeel::drawFocusRing (g, bounds, radius);
    }
    else
    {
        g.setColour (theme::line);
        g.drawRoundedRectangle (bounds.reduced (0.5f), radius - 0.5f, 1.0f);
    }

    g.setColour (dot);
    g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ bounds.getX() + 16.0f, bounds.getCentreY() }));

    g.setColour (theme::text1.interpolatedWith (theme::text0, fade));
    g.setFont (theme::sans (12.0f, theme::Sans::Medium));
    g.drawText (label, bounds.withTrimmedLeft (28.0f).withTrimmedRight (10.0f), juce::Justification::centredLeft, true);
}

void StatusPill::mouseUp (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu() && getLocalBounds().contains (e.getPosition()) && onClick)
        onClick();
}

bool StatusPill::keyPressed (const juce::KeyPress& key)
{
    if (key.getKeyCode() == juce::KeyPress::returnKey || key.getKeyCode() == juce::KeyPress::spaceKey)
    {
        if (onClick)
            onClick();

        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------------------------------
// BannerRow
// ---------------------------------------------------------------------------------------------------

BannerRow::BannerRow (BannerSpec spec) : banner (std::move (spec))
{
    if (banner.actionLabel.isNotEmpty())
    {
        actionButton.setButtonText (banner.actionLabel);
        actionButton.getProperties().set (style::button, "link");
        actionButton.setMouseClickGrabsKeyboardFocus (false);
        actionButton.onClick = [this]
        {
            if (banner.action)
                banner.action();
        };
        addAndMakeVisible (actionButton);
    }

    if (banner.onDismiss)
    {
        closeButton.setTooltip ("Dismiss");
        closeButton.onClick = [this]
        {
            if (banner.onDismiss)
                banner.onDismiss();
        };
        addAndMakeVisible (closeButton);
    }
}

juce::Colour BannerRow::severityColour() const
{
    switch (banner.severity)
    {
        case BannerSpec::Severity::error:   return theme::bad;
        case BannerSpec::Severity::warning: return theme::warn;
        case BannerSpec::Severity::info:    break;
    }

    return theme::text1;
}

void BannerRow::resized()
{
    auto area = getLocalBounds().withTrimmedRight (theme::kSidePadding - 4);

    if (banner.onDismiss)
        closeButton.setBounds (area.removeFromRight (28).withSizeKeepingCentre (28, 28));

    if (banner.actionLabel.isNotEmpty())
    {
        const int w = getLookAndFeel().getTextButtonWidthToFitText (actionButton, 28);
        actionButton.setBounds (area.removeFromRight (w).withSizeKeepingCentre (w, 28));
        area.removeFromRight (8);
    }

    textArea = area.withTrimmedLeft (theme::kSidePadding + 16 + 12);
}

void BannerRow::paint (juce::Graphics& g)
{
    const auto colour = severityColour();

    g.fillAll (theme::tint (theme::bg0, colour, 0.12f));
    g.setColour (theme::tint (theme::bg0, colour, 0.26f));
    g.fillRect (0, getHeight() - 1, getWidth(), 1);

    const char* icon = banner.severity == BannerSpec::Severity::info ? "info"
                     : banner.severity == BannerSpec::Severity::warning ? "triangle-alert" : "circle-alert";
    Icons::draw (g, icon, juce::Point<float> ((float) theme::kSidePadding + 8.0f, (float) getHeight() * 0.5f - 0.5f), 16.0f, colour);

    g.setColour (theme::text0);
    g.setFont (theme::sans (13.0f));
    g.drawText (banner.text, textArea, juce::Justification::centredLeft, true);
}
}
