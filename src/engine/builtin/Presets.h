// Mic presets for the FX drawer's "Preset" menu: ordered built-in effects with parameter values.
#pragma once

#include "Juce.h"

#include <utility>
#include <vector>

namespace hr
{
struct PresetSlot
{
    juce::String builtinId;                                         // BuiltinFx registry id
    std::vector<std::pair<juce::String, float>> params;             // paramId -> plain (not normalised) value
};

struct Preset
{
    juce::String id;                                                // stable: "clean", "broadcast", "noisy", "off"
    juce::String displayName;
    juce::String description;                                       // one short line for the menu
    std::vector<PresetSlot> slots;
};

const std::vector<Preset>& micPresets();
const Preset* findMicPreset (const juce::String& presetId);
}
