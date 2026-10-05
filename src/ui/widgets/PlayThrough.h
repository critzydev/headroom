// PlayThrough: the "Play through" list: every device a mix plays to, with a tick each, in a small panel that opens
// under a Dropdown and stays open while ticks are changed. One control for the mixer's header and for Settings.
#pragma once

#include "Juce.h"
#include "app/AppController.h"
#include "ui/widgets/Dropdown.h"

#include <functional>
#include <vector>

namespace hr::playthrough
{
// One line of the list: a device (or "Automatic"), and what to say about it.
struct Entry
{
    juce::String key;               // "auto" or the device's node.name
    juce::String text;              // "Automatic (Elgato Wave:3 Headphones)", "HDMI Audio"
    bool missing = false;           // ticked in the model but not on the machine
};

// What the panel lists for a mix, in order: Automatic (Headphones only), the devices that are there, then the ticked ones that are not.
std::vector<Entry> entriesFor (AppController& controller, Mix mix);

// What the closed control says: the device ("Elgato Wave:3 Headphones"), or "Wave:3 Headphones + 1 more"; for the Stream mix
// "none" until a device is ticked. `extraDevices` is how many devices the mix plays to besides the first (Stream: besides
// "Headroom Stream Mix"). With `spellOutAutomatic` the one automatic device reads "Automatic (Elgato Wave:3 Headphones)", as Settings
// always said it.
juce::String summaryFor (AppController& controller, Mix mix, int* extraDevices = nullptr, bool spellOutAutomatic = false);

// Makes `box` the control of `mix`: its closed text follows refresh(), its click opens the panel under it.
void attach (Dropdown& box, AppController& controller, Mix mix, bool spellOutAutomatic = false);
void refresh (Dropdown& box, AppController& controller, Mix mix, bool spellOutAutomatic = false);

// The panel itself, in a layer over the window (the top-level component of `source`) that closes it on a click outside, on Esc and
// when the window is resized. Not a native pop-up: it is a child of the window, so that it behaves the same where the window
// system does not report the application as active (the screenshot run, some remote sessions) and is captured with the window.
class Panel;
Panel& open (Dropdown& source, AppController& controller, Mix mix);

// Closes the list that is open in the window of `anyComponentInIt` (at once; a click or Esc closes it after the event is handled).
void close (juce::Component& anyComponentInIt);
bool isOpen (juce::Component& anyComponentInIt);

class Panel final : public juce::Component,
                    private juce::Timer
{
public:
    Panel (AppController& controller, Mix mix);
    ~Panel() override;

    // Re-reads the model and the backend's lists; the rows are rebuilt only when what they say changed.
    void refresh();

    // What a click on a row does. False when the row does nothing (the last device of the Headphones mix stays ticked).
    bool toggle (const juce::String& key);

    int getNumRows() const noexcept                         { return (int) rows.size(); }
    bool isRowTicked (int index) const;
    juce::String getRowKey (int index) const;
    juce::String getRowText (int index) const;
    bool isRowMissing (int index) const;
    juce::Component* getRow (int index) const;

    int getIdealWidth() const noexcept                      { return idealWidth; }
    int getIdealHeight() const noexcept                     { return idealHeight; }

    void paint (juce::Graphics&) override;
    void resized() override;
    void parentHierarchyChanged() override;

private:
    class RowComponent;

    // Devices come and go while it is open; a panel that is no longer on screen (the window went away under it) leaves the controller alone.
    void timerCallback() override                           { if (isShowing()) refresh(); }
    void rebuild (std::vector<Entry> fresh);
    bool isTicked (const juce::String& key) const;
    bool isLocked (const juce::String& key) const;

    AppController& controller;
    const Mix mix;
    std::vector<Entry> entries;
    std::vector<std::unique_ptr<RowComponent>> rows;
    int idealWidth = 260, idealHeight = 80;
    bool focusedOnce = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Panel)
};
}
