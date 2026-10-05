// Icons: Lucide line icons (ISC) from the embedded SVGs, drawn tinted with a 1.5 px stroke at any size.
#pragma once

#include "Juce.h"

namespace hr
{
class Icons
{
public:
    // Draws the icon `name` (file name without ".svg", e.g. "chevron-down") into a `size` px square centred in `area`.
    static void draw (juce::Graphics& g, const juce::String& name, juce::Rectangle<float> area, float size, juce::Colour colour);
    static void draw (juce::Graphics& g, const juce::String& name, juce::Point<float> centre, float size, juce::Colour colour);

    // A drawable copy for places that need a juce::Drawable (popup menu item images). Owned by the caller.
    static std::unique_ptr<juce::Drawable> createDrawable (const juce::String& name, float size, juce::Colour colour);

    // The model stores channel icon names ("gamepad", "message"); this maps them to file names.
    static juce::String fileForChannelIcon (const juce::String& modelName);
    static const juce::StringArray& channelIconChoices();      // model names offered in the Icon menu
    static juce::String displayNameForChannelIcon (const juce::String& modelName);

    // Drop the parsed SVGs; call while JUCE is still alive (shutdown).
    static void releaseAll();
};
}
