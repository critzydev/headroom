// HeadroomLookAndFeel: the dark "quiet hardware" skin for every stock JUCE widget we use.
#pragma once

#include "Juce.h"

namespace hr
{
// Property keys read by the button drawing code (Button::getProperties()).
namespace style
{
    inline const juce::Identifier button { "hrStyle" };     // "" (default), "primary", "ghost", "link", "danger"
    inline const juce::Identifier icon   { "hrIcon" };      // icon file name drawn left of the label
}

class HeadroomLookAndFeel : public juce::LookAndFeel_V4
{
public:
    HeadroomLookAndFeel();
    ~HeadroomLookAndFeel() override = default;

    // Buttons
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    int getTextButtonWidthToFitText (juce::TextButton&, int buttonHeight) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;

    // Combo boxes
    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown, int buttonX, int buttonY,
                       int buttonW, int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;

    // Popup menus
    juce::Font getPopupMenuFont() override;
    void drawPopupMenuBackgroundWithOptions (juce::Graphics&, int width, int height, const juce::PopupMenu::Options&) override;
    void drawPopupMenuItemWithOptions (juce::Graphics&, const juce::Rectangle<int>& area, bool isHighlighted,
                                       const juce::PopupMenu::Item&, const juce::PopupMenu::Options&) override;
    void drawPopupMenuSectionHeaderWithOptions (juce::Graphics&, const juce::Rectangle<int>& area, const juce::String& name,
                                                const juce::PopupMenu::Options&) override;
    void getIdealPopupMenuItemSizeWithOptions (const juce::String& text, bool isSeparator, int standardItemHeight,
                                               int& idealWidth, int& idealHeight, const juce::PopupMenu::Options&) override;
    void getIdealPopupMenuSectionHeaderSizeWithOptions (const juce::String& text, int standardItemHeight,
                                                        int& idealWidth, int& idealHeight, const juce::PopupMenu::Options&) override;
    int getPopupMenuBorderSizeWithOptions (const juce::PopupMenu::Options&) override;
    int getMenuWindowFlags() override;

    // Sliders (horizontal bars in Settings)
    int getSliderThumbRadius (juce::Slider&) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos, float minSliderPos,
                           float maxSliderPos, juce::Slider::SliderStyle, juce::Slider&) override;

    // Scrollbars
    int getDefaultScrollbarWidth() override;
    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height, bool isVertical,
                        int thumbStart, int thumbSize, bool isMouseOver, bool isMouseDown) override;

    // Tooltips
    juce::Rectangle<int> getTooltipBounds (const juce::String& text, juce::Point<int> screenPos, juce::Rectangle<int> parentArea) override;
    void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;

    // Text editors (inline rename)
    void fillTextEditorBackground (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int width, int height, juce::TextEditor&) override;

    // Shared by widgets that draw themselves: a keyboard-focus ring (1 px accent, inset).
    static void drawFocusRing (juce::Graphics&, juce::Rectangle<float> bounds, float radius);
};
}
