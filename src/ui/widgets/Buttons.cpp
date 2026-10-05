// Small custom buttons and the segmented control.
#include "ui/widgets/Buttons.h"

#include "ui/HeadroomLookAndFeel.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/widgets/HoverFade.h"

namespace hr
{
namespace
{
    void setUpButton (juce::Button& b)
    {
        b.setMouseClickGrabsKeyboardFocus (false);     // a focus ring only ever means "you got here with the keyboard"
        b.setWantsKeyboardFocus (true);
        HoverFade::attach (b);
    }
}

// ---------------------------------------------------------------------------------------------------
// IconButton
// ---------------------------------------------------------------------------------------------------

IconButton::IconButton (const juce::String& iconName, float iconSizePx)
    : juce::Button (iconName), icon (iconName), size (iconSizePx)
{
    setUpButton (*this);
}

void IconButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto bounds = getLocalBounds().toFloat();
    const float hover = isEnabled() ? HoverFade::get (*this, highlighted) : 0.0f;

    if (isEnabled() && (hover > 0.0f || down))
    {
        g.setColour (down ? theme::bg3.brighter (0.06f) : theme::bg3.withAlpha (hover));
        g.fillRoundedRectangle (bounds, theme::kControlRadius);
    }

    if (hasKeyboardFocus (false))
        HeadroomLookAndFeel::drawFocusRing (g, bounds, theme::kControlRadius);

    auto c = hasColour ? colour : theme::text1.interpolatedWith (theme::text0, down ? 1.0f : hover);

    if (! isEnabled())
        c = theme::text2;

    Icons::draw (g, icon, bounds, size, c);
}

// ---------------------------------------------------------------------------------------------------
// MuteButton / LinkToggle
// ---------------------------------------------------------------------------------------------------

MuteButton::MuteButton() : juce::Button ("Mute")
{
    setUpButton (*this);
    setClickingTogglesState (false);
    setSize (32, 32);
}

void MuteButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto bounds = getLocalBounds().toFloat();
    const bool muted = getToggleState();
    const float hover = down ? 1.0f : HoverFade::get (*this, highlighted);

    juce::Colour fill = theme::bg2.interpolatedWith (theme::bg3, hover);

    if (muted)
        fill = theme::tint (theme::bg1, theme::bad, 0.14f + 0.06f * hover);

    g.setColour (fill);
    g.fillRoundedRectangle (bounds, theme::kControlRadius);

    if (hasKeyboardFocus (false))
        HeadroomLookAndFeel::drawFocusRing (g, bounds, theme::kControlRadius);

    Icons::draw (g, muted ? "volume-x" : "volume-2", bounds, 16.0f,
                 muted ? theme::bad : theme::text1.interpolatedWith (theme::text0, hover));
}

LinkToggle::LinkToggle() : juce::Button ("Link")
{
    setUpButton (*this);
    setClickingTogglesState (false);
    setSize (32, 32);
}

void LinkToggle::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto bounds = getLocalBounds().toFloat();
    const bool linked = getToggleState();
    const float hover = down ? 1.0f : HoverFade::get (*this, highlighted);

    g.setColour (theme::bg2.interpolatedWith (theme::bg3, hover));
    g.fillRoundedRectangle (bounds, theme::kControlRadius);

    if (hasKeyboardFocus (false))
        HeadroomLookAndFeel::drawFocusRing (g, bounds, theme::kControlRadius);

    Icons::draw (g, linked ? "link" : "link-2-off", bounds, 16.0f,
                 linked ? theme::accent : theme::text2.interpolatedWith (theme::text1, hover));
}

// ---------------------------------------------------------------------------------------------------
// LedToggle / SwitchToggle
// ---------------------------------------------------------------------------------------------------

LedToggle::LedToggle (Style s) : juce::Button ("Power"), style (s)
{
    setUpButton (*this);
    setClickingTogglesState (true);
}

int LedToggle::idealWidth (const juce::String& labelText)
{
    return 8 + 8 + (int) std::ceil (theme::textWidth (theme::sans (12.0f, theme::Sans::Medium), labelText)) + 8;
}

void LedToggle::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto bounds = getLocalBounds().toFloat();
    const bool on = getToggleState();
    const float hover = isEnabled() ? (down ? 1.0f : HoverFade::get (*this, highlighted)) : 0.0f;

    if (style == Style::power)
    {
        if (hover > 0.0f)
        {
            g.setColour (theme::bg3.withAlpha (hover));
            g.fillRoundedRectangle (bounds, theme::kControlRadius);
        }

        if (hasKeyboardFocus (false))
            HeadroomLookAndFeel::drawFocusRing (g, bounds, theme::kControlRadius);

        Icons::draw (g, "power", bounds, 16.0f, ! isEnabled() ? theme::text2 : (on ? theme::ok : theme::text2.interpolatedWith (theme::text1, hover)));
        return;
    }

    if (hover > 0.0f)
    {
        g.setColour (theme::bg3.withAlpha (hover));
        g.fillRoundedRectangle (bounds, theme::kControlRadius);
    }

    if (hasKeyboardFocus (false))
        HeadroomLookAndFeel::drawFocusRing (g, bounds, theme::kControlRadius);

    const auto led = juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ bounds.getX() + 10.0f, bounds.getCentreY() });

    if (on)
    {
        g.setColour (theme::ok);
        g.fillEllipse (led);
    }
    else
    {
        g.setColour (theme::text2);
        g.drawEllipse (led.reduced (0.5f), 1.0f);
    }

    // The label shrinks a little rather than being cut off: "Protect my ears" has to fit the 120 px master strip.
    const float available = bounds.getWidth() - 24.0f - 2.0f;
    float fontSize = 12.0f;

    while (fontSize > 10.0f && theme::textWidth (theme::sans (fontSize, theme::Sans::Medium), label) > available)
        fontSize -= 0.5f;

    g.setColour (on ? theme::text0 : theme::text1);
    g.setFont (theme::sans (fontSize, theme::Sans::Medium));
    g.drawText (label, bounds.withTrimmedLeft (22.0f), juce::Justification::centredLeft, true);
}

SwitchToggle::SwitchToggle() : juce::Button ("Switch")
{
    setUpButton (*this);
    setClickingTogglesState (true);
    setSize (32, 18);
}

void SwitchToggle::paintButton (juce::Graphics& g, bool highlighted, bool)
{
    const auto bounds = getLocalBounds().toFloat().withSizeKeepingCentre (32.0f, 18.0f);
    const bool on = getToggleState();
    const float alpha = isEnabled() ? 1.0f : 0.4f;      // a switch that depends on another one: same look, faded

    g.setColour ((on ? theme::accent : theme::bg2.interpolatedWith (theme::bg3, isEnabled() ? HoverFade::get (*this, highlighted) : 0.0f))
                     .withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (bounds, 9.0f);

    if (! on)
    {
        g.setColour (theme::lineStrong.withMultipliedAlpha (alpha));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 8.5f, 1.0f);
    }

    const float thumbX = on ? bounds.getRight() - 9.0f : bounds.getX() + 9.0f;
    g.setColour ((on ? theme::bg0 : theme::text1).withMultipliedAlpha (alpha));
    g.fillEllipse (juce::Rectangle<float> (12.0f, 12.0f).withCentre ({ thumbX, bounds.getCentreY() }));

    if (hasKeyboardFocus (false))
        HeadroomLookAndFeel::drawFocusRing (g, bounds.expanded (3.0f), 12.0f);
}

// ---------------------------------------------------------------------------------------------------
// FxButton
// ---------------------------------------------------------------------------------------------------

FxButton::FxButton() : juce::Button ("FX")
{
    setUpButton (*this);
    setClickingTogglesState (false);
}

void FxButton::setState (int activeEffects, bool drawerOpen)
{
    if (count != activeEffects || open != drawerOpen)
    {
        count = activeEffects;
        open = drawerOpen;
        repaint();
    }
}

void FxButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto bounds = getLocalBounds().toFloat();
    const bool active = count > 0;

    // Open (its drawer is showing): a faint amber wash, so you can see which channel the drawer belongs to.
    const float hover = down ? 1.0f : HoverFade::get (*this, highlighted);
    g.setColour (open ? theme::tint (theme::bg2, theme::accent, 0.13f + 0.05f * hover) : theme::bg2.interpolatedWith (theme::bg3, hover));
    g.fillRoundedRectangle (bounds, theme::kControlRadius);

    const bool focused = hasKeyboardFocus (false);
    g.setColour (focused ? (active ? theme::text0 : theme::accent) : (active ? theme::accent : (open ? theme::text2 : theme::line)));
    g.drawRoundedRectangle (bounds.reduced (0.5f), theme::kControlRadius - 0.5f, 1.0f);

    const auto labelFont = theme::sans (12.0f, theme::Sans::SemiBold);
    const auto countText = active ? juce::String (count) : juce::String();
    const float labelW = theme::textWidth (labelFont, "FX");
    const float countW = active ? theme::textWidth (theme::mono (11.0f), countText) : 0.0f;
    const float groupW = labelW + (active ? 6.0f + countW : 0.0f);
    const float x = std::floor (bounds.getCentreX() - groupW * 0.5f);

    g.setColour (theme::text0);
    g.setFont (labelFont);
    g.drawText ("FX", juce::Rectangle<float> (x, bounds.getY(), labelW + 2.0f, bounds.getHeight()), juce::Justification::centredLeft, false);

    if (active)
    {
        g.setColour (theme::accent);
        g.setFont (theme::mono (11.0f));
        g.drawText (countText, juce::Rectangle<float> (x + labelW + 6.0f, bounds.getY(), countW + 2.0f, bounds.getHeight()),
                    juce::Justification::centredLeft, false);
    }
}

// ---------------------------------------------------------------------------------------------------
// TabButton
// ---------------------------------------------------------------------------------------------------

TabButton::TabButton (const juce::String& text) : juce::Button (text)
{
    setUpButton (*this);
    setClickingTogglesState (false);        // TopBar owns which tab is selected (a JUCE radio group would fire every tab's onClick)
    setButtonText (text);
}

int TabButton::getIdealWidth() const
{
    return (int) std::ceil (theme::textWidth (theme::sans (13.0f, theme::Sans::Medium), getButtonText())) + 24;
}

void TabButton::paintButton (juce::Graphics& g, bool highlighted, bool)
{
    const auto bounds = getLocalBounds().toFloat();
    const bool selected = getToggleState();

    g.setColour (selected ? theme::text0 : theme::text1.interpolatedWith (theme::text0, HoverFade::get (*this, highlighted)));
    g.setFont (theme::sans (13.0f, theme::Sans::Medium));
    g.drawText (getButtonText(), bounds.withTrimmedBottom (2.0f), juce::Justification::centred, false);

    if (selected)
    {
        const float w = theme::textWidth (theme::sans (13.0f, theme::Sans::Medium), getButtonText());
        g.setColour (theme::accent);
        g.fillRect (std::floor (bounds.getCentreX() - w * 0.5f) - 2.0f, bounds.getBottom() - 2.0f, std::ceil (w) + 4.0f, 2.0f);
    }

    if (hasKeyboardFocus (false))
        HeadroomLookAndFeel::drawFocusRing (g, bounds.reduced (2.0f, 8.0f), 4.0f);
}

// ---------------------------------------------------------------------------------------------------
// SegmentedControl
// ---------------------------------------------------------------------------------------------------

SegmentedControl::SegmentedControl()
{
    setWantsKeyboardFocus (true);
    setMouseClickGrabsKeyboardFocus (false);
}

void SegmentedControl::setLabels (const juce::StringArray& newLabels, bool equalWidths)
{
    labels = newLabels;
    equalWidth = equalWidths;
    disabled.assign ((size_t) labels.size(), false);
    selected = juce::jlimit (0, juce::jmax (0, labels.size() - 1), selected);
    repaint();
}

void SegmentedControl::setSelected (int index, bool notify)
{
    index = juce::jlimit (0, juce::jmax (0, labels.size() - 1), index);

    if (index == selected)
        return;

    selected = index;
    repaint();

    if (notify && onChange)
        onChange (selected);
}

void SegmentedControl::setSegmentEnabled (int index, bool enabled)
{
    if (juce::isPositiveAndBelow (index, (int) disabled.size()))
    {
        disabled[(size_t) index] = ! enabled;
        repaint();
    }
}

int SegmentedControl::getIdealWidth() const
{
    const auto font = theme::sans (12.0f, theme::Sans::Medium);
    float widest = 0.0f, sum = 0.0f;

    for (auto& label : labels)
    {
        const float w = theme::textWidth (font, label) + 24.0f;
        widest = juce::jmax (widest, w);
        sum += w;
    }

    return (int) std::ceil (equalWidth ? widest * (float) labels.size() : sum) + 4;
}

juce::Rectangle<float> SegmentedControl::segmentBounds (int index) const
{
    const auto inner = getLocalBounds().toFloat().reduced (2.0f);
    const int n = juce::jmax (1, labels.size());

    if (equalWidth)
    {
        const float w = inner.getWidth() / (float) n;
        return { inner.getX() + w * (float) index, inner.getY(), w, inner.getHeight() };
    }

    const auto font = theme::sans (12.0f, theme::Sans::Medium);
    float total = 0.0f;

    for (auto& label : labels)
        total += theme::textWidth (font, label) + 24.0f;

    const float scale = inner.getWidth() / juce::jmax (1.0f, total);
    float x = inner.getX();

    for (int i = 0; i < index; ++i)
        x += (theme::textWidth (font, labels[i]) + 24.0f) * scale;

    return { x, inner.getY(), (theme::textWidth (font, labels[index]) + 24.0f) * scale, inner.getHeight() };
}

int SegmentedControl::indexAt (juce::Point<int> p) const
{
    for (int i = 0; i < labels.size(); ++i)
        if (segmentBounds (i).contains (p.toFloat()))
            return i;

    return -1;
}

void SegmentedControl::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();

    g.setColour (theme::bg2);
    g.fillRoundedRectangle (bounds, theme::kControlRadius);

    if (hasKeyboardFocus (false))
    {
        HeadroomLookAndFeel::drawFocusRing (g, bounds, theme::kControlRadius);
    }
    else
    {
        g.setColour (theme::line);
        g.drawRoundedRectangle (bounds.reduced (0.5f), theme::kControlRadius - 0.5f, 1.0f);
    }

    g.setFont (theme::sans (12.0f, theme::Sans::Medium));

    for (int i = 0; i < labels.size(); ++i)
    {
        const auto seg = segmentBounds (i);
        const bool isSelected = i == selected;
        const bool isOff = i < (int) disabled.size() && disabled[(size_t) i];

        if (isSelected)
        {
            g.setColour (theme::bg3);
            g.fillRoundedRectangle (seg, theme::kControlRadius - 2.0f);
            g.setColour (theme::lineStrong);
            g.drawRoundedRectangle (seg.reduced (0.5f), theme::kControlRadius - 2.5f, 1.0f);
        }

        g.setColour (isOff ? theme::text2 : (isSelected || i == hover ? theme::text0 : theme::text1));
        g.drawText (labels[i], seg, juce::Justification::centred, false);
    }
}

void SegmentedControl::mouseMove (const juce::MouseEvent& e)
{
    const int h = indexAt (e.getPosition());

    if (h != hover)
    {
        hover = h;
        repaint();
    }
}

void SegmentedControl::mouseUp (const juce::MouseEvent& e)
{
    if (e.mouseWasDraggedSinceMouseDown() || e.mods.isPopupMenu())
        return;

    const int i = indexAt (e.getPosition());

    if (i >= 0 && ! (i < (int) disabled.size() && disabled[(size_t) i]))
        setSelected (i, true);
}

bool SegmentedControl::keyPressed (const juce::KeyPress& key)
{
    const int code = key.getKeyCode();

    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::upKey)
    {
        setSelected (selected - 1, true);
        return true;
    }

    if (code == juce::KeyPress::rightKey || code == juce::KeyPress::downKey)
    {
        setSelected (selected + 1, true);
        return true;
    }

    return false;
}
}
