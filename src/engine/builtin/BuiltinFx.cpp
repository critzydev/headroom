// Built-in effect registry.
#include "engine/builtin/BuiltinFx.h"

#include "engine/builtin/Compressor.h"
#include "engine/builtin/DeEsser.h"
#include "engine/builtin/Equalizer.h"
#include "engine/builtin/Gate.h"
#include "engine/builtin/Limiter.h"
#include "engine/builtin/NoiseRemoval.h"

namespace hr::BuiltinFx
{
const std::vector<Info>& list()
{
    static const std::vector<Info> effects
    {
        { NoiseRemoval::kId, "Noise Removal", "Removes background noise like fans and keyboard clicks from your voice." },
        { Equalizer::kId,    "EQ",            "Shape the tone: cut rumble, add clarity." },
        { Compressor::kId,   "Compressor",    "Evens out loud and quiet parts." },
        { Gate::kId,         "Noise Gate",    "Goes quiet when nothing is being said or played." },
        { DeEsser::kId,      "De-esser",      "Softens harsh S sounds." },
        { Limiter::kId,      "Limiter",       "Keeps the sound from getting louder than a set ceiling." },
    };

    return effects;
}

const Info* find (const juce::String& builtinId)
{
    for (const auto& info : list())
        if (info.id == builtinId)
            return &info;

    return nullptr;
}

juce::String displayNameFor (const juce::String& builtinId)
{
    if (auto* info = find (builtinId))
        return info->displayName;

    return builtinId;
}

std::unique_ptr<BuiltinProcessor> create (const juce::String& builtinId)
{
    if (builtinId == NoiseRemoval::kId)  return std::make_unique<NoiseRemoval>();
    if (builtinId == Equalizer::kId)     return std::make_unique<Equalizer>();
    if (builtinId == Compressor::kId)    return std::make_unique<Compressor>();
    if (builtinId == Gate::kId)          return std::make_unique<Gate>();
    if (builtinId == DeEsser::kId)       return std::make_unique<DeEsser>();
    if (builtinId == Limiter::kId)       return std::make_unique<Limiter>();

    return nullptr;
}
}
