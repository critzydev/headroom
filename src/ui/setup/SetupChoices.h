// SetupChoices: what the setup wizard collects, where it starts from, and how it goes into the model.
// The wizard changes the mixer through AppController and AppState exactly like the Settings page does; nothing here is its own
// copy of the settings. Everything except the two devices (which are live, so that the lists and the Wave:3 step follow what the
// person picks) is applied once, when they finish.
#pragma once

#include "Juce.h"
#include "app/AppController.h"

#include <vector>

namespace hr
{
struct SetupChoices
{
    struct Channel
    {
        juce::String id;                // the existing channel's id; empty for one the wizard adds
        juce::String name;
        juce::String colour;            // RRGGBB, what the row's dot shows
        bool keep = true;
        bool isNew = false;
    };

    // Devices
    juce::String inputDevice = "auto", outputDevice = "auto";

    // Channels (the mic is always there and is not listed)
    std::vector<Channel> channels;

    // Voice: a preset id from engine/builtin/Presets.h ("clean", "broadcast", "noisy", "off"), or "custom" = leave the chain alone.
    // The noise removal mode is the effect's own "mode" parameter (Fast 0, Best 1, Light 2).
    juce::String preset = "clean";
    int noiseMode = 1;
    bool noiseModeChosen = false;       // the person picked a mode themselves: a preset no longer decides it

    // System
    bool startAtLogin = true, makeDefault = true, keepDefault = true;
    bool keepInPlace = false;           // AppImage: run it from where it is (a manager looks after it) instead of copying it

    // Wave:3 hardware controls (only used when the permission is there)
    bool clipGuard = true, safeGain = false;

    static constexpr int kMaxChannels = 8;

    // What the model says now: the person's current settings on a re-run, the shipped defaults on the first one.
    static SetupChoices startingPoint (AppController& controller);

    // Puts everything except the devices into the model. Call once.
    void apply (AppController& controller) const;

    // The Noise Removal effect on the mic (the "mode" parameter), if the mic chain has one.
    static int noiseModeOf (AppController& controller, int fallback);
    static int defaultNoiseModeFor (const juce::String& presetId);
    static bool setNoiseMode (AppController& controller, int mode);
};
}
