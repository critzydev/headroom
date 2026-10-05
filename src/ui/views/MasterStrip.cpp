// MasterStrip: same vertical rhythm as a channel strip so the faders line up.
#include "ui/views/MasterStrip.h"

#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/views/StripLayout.h"

namespace hr
{
MasterStrip::MasterStrip (AppController& c) : controller (c)
{
    protectToggle.setLabel ("Protect my ears");
    protectToggle.setTooltip ("Keeps sudden loud sounds from hurting your ears. Leave this on.");
    protectToggle.onClick = [this] { controller.setLimiterEnabled (Mix::Monitor, protectToggle.getToggleState()); };
    addAndMakeVisible (protectToggle);

    fader.onChange = [this] (float db) { controller.setMasterLevel (mix, db); };
    fader.setTooltip ("Overall level of this mix. Hold Shift for fine control, double-click for 0 dB.");
    addAndMakeVisible (fader);

    muteButton.onClick = [this] { controller.setMasterMuted (mix, ! muteButton.getToggleState()); };
    addAndMakeVisible (muteButton);
}

void MasterStrip::sync (Mix shownMix, const juce::String& deviceLabel, int extraDevices)
{
    const bool mixChanged = shownMix != mix;
    mix = shownMix;

    if (mixChanged)
    {
        meter = {};
        fader.clearLevels();
    }

    const auto mixTree = controller.getState().getMix (mix);
    const float db = mixTree.isValid() ? (float) (double) mixTree[ids::gainDb] : 0.0f;
    const bool muted = mixTree.isValid() && (int) mixTree[ids::muted] != 0;
    const bool limiter = ! mixTree.isValid() || mixTree[ids::limiter].isVoid() || (int) mixTree[ids::limiter] != 0;

    fader.setDb (db);
    fader.setMuted (muted);
    muteButton.setToggleState (muted, juce::dontSendNotification);
    muteButton.setTooltip (muted ? "Unmute" : "Mute this mix");
    protectToggle.setToggleState (limiter, juce::dontSendNotification);
    protectToggle.setVisible (mix == Mix::Monitor);

    // "Elgato Wave:3 Headphones" -> "Wave:3": the strip is already called Headphones. More devices: "Wave:3 +1".
    auto shortDevice = deviceLabel;

    if (shortDevice.startsWithIgnoreCase ("Elgato "))
        shortDevice = shortDevice.substring (7);

    if (shortDevice.endsWithIgnoreCase (" Headphones"))
        shortDevice = shortDevice.dropLastCharacters (11);

    const auto extra = extraDevices > 0 ? " +" + juce::String (extraDevices) : juce::String();
    const auto newLine = (mix == Mix::Monitor ? shortDevice : juce::String ("Sent to OBS")) + extra;

    if (newLine != lineText || mixChanged)
    {
        lineText = newLine;
        repaint();
    }
}

void MasterStrip::pushLevels (const PeakPair& peaks, double dt)
{
    meter.push (peaks, dt);
    fader.setLevels (meter.l, meter.r);
}

void MasterStrip::resized()
{
    const auto layout = StripLayout::compute (getLocalBounds());

    capsArea = layout.caps;
    nameArea = layout.name;
    lineArea = layout.line;

    protectToggle.setBounds (layout.slot);
    fader.setBounds (layout.fader);
    muteButton.setBounds (layout.bottom.withSizeKeepingCentre (32, 32));
}

void MasterStrip::paint (juce::Graphics& g)
{
    g.setColour (theme::bg1);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), theme::kPanelRadius);

    if (! g.getClipBounds().intersects (getLocalBounds().withHeight (lineArea.getBottom() + 2)))
        return;

    Icons::draw (g, mix == Mix::Monitor ? "headphones" : "radio", capsArea.withWidth (20).toFloat(), 20.0f, theme::text1);

    g.setColour (theme::text2);
    g.setFont (theme::caps (10.0f));
    g.drawText ("MASTER", capsArea, juce::Justification::centredRight, false);

    g.setColour (theme::text0);
    g.setFont (theme::sans (15.0f, theme::Sans::SemiBold));
    g.drawText (mix == Mix::Monitor ? "Headphones" : "Stream", nameArea, juce::Justification::centredLeft, true);

    g.setColour (theme::text2);
    g.setFont (theme::sans (11.0f));
    g.drawText (lineText, lineArea, juce::Justification::centredLeft, true);
}
}
