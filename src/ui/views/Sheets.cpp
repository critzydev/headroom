// Sheets implementation.
#include "ui/views/Sheets.h"

#include "ui/Focus.h"
#include "ui/HeadroomLookAndFeel.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

namespace hr
{
SheetHost::SheetHost()
{
    setWantsKeyboardFocus (true);
    setInterceptsMouseClicks (true, true);
    setFocusContainerType (FocusContainerType::keyboardFocusContainer);      // Tab stays inside the sheet while it is open
    setVisible (false);
}

void SheetHost::show (std::unique_ptr<juce::Component> newContent, int width, int height)
{
    hide();
    content = std::move (newContent);
    panelWidth = width;
    panelHeight = height;
    addAndMakeVisible (*content);
    setVisible (true);
    toFront (false);
    resized();

    // Start on the first button so Return / Space do the obvious thing and Tab stays in the sheet.
    for (auto* child : content->getChildren())
    {
        if (child->getWantsKeyboardFocus())
        {
            focusIfShowing (*child);
            return;
        }
    }

    focusIfShowing (*this);
}

void SheetHost::hide()
{
    if (content != nullptr)
    {
        removeChildComponent (content.get());

        // hide() is usually called from one of the sheet's own buttons: let that callback finish before the
        // button (and the std::function it is running) is destroyed.
        std::shared_ptr<juce::Component> dying (std::move (content));
        juce::MessageManager::callAsync ([dying] {});
    }

    setVisible (false);
}

void SheetHost::resized()
{
    panel = juce::Rectangle<int> (panelWidth, panelHeight).withCentre (getLocalBounds().getCentre());
    panel = panel.constrainedWithin (getLocalBounds().reduced (16));

    if (content != nullptr)
        content->setBounds (panel);
}

void SheetHost::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black.withAlpha (0.62f));

    if (content == nullptr)
        return;

    g.setColour (theme::bg1);
    g.fillRoundedRectangle (panel.toFloat(), theme::kPanelRadius);
    g.setColour (theme::lineStrong);
    g.drawRoundedRectangle (panel.toFloat().reduced (0.5f), theme::kPanelRadius - 0.5f, 1.0f);
}

bool SheetHost::keyPressed (const juce::KeyPress& key)
{
    if (key.getKeyCode() == juce::KeyPress::escapeKey)
    {
        if (onEscape)
            onEscape();

        return true;
    }

    if (key.getKeyCode() == juce::KeyPress::returnKey && onEnter)
    {
        onEnter();
        return true;
    }

    return key.getKeyCode() != juce::KeyPress::tabKey;      // modal: keys don't leak to the mixer underneath, Tab moves between buttons
}

// ---------------------------------------------------------------------------------------------------
// FirstRunSheet
// ---------------------------------------------------------------------------------------------------

FirstRunSheet::FirstRunSheet (const BackendStatus& status, const juce::String& chatChannelName, std::function<void()> onDone)
{
    Line mic;

    if (status.mic == MicState::NotFound || status.inputLabel.isEmpty())
    {
        mic = { "No mic yet", "Plug in a microphone and Headroom will pick it up by itself.", false };
    }
    else
    {
        mic = { "Mic found", status.inputLabel + " is your microphone. Headroom keeps it working after every restart.", true };
    }

    lines.push_back (mic);
    lines.push_back ({ "Channels ready", "System, Game, Music and Chat are set up. Apps you open are sorted into them automatically.", true });
    lines.push_back ({ "One thing in Discord", "Under Voice & Video, set Input to Headroom Mic and Output to Headroom " + chatChannelName + ".", true });

    doneButton.getProperties().set (style::button, "primary");
    doneButton.onClick = std::move (onDone);
    addAndMakeVisible (doneButton);
}

void FirstRunSheet::resized()
{
    doneButton.setBounds (getWidth() - 32 - 96, getHeight() - 32 - 32, 96, 32);
}

void FirstRunSheet::paint (juce::Graphics& g)
{
    g.setColour (theme::text0);
    g.setFont (theme::sans (20.0f, theme::Sans::SemiBold));
    g.drawText ("You" + theme::glyph::quoteClose + "re set up", juce::Rectangle<int> (32, 28, getWidth() - 64, 30), juce::Justification::centredLeft, false);

    int y = 84;

    for (const auto& line : lines)
    {
        Icons::draw (g, line.good ? "check" : "triangle-alert", juce::Point<float> (32.0f + 8.0f, (float) y + 10.0f), 16.0f,
                     line.good ? theme::ok : theme::warn);

        g.setColour (theme::text0);
        g.setFont (theme::sans (13.0f, theme::Sans::SemiBold));
        g.drawText (line.title, juce::Rectangle<int> (60, y, getWidth() - 92, 20), juce::Justification::centredLeft, true);

        g.setColour (theme::text1);
        g.setFont (theme::sans (13.0f));
        g.drawFittedText (line.text, juce::Rectangle<int> (60, y + 22, getWidth() - 92, 38), juce::Justification::topLeft, 2, 1.0f);

        y += 68;
    }
}

// ---------------------------------------------------------------------------------------------------
// ConfirmSheet
// ---------------------------------------------------------------------------------------------------

ConfirmSheet::ConfirmSheet (const juce::String& title, const juce::String& message, const juce::String& confirmLabel,
                            std::function<void()> onConfirm, std::function<void()> onCancel)
    : heading (title), body (message)
{
    cancelButton.onClick = std::move (onCancel);
    addAndMakeVisible (cancelButton);

    confirmButton.setButtonText (confirmLabel);
    confirmButton.getProperties().set (style::button, "danger");
    confirmButton.onClick = std::move (onConfirm);
    addAndMakeVisible (confirmButton);
}

void ConfirmSheet::resized()
{
    auto row = getLocalBounds().reduced (24).removeFromBottom (32);
    confirmButton.setBounds (row.removeFromRight (getLookAndFeel().getTextButtonWidthToFitText (confirmButton, 32) + 8));
    row.removeFromRight (8);
    cancelButton.setBounds (row.removeFromRight (80));
}

void ConfirmSheet::paint (juce::Graphics& g)
{
    g.setColour (theme::text0);
    g.setFont (theme::sans (15.0f, theme::Sans::SemiBold));
    g.drawText (heading, juce::Rectangle<int> (24, 24, getWidth() - 48, 24), juce::Justification::centredLeft, true);

    g.setColour (theme::text1);
    g.setFont (theme::sans (13.0f));
    g.drawFittedText (body, juce::Rectangle<int> (24, 58, getWidth() - 48, 70), juce::Justification::topLeft, 4, 1.0f);
}
}
