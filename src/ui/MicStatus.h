// MicStatus: turns BackendStatus into the words and colours the pill, banner and Settings share.
#pragma once

#include "Juce.h"
#include "backend/BackendTypes.h"
#include "ui/Theme.h"

namespace hr
{
struct MicStatusView
{
    juce::Colour dot = theme::text2;
    juce::String pill;          // "Wave:3 . live"
    juce::String sentence;      // one plain sentence for Settings and tooltips
};

// "Elgato Wave:3" -> "Wave:3": the brand is noise in a 20 px pill.
inline juce::String shortDeviceName (const juce::String& label)
{
    return label.startsWithIgnoreCase ("Elgato ") ? label.substring (7) : label;
}

// How a sentence names the mic: "Wave XLR" for an Elgato model the backend knows, the device's own name for another mic, "mic" when
// there is none. The backend says the model-specific things itself (BackendStatus::message); these are for when it has not.
inline juce::String micNoun (const BackendStatus& status)
{
    const auto name = shortDeviceName (status.inputLabel);
    return name.isNotEmpty() ? name : juce::String ("mic");
}

inline juce::String noMicSentence()
{
    return "No microphone found. Plug one in and Headroom will pick it up.";
}

inline juce::String silentMicSentence (const BackendStatus& status)
{
    return "No sound from your " + micNoun (status) + ". Check that it is not muted on the mic itself.";
}

inline juce::String mutedMicSentence (const BackendStatus& status)
{
    return "The " + micNoun (status) + " is muted. Unmute it on the mic itself.";
}

inline MicStatusView describeMic (const BackendStatus& status)
{
    MicStatusView v;
    auto shortName = shortDeviceName (status.inputLabel);

    if (shortName.length() > 20)
        shortName = shortName.substring (0, 19).trimEnd() + theme::glyph::ellipsis;

    if (! status.connected)
    {
        v.pill = "Not connected";
        v.sentence = "Headroom is not connected to the audio system.";
        return v;
    }

    switch (status.mic)
    {
        case MicState::NotFound:
            v.pill = "No mic";
            v.sentence = noMicSentence();
            break;

        case MicState::Starting:
            v.pill = "Starting" + theme::glyph::ellipsis;
            v.sentence = "Getting your mic ready.";
            break;

        case MicState::Ok:
            v.dot = theme::ok;
            v.pill = (shortName.isNotEmpty() ? shortName : juce::String ("Mic")) + " " + theme::glyph::dot + " live";
            v.sentence = "Working. Headroom hears " + (status.inputLabel.isNotEmpty() ? status.inputLabel : juce::String ("your mic")) + ".";
            break;

        case MicState::Recovering:
            v.dot = theme::warn;
            v.pill = "Fixing mic" + theme::glyph::ellipsis;
            v.sentence = "Getting your mic to start. This takes a few seconds.";
            break;

        case MicState::SilentFromStart:
            v.dot = theme::bad;
            v.pill = "No sound";
            // The backend knows which mic this is and says it in its own words; these are for when it has none.
            v.sentence = status.message.isNotEmpty() ? status.message : silentMicSentence (status);
            break;

        case MicState::MutedOnDevice:
            v.dot = theme::bad;
            v.pill = "Mic muted";
            v.sentence = status.message.isNotEmpty() ? status.message : mutedMicSentence (status);
            break;
    }

    return v;
}
}
