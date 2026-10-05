// HeadroomLookAndFeel: Theme colours, Inter, flat buttons, quiet menus, thin scrollbars.
#include "ui/HeadroomLookAndFeel.h"

#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/widgets/HoverFade.h"

namespace hr
{
HeadroomLookAndFeel::HeadroomLookAndFeel()
{
    setDefaultSansSerifTypeface (theme::sansTypeface (theme::Sans::Regular));

    setColour (juce::ResizableWindow::backgroundColourId, theme::bg0);

    setColour (juce::Label::textColourId, theme::text0);
    setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);

    setColour (juce::TextButton::buttonColourId, theme::bg2);
    setColour (juce::TextButton::buttonOnColourId, theme::bg3);
    setColour (juce::TextButton::textColourOffId, theme::text0);
    setColour (juce::TextButton::textColourOnId, theme::text0);
    setColour (juce::ToggleButton::textColourId, theme::text0);
    setColour (juce::ToggleButton::tickColourId, theme::accent);
    setColour (juce::ToggleButton::tickDisabledColourId, theme::text2);

    setColour (juce::Slider::backgroundColourId, theme::bg3);
    setColour (juce::Slider::trackColourId, theme::accent);
    setColour (juce::Slider::thumbColourId, theme::capTop);

    setColour (juce::PopupMenu::backgroundColourId, theme::bg1);
    setColour (juce::PopupMenu::textColourId, theme::text0);
    setColour (juce::PopupMenu::headerTextColourId, theme::text2);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, theme::bg3);
    setColour (juce::PopupMenu::highlightedTextColourId, theme::text0);

    setColour (juce::ComboBox::backgroundColourId, theme::bg2);
    setColour (juce::ComboBox::textColourId, theme::text0);
    setColour (juce::ComboBox::outlineColourId, theme::line);
    setColour (juce::ComboBox::arrowColourId, theme::text1);
    setColour (juce::ComboBox::focusedOutlineColourId, theme::accent);

    setColour (juce::TextEditor::backgroundColourId, theme::bg0);
    setColour (juce::TextEditor::textColourId, theme::text0);
    setColour (juce::TextEditor::outlineColourId, theme::line);
    setColour (juce::TextEditor::focusedOutlineColourId, theme::accent);
    setColour (juce::TextEditor::highlightColourId, theme::accent.withAlpha (0.28f));
    setColour (juce::TextEditor::highlightedTextColourId, theme::text0);
    setColour (juce::CaretComponent::caretColourId, theme::accent);

    setColour (juce::ScrollBar::thumbColourId, theme::lineStrong);
    setColour (juce::ScrollBar::trackColourId, juce::Colours::transparentBlack);

    setColour (juce::TooltipWindow::backgroundColourId, theme::bg2);
    setColour (juce::TooltipWindow::textColourId, theme::text0);
    setColour (juce::TooltipWindow::outlineColourId, theme::lineStrong);

    setColour (juce::AlertWindow::backgroundColourId, theme::bg1);
    setColour (juce::AlertWindow::textColourId, theme::text0);
    setColour (juce::AlertWindow::outlineColourId, theme::lineStrong);

    setColour (juce::DocumentWindow::textColourId, theme::text0);
}

void HeadroomLookAndFeel::drawFocusRing (juce::Graphics& g, juce::Rectangle<float> bounds, float radius)
{
    g.setColour (theme::accent);
    g.drawRoundedRectangle (bounds.reduced (0.5f), radius, 1.0f);
}

// ---------------------------------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------------------------------

juce::Font HeadroomLookAndFeel::getTextButtonFont (juce::TextButton&, int)
{
    return theme::sans (12.0f, theme::Sans::Medium);
}

int HeadroomLookAndFeel::getTextButtonWidthToFitText (juce::TextButton& button, int)
{
    const auto icon = button.getProperties()[style::icon].toString();
    const auto textW = (int) std::ceil (theme::textWidth (theme::sans (12.0f, theme::Sans::Medium), button.getButtonText()));
    return textW + 24 + (icon.isNotEmpty() ? 22 : 0);
}

void HeadroomLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour&,
                                                bool highlighted, bool down)
{
    HoverFade::attach (button);         // first paint: from here on hovering fades in and out over 150 ms

    const auto bounds = button.getLocalBounds().toFloat();
    const auto kind = button.getProperties()[style::button].toString();
    const bool enabled = button.isEnabled();
    const bool focused = button.hasKeyboardFocus (false);
    const float hover = enabled ? HoverFade::get (button, highlighted) : 0.0f;

    juce::Colour fill = theme::bg2, outline = theme::line;

    if (kind == "ghost" || kind == "link")
    {
        fill = juce::Colours::transparentBlack;
        outline = juce::Colours::transparentBlack;
    }
    else if (kind == "primary")
    {
        outline = theme::accent.withAlpha (enabled ? 1.0f : 0.35f);
    }
    else if (kind == "danger")
    {
        outline = theme::bad.withAlpha (0.7f);
    }

    if (enabled)
    {
        if (kind == "ghost" || kind == "link")
            fill = theme::text0.withAlpha (down ? 0.14f : 0.08f * hover);        // an overlay works on any background, banners included
        else
            fill = down ? theme::bg3.brighter (0.06f) : theme::bg2.interpolatedWith (theme::bg3, hover);
    }

    if (fill.getAlpha() > 0)
    {
        g.setColour (fill);
        g.fillRoundedRectangle (bounds, theme::kControlRadius);
    }

    if (focused)
    {
        drawFocusRing (g, bounds, theme::kControlRadius);
    }
    else if (outline.getAlpha() > 0)
    {
        g.setColour (outline);
        g.drawRoundedRectangle (bounds.reduced (0.5f), theme::kControlRadius - 0.5f, 1.0f);
    }
}

void HeadroomLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button, bool highlighted, bool)
{
    const auto kind = button.getProperties()[style::button].toString();
    const auto iconName = button.getProperties()[style::icon].toString();
    const bool enabled = button.isEnabled();

    juce::Colour colour = theme::text0;

    if (! enabled)                      colour = theme::text2;
    else if (kind == "ghost")           colour = theme::text1.interpolatedWith (theme::text0, HoverFade::get (button, highlighted));
    else if (kind == "link")            colour = theme::text0;
    else if (kind == "danger")          colour = theme::bad;

    const auto font = getTextButtonFont (button, button.getHeight());
    const auto text = button.getButtonText();
    const auto textW = theme::textWidth (font, text);
    const float iconSpace = iconName.isNotEmpty() ? 22.0f : 0.0f;
    const float groupW = textW + iconSpace;

    auto area = button.getLocalBounds().toFloat();
    const float startX = std::floor (area.getCentreX() - groupW * 0.5f);

    if (iconName.isNotEmpty())
        Icons::draw (g, iconName, juce::Point<float> (startX + 8.0f, area.getCentreY()), 16.0f, colour);

    g.setColour (colour);
    g.setFont (font);
    g.drawText (text, juce::Rectangle<float> (startX + iconSpace, area.getY(), textW + 2.0f, area.getHeight()),
                juce::Justification::centredLeft, false);
}

void HeadroomLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool)
{
    HoverFade::attach (button);
    // A switch on the left, label on the right. Settings uses its own SwitchToggle; this keeps stray ToggleButtons on-theme.
    const auto bounds = button.getLocalBounds().toFloat();
    const auto track = juce::Rectangle<float> (32.0f, 18.0f).withCentre ({ bounds.getX() + 16.0f, bounds.getCentreY() });
    const bool on = button.getToggleState();

    g.setColour (on ? theme::accent : (highlighted ? theme::bg3 : theme::bg2));
    g.fillRoundedRectangle (track, 9.0f);

    if (! on)
    {
        g.setColour (theme::lineStrong);
        g.drawRoundedRectangle (track.reduced (0.5f), 8.5f, 1.0f);
    }

    const float thumbX = on ? track.getRight() - 9.0f : track.getX() + 9.0f;
    g.setColour (on ? theme::bg0 : theme::text1);
    g.fillEllipse (juce::Rectangle<float> (12.0f, 12.0f).withCentre ({ thumbX, track.getCentreY() }));

    g.setColour (button.isEnabled() ? theme::text0 : theme::text2);
    g.setFont (theme::sans (13.0f));
    g.drawText (button.getButtonText(), bounds.withTrimmedLeft (44.0f), juce::Justification::centredLeft, true);
}

// ---------------------------------------------------------------------------------------------------
// Combo boxes
// ---------------------------------------------------------------------------------------------------

void HeadroomLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool isButtonDown, int, int, int, int,
                                        juce::ComboBox& box)
{
    const auto bounds = juce::Rectangle<float> ((float) width, (float) height);

    g.setColour (isButtonDown || box.isMouseOver (true) ? theme::bg3 : theme::bg2);
    g.fillRoundedRectangle (bounds, theme::kControlRadius);

    if (box.hasKeyboardFocus (false))
    {
        drawFocusRing (g, bounds, theme::kControlRadius);
    }
    else
    {
        g.setColour (theme::line);
        g.drawRoundedRectangle (bounds.reduced (0.5f), theme::kControlRadius - 0.5f, 1.0f);
    }

    Icons::draw (g, "chevron-down", juce::Point<float> ((float) width - 16.0f, (float) height * 0.5f), 16.0f,
                 box.isEnabled() ? theme::text1 : theme::text2);
}

juce::Font HeadroomLookAndFeel::getComboBoxFont (juce::ComboBox&)
{
    return theme::sans (13.0f);
}

void HeadroomLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (10, 1, box.getWidth() - 34, box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
}

// ---------------------------------------------------------------------------------------------------
// Popup menus
// ---------------------------------------------------------------------------------------------------

juce::Font HeadroomLookAndFeel::getPopupMenuFont()
{
    return theme::sans (13.0f);
}

void HeadroomLookAndFeel::drawPopupMenuBackgroundWithOptions (juce::Graphics& g, int width, int height, const juce::PopupMenu::Options&)
{
    g.fillAll (theme::bg1);
    g.setColour (theme::lineStrong);
    g.drawRect (0, 0, width, height, 1);
}

int HeadroomLookAndFeel::getPopupMenuBorderSizeWithOptions (const juce::PopupMenu::Options&)
{
    return 4;
}

int HeadroomLookAndFeel::getMenuWindowFlags()
{
    return 0;       // no OS drop shadow: menus are defined by a hairline like everything else
}

void HeadroomLookAndFeel::getIdealPopupMenuItemSizeWithOptions (const juce::String& text, bool isSeparator, int standardItemHeight,
                                                                int& idealWidth, int& idealHeight, const juce::PopupMenu::Options&)
{
    if (isSeparator)
    {
        idealWidth = 50;
        idealHeight = 9;
        return;
    }

    idealHeight = standardItemHeight > 0 ? standardItemHeight : 28;
    // 10 left + icon column (reserved: items with dots/icons) + tick or arrow on the right.
    idealWidth = (int) std::ceil (theme::textWidth (getPopupMenuFont(), text)) + 10 + 24 + 30;
}

void HeadroomLookAndFeel::getIdealPopupMenuSectionHeaderSizeWithOptions (const juce::String& text, int standardItemHeight,
                                                                         int& idealWidth, int& idealHeight, const juce::PopupMenu::Options&)
{
    idealHeight = (standardItemHeight > 0 ? standardItemHeight : 28) - 4;
    idealWidth = (int) std::ceil (theme::textWidth (theme::caps (10.0f), text.toUpperCase())) + 24;
}

void HeadroomLookAndFeel::drawPopupMenuSectionHeaderWithOptions (juce::Graphics& g, const juce::Rectangle<int>& area,
                                                                 const juce::String& name, const juce::PopupMenu::Options&)
{
    g.setColour (theme::text2);
    g.setFont (theme::caps (10.0f));
    g.drawText (name.toUpperCase(), area.withTrimmedLeft (10).withTrimmedTop (4), juce::Justification::centredLeft, false);
}

void HeadroomLookAndFeel::drawPopupMenuItemWithOptions (juce::Graphics& g, const juce::Rectangle<int>& area, bool isHighlighted,
                                                        const juce::PopupMenu::Item& item, const juce::PopupMenu::Options&)
{
    if (item.isSeparator)
    {
        g.setColour (theme::line);
        g.fillRect (area.getX() + 6, area.getCentreY(), area.getWidth() - 12, 1);
        return;
    }

    const bool active = item.isEnabled;
    const auto row = area.reduced (0, 1).toFloat();

    if (isHighlighted && active)
    {
        g.setColour (theme::bg3);
        g.fillRoundedRectangle (row, 4.0f);
    }

    auto textColour = item.colour != juce::Colour() ? item.colour : theme::text0;

    if (! active)
        textColour = theme::text2;

    float x = (float) area.getX() + 10.0f;

    if (item.image != nullptr)
    {
        item.image->drawWithin (g, juce::Rectangle<float> (x, row.getCentreY() - 8.0f, 16.0f, 16.0f),
                                juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize, active ? 1.0f : 0.4f);
        x += 24.0f;
    }

    const bool hasSub = item.subMenu != nullptr && (item.itemID == 0 || item.subMenu->getNumItems() > 0);
    float rightX = (float) area.getRight() - 10.0f;

    if (hasSub)
    {
        Icons::draw (g, "chevron-right", juce::Point<float> (rightX - 7.0f, row.getCentreY()), 14.0f, theme::text1);
        rightX -= 18.0f;
    }
    else if (item.isTicked)
    {
        Icons::draw (g, "check", juce::Point<float> (rightX - 7.0f, row.getCentreY()), 14.0f, theme::accent);
        rightX -= 18.0f;
    }

    if (item.shortcutKeyDescription.isNotEmpty())
    {
        g.setColour (theme::text2);
        g.setFont (theme::mono (11.0f));
        g.drawText (item.shortcutKeyDescription, juce::Rectangle<float> (x, row.getY(), rightX - x, row.getHeight()),
                    juce::Justification::centredRight, false);
    }

    g.setColour (textColour);
    g.setFont (getPopupMenuFont());
    g.drawText (item.text, juce::Rectangle<float> (x, row.getY(), rightX - x, row.getHeight()), juce::Justification::centredLeft, true);
}

// ---------------------------------------------------------------------------------------------------
// Sliders
// ---------------------------------------------------------------------------------------------------

int HeadroomLookAndFeel::getSliderThumbRadius (juce::Slider&)
{
    return 7;
}

void HeadroomLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos, float minSliderPos,
                                            float maxSliderPos, juce::Slider::SliderStyle sliderStyle, juce::Slider& slider)
{
    if (sliderStyle != juce::Slider::LinearHorizontal)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, sliderStyle, slider);
        return;
    }

    const bool enabled = slider.isEnabled();
    const float cy = (float) y + (float) height * 0.5f;
    const auto track = juce::Rectangle<float> ((float) x, cy - 2.0f, (float) width, 4.0f);

    g.setColour (theme::bg3);
    g.fillRoundedRectangle (track, 2.0f);

    g.setColour (enabled ? theme::accent : theme::lineStrong);
    g.fillRoundedRectangle (track.withRight (sliderPos), 2.0f);

    const auto thumb = juce::Rectangle<float> (14.0f, 14.0f).withCentre ({ sliderPos, cy });
    g.setColour (enabled ? theme::capTop : theme::text2);
    g.fillEllipse (thumb);
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.drawEllipse (thumb.reduced (0.5f), 1.0f);

    if (slider.hasKeyboardFocus (false))
    {
        g.setColour (theme::accent);
        g.drawEllipse (thumb.expanded (2.0f), 1.0f);
    }
}

// ---------------------------------------------------------------------------------------------------
// Scrollbars, tooltips, text editors
// ---------------------------------------------------------------------------------------------------

int HeadroomLookAndFeel::getDefaultScrollbarWidth()
{
    return 10;
}

void HeadroomLookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int, int, int width, int height, bool isVertical,
                                         int thumbStart, int thumbSize, bool isMouseOver, bool isMouseDown)
{
    auto thumb = isVertical ? juce::Rectangle<int> (0, thumbStart, width, thumbSize)
                            : juce::Rectangle<int> (thumbStart, 0, thumbSize, height);

    // 4 px pill centred in the 10 px lane: quiet until you go looking for it.
    thumb = isVertical ? thumb.withSizeKeepingCentre (4, thumb.getHeight()) : thumb.withSizeKeepingCentre (thumb.getWidth(), 4);

    g.setColour (isMouseDown ? theme::text2 : (isMouseOver ? theme::text2.withAlpha (0.8f) : theme::lineStrong));
    g.fillRoundedRectangle (thumb.toFloat(), 2.0f);
}

juce::Rectangle<int> HeadroomLookAndFeel::getTooltipBounds (const juce::String& text, juce::Point<int> screenPos,
                                                            juce::Rectangle<int> parentArea)
{
    juce::AttributedString s;
    s.append (text, theme::sans (12.0f), theme::text0);
    juce::TextLayout layout;
    layout.createLayout (s, 260.0f);

    const int w = (int) std::ceil (layout.getWidth()) + 20;
    const int h = (int) std::ceil (layout.getHeight()) + 12;

    return juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 16,
                                 screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6) : screenPos.y + 18, w, h)
        .constrainedWithin (parentArea);
}

void HeadroomLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height)
{
    g.fillAll (theme::bg2);
    g.setColour (theme::lineStrong);
    g.drawRect (0, 0, width, height, 1);

    juce::AttributedString s;
    s.append (text, theme::sans (12.0f), theme::text0);
    juce::TextLayout layout;
    layout.createLayout (s, 260.0f);
    layout.draw (g, juce::Rectangle<float> (10.0f, 6.0f, (float) width - 20.0f, (float) height - 12.0f));
}

void HeadroomLookAndFeel::fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    g.setColour (editor.findColour (juce::TextEditor::backgroundColourId));
    g.fillRoundedRectangle (0.0f, 0.0f, (float) width, (float) height, 4.0f);
}

void HeadroomLookAndFeel::drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    const auto bounds = juce::Rectangle<float> ((float) width, (float) height).reduced (0.5f);
    const bool focused = editor.hasKeyboardFocus (true) && ! editor.isReadOnly();

    g.setColour (editor.findColour (focused ? juce::TextEditor::focusedOutlineColourId : juce::TextEditor::outlineColourId));
    g.drawRoundedRectangle (bounds, 3.5f, 1.0f);
}
}
