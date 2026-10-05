// Registry of the built-in effects: list() for the "Add effect" menu, create() to instantiate one by id.
#pragma once

#include "Juce.h"
#include "engine/builtin/BuiltinProcessor.h"

#include <memory>
#include <vector>

namespace hr::BuiltinFx
{
struct Info
{
    juce::String id;            // stable, stored in the model: noise eq comp gate deess limit
    juce::String displayName;   // "Noise Removal"
    juce::String description;   // one plain sentence for the menu
};

// In the order the UI should show them.
const std::vector<Info>& list();

// nullptr if the id is unknown.
const Info* find (const juce::String& builtinId);

// Display name for a builtin id, or the id itself if unknown (used when seeding SLOT names).
juce::String displayNameFor (const juce::String& builtinId);

// nullptr if the id is unknown. Message thread.
std::unique_ptr<BuiltinProcessor> create (const juce::String& builtinId);
}
