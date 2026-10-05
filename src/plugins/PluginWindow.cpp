// PluginWindow implementation.
#include "plugins/PluginWindow.h"

#include "ui/Theme.h"

namespace hr
{
namespace
{
    // The generic editor is plain JUCE widgets: give them the app's dark colours explicitly, so they are readable
    // whichever LookAndFeel happens to be installed. (Children inherit colours set on the editor.)
    void styleGenericEditor (juce::Component& editor)
    {
        using namespace juce;

        editor.setColour (ResizableWindow::backgroundColourId, theme::bg1);
        editor.setColour (Label::textColourId, theme::text0);
        editor.setColour (Label::backgroundColourId, Colours::transparentBlack);

        editor.setColour (Slider::backgroundColourId, theme::bg2);
        editor.setColour (Slider::trackColourId, theme::lineStrong);
        editor.setColour (Slider::thumbColourId, Colour (0xffd9d6d0));
        editor.setColour (Slider::rotarySliderFillColourId, theme::accent);
        editor.setColour (Slider::rotarySliderOutlineColourId, theme::bg3);
        editor.setColour (Slider::textBoxTextColourId, theme::text0);
        editor.setColour (Slider::textBoxBackgroundColourId, theme::bg2);
        editor.setColour (Slider::textBoxOutlineColourId, theme::line);
        editor.setColour (Slider::textBoxHighlightColourId, theme::bg3);

        editor.setColour (ToggleButton::textColourId, theme::text0);
        editor.setColour (ToggleButton::tickColourId, theme::accent);
        editor.setColour (ToggleButton::tickDisabledColourId, theme::text2);

        editor.setColour (TextButton::buttonColourId, theme::bg2);
        editor.setColour (TextButton::buttonOnColourId, theme::bg3);
        editor.setColour (TextButton::textColourOffId, theme::text0);
        editor.setColour (TextButton::textColourOnId, theme::text0);

        editor.setColour (ComboBox::backgroundColourId, theme::bg2);
        editor.setColour (ComboBox::textColourId, theme::text0);
        editor.setColour (ComboBox::outlineColourId, theme::line);
        editor.setColour (ComboBox::arrowColourId, theme::text1);
        editor.setColour (ComboBox::buttonColourId, theme::bg2);

        editor.setColour (ScrollBar::thumbColourId, theme::lineStrong);
    }
}

bool PluginWindow::isPositionUsable (const juce::Rectangle<int>& bounds)
{
    if (bounds.isEmpty())
        return false;

    // A strip the height of a title bar along the top edge must overlap the displays by a grabbable amount.
    const auto grabStrip = bounds.withHeight (32);

    for (const auto& display : juce::Desktop::getInstance().getDisplays().displays)
    {
        const auto overlap = display.userBounds.getIntersection (grabStrip.toFloat());

        if (overlap.getWidth() >= 64.0f && overlap.getHeight() >= 16.0f)
            return true;
    }

    return false;
}

PluginWindow::PluginWindow (juce::AudioProcessor& processor, const juce::String& title, std::function<void()> onCloseRequested,
                            std::optional<juce::Rectangle<int>> rememberedBounds)
    : juce::DocumentWindow (title, theme::bg1, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
      closeCallback (std::move (onCloseRequested))
{
    setUsingNativeTitleBar (true);

    // createEditor() is private since JUCE 8.0.13: createEditorAndMakeActive() is the supported entry point.
    juce::AudioProcessorEditor* editor = processor.hasEditor() ? processor.createEditorAndMakeActive() : nullptr;

    if (editor == nullptr)
    {
        editor = new juce::GenericAudioProcessorEditor (processor);
        styleGenericEditor (*editor);
        genericEditor = true;
    }

    setContentOwned (editor, true);

    const bool editorIsResizable = editor->isResizable();
    setResizable (editorIsResizable, false);

    if (editorIsResizable)
        if (auto* editorConstrainer = editor->getConstrainer())
            setConstrainer (editorConstrainer);

    if (rememberedBounds.has_value() && isPositionUsable (*rememberedBounds))
    {
        if (editorIsResizable)
            setBounds (*rememberedBounds);
        else
            setTopLeftPosition (rememberedBounds->getPosition());
    }
    else
    {
        centreWithSize (getWidth(), getHeight());
    }

    setVisible (true);

#if JUCE_LINUX
    // XWayland keyboard focus bug (JUCE #1754): the editor doesn't get keys until it is asked again.
    juce::Component::SafePointer<juce::Component> safeEditor (editor);
    juce::Timer::callAfterDelay (200, [safeEditor]
    {
        if (safeEditor != nullptr)
            safeEditor->grabKeyboardFocus();
    });
#endif
}

PluginWindow::~PluginWindow()
{
    setConstrainer (nullptr);   // it belongs to the editor, which is about to go
    clearContentComponent();    // deletes the editor while the window (and processor) are still alive
}

void PluginWindow::closeButtonPressed()
{
    if (closeCallback)
        closeCallback();
}
}
