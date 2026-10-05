// UpdateSheet and UninstallSheet implementation.
#include "ui/setup/SetupSheets.h"

#include "ui/HeadroomLookAndFeel.h"
#include "ui/Theme.h"

namespace hr
{
// ---------------------------------------------------------------------------------------------------
// UpdateSheet
// ---------------------------------------------------------------------------------------------------

UpdateSheet::UpdateSheet (const juce::String& newVersion, const juce::String& installedVersion,
                          std::function<void()> onUpdate, std::function<void()> onNotNow)
    : heading ("Update Headroom"), notNow (std::move (onNotNow))
{
    body = "Version " + newVersion + " is ready, and " + (installedVersion.isNotEmpty() ? "version " + installedVersion : juce::String ("an older version"))
           + " is installed. Updating replaces it and restarts Headroom. Your settings and channels stay as they are.";

    updateButton.getProperties().set (style::button, "primary");
    updateButton.onClick = std::move (onUpdate);
    addAndMakeVisible (updateButton);

    notNowButton.getProperties().set (style::button, "ghost");
    notNowButton.onClick = [this] { if (notNow) notNow(); };
    addAndMakeVisible (notNowButton);
}

UpdateSheet::~UpdateSheet()
{
    stopTimer();
}

void UpdateSheet::setBusy()
{
    busy = true;
    updateButton.setEnabled (false);
    notNowButton.setEnabled (false);
    startTimerHz (30);
    repaint();
}

void UpdateSheet::showError (const juce::String& plainSentence)
{
    busy = false;
    stopTimer();
    errorText = plainSentence;
    heading = "The update did not finish";
    updateButton.setVisible (false);
    notNowButton.setEnabled (true);
    notNowButton.setButtonText ("Close");
    repaint();
}

void UpdateSheet::resized()
{
    auto row = getLocalBounds().reduced (24).removeFromBottom (32);
    updateButton.setBounds (row.removeFromRight (96));
    row.removeFromRight (8);
    notNowButton.setBounds (row.removeFromRight (88));
}

void UpdateSheet::paint (juce::Graphics& g)
{
    g.setColour (theme::text0);
    g.setFont (theme::sans (15.0f, theme::Sans::SemiBold));
    g.drawText (heading, juce::Rectangle<int> (24, 24, getWidth() - 48, 24), juce::Justification::centredLeft, true);

    g.setColour (errorText.isNotEmpty() ? theme::warn : theme::text1);
    g.setFont (theme::sans (13.0f));
    g.drawFittedText (errorText.isNotEmpty() ? errorText : body, juce::Rectangle<int> (24, 58, getWidth() - 48, 90), juce::Justification::topLeft, 5, 1.0f);

    if (busy)
    {
        const auto track = juce::Rectangle<float> (24.0f, (float) getHeight() - 24.0f - 16.0f, (float) getWidth() - 48.0f, 3.0f);
        g.setColour (theme::bg2);
        g.fillRoundedRectangle (track, 1.5f);

        const float length = track.getWidth() * 0.22f;
        const float x = std::fmod ((float) juce::Time::getMillisecondCounterHiRes() * 0.28f, track.getWidth() + length) - length;
        g.saveState();
        g.reduceClipRegion (track.toNearestInt());
        g.setColour (theme::accent);
        g.fillRoundedRectangle (juce::Rectangle<float> (track.getX() + x, track.getY(), length, track.getHeight()), 1.5f);
        g.restoreState();

        g.setColour (theme::text1);
        g.setFont (theme::sans (12.0f));
        g.drawText ("Updating" + theme::glyph::ellipsis, juce::Rectangle<int> (24, getHeight() - 24 - 16 - 22, 200, 18), juce::Justification::centredLeft, false);
    }
}

// ---------------------------------------------------------------------------------------------------
// UninstallSheet
// ---------------------------------------------------------------------------------------------------

UninstallSheet::UninstallSheet (std::function<void (bool)> onConfirm, std::function<void()> onCancel)
{
    cancelButton.onClick = std::move (onCancel);
    addAndMakeVisible (cancelButton);

    confirmButton.getProperties().set (style::button, "danger");
    confirmButton.onClick = [this, onConfirm = std::move (onConfirm)] { onConfirm (deleteSettings.getToggleState()); };
    addAndMakeVisible (confirmButton);

    deleteSettings.setToggleState (false, juce::dontSendNotification);
    addAndMakeVisible (deleteSettings);
}

void UninstallSheet::resized()
{
    auto row = getLocalBounds().reduced (24).removeFromBottom (32);
    confirmButton.setBounds (row.removeFromRight (getLookAndFeel().getTextButtonWidthToFitText (confirmButton, 32) + 8));
    row.removeFromRight (8);
    cancelButton.setBounds (row.removeFromRight (80));

    deleteSettings.setBounds (24, 150, 18, 18);
}

void UninstallSheet::paint (juce::Graphics& g)
{
    g.setColour (theme::text0);
    g.setFont (theme::sans (15.0f, theme::Sans::SemiBold));
    g.drawText ("Uninstall Headroom?", juce::Rectangle<int> (24, 24, getWidth() - 48, 24), juce::Justification::centredLeft, true);

    g.setColour (theme::text1);
    g.setFont (theme::sans (13.0f));
    g.drawFittedText ("Headroom quits and is removed from your app menu and from starting at login. Your sound goes back to your normal speakers "
                      "and microphone. The downloaded AppImage file is not touched.",
                      juce::Rectangle<int> (24, 58, getWidth() - 48, 80), juce::Justification::topLeft, 4, 1.0f);

    g.setColour (theme::text0);
    g.setFont (theme::sans (13.0f));
    g.drawText ("Also delete my settings", juce::Rectangle<int> (52, 146, getWidth() - 76, 20), juce::Justification::centredLeft, true);

    g.setColour (theme::text2);
    g.setFont (theme::sans (12.0f));
    g.drawFittedText ("Your channels, voice effects and levels. Leave it off to keep them for later.",
                      juce::Rectangle<int> (52, 166, getWidth() - 76, 34), juce::Justification::topLeft, 2, 1.0f);
}
}
