// ChannelStrip and AddChannelStrip.
#include "ui/views/ChannelStrip.h"

#include "ui/Focus.h"
#include "ui/HeadroomLookAndFeel.h"
#include "ui/Icons.h"
#include "ui/MicStatus.h"
#include "ui/Theme.h"
#include "ui/views/StripLayout.h"
#include "ui/widgets/Dropdown.h"
#include "ui/widgets/HoverFade.h"

namespace hr
{
namespace
{
    const char* const kColourNames[] = { "Amber", "Teal", "Blue", "Violet", "Pink", "Green", "Sand", "Slate" };

    constexpr int kColourIdBase = 100, kIconIdBase = 200, kInputIdBase = 300, kRenameId = 1, kRemoveId = 2, kStereoId = 3;
}

ChannelStrip::ChannelStrip (AppController& c, const juce::String& id)
    : controller (c), channelId (id)
{
    setTitle (id);

    fxButton.onClick = [this] { if (onToggleFx) onToggleFx (channelId); };
    fxButton.setTooltip ("Effects for this channel");
    addAndMakeVisible (fxButton);

    fader.onChange = [this] (float db) { controller.setChannelLevel (channelId, ctx.mix, db); };
    fader.setTooltip ("Drag to set the level. Hold Shift for fine control, double-click for 0 dB.");
    addAndMakeVisible (fader);

    muteButton.onClick = [this] { controller.toggleMute (channelId, ctx.mix); };
    addAndMakeVisible (muteButton);

    linkButton.onClick = [this] { controller.setChannelLinked (channelId, ! info.linked); };
    addAndMakeVisible (linkButton);
}

void ChannelStrip::sync (const Context& newContext)
{
    const auto fresh = AppState::describe (controller.getState().findChannel (channelId));
    const bool mixChanged = newContext.mix != ctx.mix;
    const bool visualChange = fresh.name != info.name || fresh.colour != info.colour || fresh.icon != info.icon
                              || newContext.micLabel != ctx.micLabel || newContext.routedApps != ctx.routedApps
                              || newContext.deviceLabel != ctx.deviceLabel || newContext.deviceConnected != ctx.deviceConnected
                              || fresh.device != info.device;

    info = fresh;
    ctx = newContext;

    if (mixChanged)
    {
        meter = {};
        fader.clearLevels();
    }

    fader.setDb (info.getDb (ctx.mix));
    fader.setMuted (info.isMuted (ctx.mix));
    muteButton.setToggleState (info.isMuted (ctx.mix), juce::dontSendNotification);
    linkButton.setToggleState (info.linked, juce::dontSendNotification);
    fxButton.setState (ctx.activeEffects, ctx.drawerOpen);

    muteButton.setTooltip (info.isMuted (ctx.mix) ? "Unmute" : "Mute");

    // An Elgato Wave already plays your own voice in your headphones without delay: the mic's send starts at -inf. Another mic
    // does not, and a delayed copy of your own voice is not something to start with either.
    const bool elgatoMic = ctx.micLabel.startsWithIgnoreCase ("Elgato ");
    fader.setTooltip (info.isMic() && ctx.mix == Mix::Monitor
                          ? (elgatoMic ? "Your " + shortDeviceName (ctx.micLabel) + " already lets you hear yourself without delay "
                                         : juce::String ("Headroom does not play your mic in your headphones, so you do not hear yourself late "))
                                + theme::glyph::emDash + " turn this up only if you want to hear the effects."
                          : juce::String ("Drag to set the level. Hold Shift for fine control, double-click for 0 dB."));
    linkButton.setTooltip (info.linked ? "Linked: one fader moves both mixes. Click to unlink."
                                       : "Link the Headphones and Stream levels");

    if (visualChange)
    {
        updateRoutedText();
        refreshTooltip();
        repaint();
    }
}

void ChannelStrip::updateRoutedText()
{
    if (info.isMic())
        routedText = ctx.micLabel.isNotEmpty() ? ctx.micLabel : juce::String ("No mic found");
    else if (info.isInput())
        routedText = isInputConnected() ? shortDeviceName (ctx.deviceLabel) : juce::String ("Not connected");
    else
        routedText = describeRouted (ctx.routedApps);
}

// An input channel whose device is on the machine (the line under its name is then the device).
bool ChannelStrip::isInputConnected() const
{
    return info.isInput() && info.device.isNotEmpty() && ctx.deviceConnected && ctx.deviceLabel.isNotEmpty();
}

juce::String ChannelStrip::describeRouted (const juce::StringArray& apps) const
{
    if (apps.isEmpty())
        return "No apps";

    const auto joined = apps.joinIntoString (", ");
    const float available = (float) lineArea.getWidth();

    if (theme::textWidth (theme::sans (11.0f), joined) <= available)
        return joined;

    return juce::String (apps.size()) + (apps.size() == 1 ? " app" : " apps");
}

void ChannelStrip::refreshTooltip()
{
    juce::String tip = info.name;

    if (info.isMic())
        tip << "\n" << (ctx.micLabel.isNotEmpty() ? ctx.micLabel : juce::String ("No mic found"));
    else if (info.isInput())
        tip << "\n" << (isInputConnected() ? ctx.deviceLabel : (ctx.deviceLabel.isNotEmpty() ? ctx.deviceLabel + " (not connected)" : juce::String ("No input chosen")));
    else if (ctx.routedApps.size() > 0)
        tip << "\n" << ctx.routedApps.joinIntoString (", ");

    setTooltip (tip + "\nDouble-click the name to rename. Right-click for more.");
}

void ChannelStrip::pushLevels (const ChannelLevels& levels, double dt)
{
    // The mic shows what the Wave:3 hears no matter which mix is selected (its headphone send is muted by default).
    const auto& peaks = info.isMic() ? levels.input : (ctx.mix == Mix::Monitor ? levels.monitor : levels.stream);
    meter.push (peaks, dt);
    fader.setLevels (meter.l, meter.r);
}

void ChannelStrip::resized()
{
    const auto layout = StripLayout::compute (getLocalBounds());

    capsArea = layout.caps;
    nameArea = layout.name;
    iconArea = layout.caps.withWidth (20);
    lineArea = layout.line;
    updateRoutedText();

    fxButton.setBounds (layout.slot);
    fader.setBounds (layout.fader);

    auto bottom = layout.bottom;
    const int pairWidth = 32 + 8 + 32;
    auto pair = bottom.withSizeKeepingCentre (pairWidth, 32);
    muteButton.setBounds (pair.removeFromLeft (32));
    linkButton.setBounds (pair.removeFromRight (32));

    if (renameEditor != nullptr)
        renameEditor->setBounds (layout.name.expanded (0, 2));
}

void ChannelStrip::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const auto clip = g.getClipBounds();

    g.setColour (theme::bg1);
    g.fillRoundedRectangle (bounds, theme::kPanelRadius);

    // Header parts only repaint when something changed there; the meter's 30 fps clip never reaches them.
    if (! clip.intersects (getLocalBounds().withHeight (lineArea.getBottom() + 2)))
        return;

    {
        juce::Graphics::ScopedSaveState state (g);
        juce::Path shape;
        shape.addRoundedRectangle (bounds, theme::kPanelRadius);
        g.reduceClipRegion (shape);
        g.setColour (theme::colourFromHex (info.colour));
        g.fillRect (0, 0, getWidth(), 3);
    }

    Icons::draw (g, Icons::fileForChannelIcon (info.icon), iconArea.toFloat(), 20.0f, theme::text1);

    if (info.isMic() || info.isInput())
    {
        g.setColour (theme::text2);
        g.setFont (theme::caps (10.0f));
        g.drawText ("INPUT", capsArea, juce::Justification::centredRight, false);
    }

    if (renameEditor == nullptr)
    {
        g.setColour (theme::text0);
        g.setFont (theme::sans (15.0f, theme::Sans::SemiBold));
        g.drawText (info.name, nameArea, juce::Justification::centredLeft, true);
    }

    // A device that is not there is the one thing on a strip that is a state to notice: amber, like the other warnings.
    g.setColour (info.isInput() && ! isInputConnected() ? theme::warn : theme::text2);
    g.setFont (theme::sans (11.0f));
    g.drawText (routedText, lineArea, juce::Justification::centredLeft, true);
}

// ---------------------------------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------------------------------

void ChannelStrip::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() && e.eventComponent == this)
        showMenu();
}

void ChannelStrip::mouseUp (const juce::MouseEvent&) {}

void ChannelStrip::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (e.eventComponent == this && (nameArea.expanded (26, 4).contains (e.getPosition())))
        beginRename();
}

void ChannelStrip::showMenu()
{
    juce::PopupMenu menu, colours, icons;
    const auto& palette = getChannelColourPalette();

    for (size_t i = 0; i < palette.size(); ++i)
    {
        juce::PopupMenu::Item item (i < 8 ? kColourNames[i] : "Colour");
        item.setID (kColourIdBase + (int) i)
            .setTicked (palette[i].equalsIgnoreCase (info.colour))
            .setImage (Dropdown::makeDot (theme::colourFromHex (palette[i])));
        colours.addItem (std::move (item));
    }

    const auto& choices = Icons::channelIconChoices();

    for (int i = 0; i < choices.size(); ++i)
    {
        juce::PopupMenu::Item item (Icons::displayNameForChannelIcon (choices[i]));
        item.setID (kIconIdBase + i)
            .setTicked (choices[i] == info.icon)
            .setImage (Icons::createDrawable (Icons::fileForChannelIcon (choices[i]), 16.0f, theme::text1));
        icons.addItem (std::move (item));
    }

    menu.addItem (kRenameId, "Rename");
    menu.addSubMenu ("Colour", colours);
    menu.addSubMenu ("Icon", icons);

    // An input channel can listen to another device, and take the device's left and right instead of its first channel.
    const auto inputs = controller.getBackend().getInputs();

    if (info.isInput())
    {
        juce::PopupMenu devices;

        for (size_t i = 0; i < inputs.size(); ++i)
        {
            juce::PopupMenu::Item item (inputs[i].label);
            item.setID (kInputIdBase + (int) i).setTicked (inputs[i].id == info.device);
            devices.addItem (std::move (item));
        }

        if (inputs.empty())
            devices.addItem (kInputIdBase + 999, "No inputs found", false);

        menu.addSeparator();
        menu.addSubMenu ("Change input" + theme::glyph::ellipsis, devices);

        juce::PopupMenu::Item stereo ("Stereo (left and right)");
        stereo.setID (kStereoId).setTicked (info.stereo);
        menu.addItem (std::move (stereo));
    }

    if (! info.isMic())
    {
        menu.addSeparator();
        menu.addItem (kRemoveId, "Remove channel");
    }

    juce::Component::SafePointer<ChannelStrip> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options().withStandardItemHeight (28).withMinimumWidth (176),
                        [safe, choices, inputs] (int result)
                        {
                            if (safe == nullptr || result == 0)
                                return;

                            auto& c = safe->controller;
                            const auto& colourPalette = getChannelColourPalette();

                            if (result == kRenameId)
                                safe->beginRename();
                            else if (result == kRemoveId && safe->onRemove)
                                safe->onRemove (safe->channelId);
                            else if (result == kStereoId)
                                c.setChannelStereo (safe->channelId, ! safe->info.stereo);
                            else if (result >= kInputIdBase && result - kInputIdBase < (int) inputs.size())
                                c.setChannelDevice (safe->channelId, inputs[(size_t) (result - kInputIdBase)].id);
                            else if (result >= kIconIdBase && result - kIconIdBase < choices.size())
                                c.setChannelIcon (safe->channelId, choices[result - kIconIdBase]);
                            else if (result >= kColourIdBase && (size_t) (result - kColourIdBase) < colourPalette.size())
                                c.setChannelColour (safe->channelId, colourPalette[(size_t) (result - kColourIdBase)]);
                        });
}

void ChannelStrip::beginRename()
{
    if (renameEditor != nullptr)
        return;

    renameEditor = std::make_unique<juce::TextEditor>();
    auto& ed = *renameEditor;

    ed.setFont (theme::sans (15.0f, theme::Sans::SemiBold));
    ed.setJustification (juce::Justification::centredLeft);
    ed.setIndents (5, 0);
    ed.setBorder (juce::BorderSize<int> (0));
    ed.setInputRestrictions (24);
    ed.setText (info.name, false);
    ed.setBounds (StripLayout::compute (getLocalBounds()).name.expanded (0, 2));

    juce::Component::SafePointer<ChannelStrip> safe (this);

    ed.onReturnKey = [safe]
    {
        if (safe != nullptr && safe->renameEditor != nullptr)
            juce::MessageManager::callAsync ([safe, text = safe->renameEditor->getText()] { if (safe != nullptr) safe->finishRename (true, text); });
    };
    ed.onEscapeKey = [safe]
    {
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->finishRename (false, {}); });
    };
    ed.onFocusLost = [safe]
    {
        if (safe != nullptr && safe->renameEditor != nullptr)
            juce::MessageManager::callAsync ([safe, text = safe->renameEditor->getText()] { if (safe != nullptr) safe->finishRename (true, text); });
    };

    addAndMakeVisible (ed);
    focusIfShowing (ed);
    ed.selectAll();
    repaint();
}

void ChannelStrip::finishRename (bool commit, const juce::String& text)
{
    if (renameEditor == nullptr)
        return;

    auto editor = std::move (renameEditor);
    editor->onFocusLost = nullptr;
    removeChildComponent (editor.get());

    if (commit && text.trim().isNotEmpty() && text.trim() != info.name)
        controller.renameChannel (channelId, text);

    repaint();
}

// ---------------------------------------------------------------------------------------------------
// AddChannelStrip
// ---------------------------------------------------------------------------------------------------

AddChannelStrip::AddChannelStrip()
{
    setWantsKeyboardFocus (true);
    setMouseClickGrabsKeyboardFocus (false);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setTitle ("Add channel");
    setTooltip ("Add a channel for another app or group of apps, or for another microphone or input");
    HoverFade::attach (*this);
}

void AddChannelStrip::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);

    juce::Path outline;
    outline.addRoundedRectangle (bounds, theme::kPanelRadius);

    juce::Path dashed;
    const float dashes[] = { 4.0f, 4.0f };
    juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dashes, 2);

    const float fade = HoverFade::get (*this, hover);

    if (fade > 0.0f)
    {
        g.setColour (theme::bg1.withAlpha (fade));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), theme::kPanelRadius);
    }

    g.setColour (hasKeyboardFocus (false) ? theme::accent : theme::line.interpolatedWith (theme::lineStrong, fade));
    g.fillPath (dashed);

    const auto colour = theme::text1.interpolatedWith (theme::text0, fade);
    const auto centre = getLocalBounds().toFloat().getCentre();

    Icons::draw (g, "plus", juce::Point<float> (centre.x, centre.y - 12.0f), 20.0f, colour);
    g.setColour (colour);
    g.setFont (theme::sans (12.0f, theme::Sans::Medium));
    g.drawText ("Add channel", juce::Rectangle<float> (0.0f, centre.y + 2.0f, (float) getWidth(), 16.0f), juce::Justification::centred, false);
}

void AddChannelStrip::mouseUp (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu() && getLocalBounds().contains (e.getPosition()) && onClick)
        onClick();
}

bool AddChannelStrip::keyPressed (const juce::KeyPress& key)
{
    if (key.getKeyCode() == juce::KeyPress::returnKey || key.getKeyCode() == juce::KeyPress::spaceKey)
    {
        if (onClick)
            onClick();

        return true;
    }

    return false;
}
}
