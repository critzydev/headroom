// Mic preset data. Values are starting points for a dynamic USB mic-style voice chain at arm's length.
#include "engine/builtin/Presets.h"

namespace hr
{
namespace
{
    // EQ band values (see Equalizer): the type and slope are choice indices. The two-stage 24 dB/oct high-pass these
    // presets always had is the Linkwitz-Riley slope, so the presets sound exactly as before the EQ got eight bands.
    constexpr float kPeak = 0.0f, kLowShelf = 1.0f, kHighShelf = 2.0f, kLowCut = 3.0f;
    constexpr float kSlope12 = 1.0f, kSlope24LinkwitzRiley = 4.0f;
    constexpr float kFlatShelfQ = 0.70710678f;

    // Noise Removal's "mode" is a choice index (Fast 0, Best 1, Light 2). Every preset with noise removal sets it on
    // purpose instead of leaning on the parameter's default; a test keeps this equal to NoiseRemoval::Mode::best.
    constexpr float kNoiseModeBest = 1.0f;
}

const std::vector<Preset>& micPresets()
{
    static const std::vector<Preset> presets {
        { "clean", "Clean voice", "Noise removal, rumble filter, gentle compression",
          {
              { "noise", { { "mode", kNoiseModeBest }, { "strength", 80.0f }, { "threshold", 60.0f }, { "hold", 150.0f } } },
              { "eq",    { { "b1On", 1.0f }, { "b1Type", kLowCut }, { "b1Freq", 80.0f }, { "b1Slope", kSlope12 } } },
              { "comp",  { { "threshold", -20.0f }, { "ratio", 3.0f }, { "attack", 10.0f }, { "release", 160.0f },
                           { "knee", 6.0f }, { "makeup", 4.0f } } },
              { "limit", { { "ceiling", -1.0f }, { "release", 50.0f } } },
          } },

        // Mud cut + presence lift + tighter compression and a de-esser, like a radio chain.
        { "broadcast", "Broadcast", "Fuller, more present voice with de-essing",
          {
              { "noise", { { "mode", kNoiseModeBest }, { "strength", 100.0f }, { "threshold", 50.0f }, { "hold", 150.0f } } },
              { "eq",    { { "b1On", 1.0f }, { "b1Type", kLowCut }, { "b1Freq", 90.0f }, { "b1Slope", kSlope24LinkwitzRiley },
                           { "b2On", 1.0f }, { "b2Type", kLowShelf }, { "b2Freq", 150.0f }, { "b2Gain", 2.0f }, { "b2Q", kFlatShelfQ },
                           { "b3On", 1.0f }, { "b3Type", kPeak }, { "b3Freq", 300.0f }, { "b3Gain", -2.5f }, { "b3Q", 1.2f },
                           { "b5On", 1.0f }, { "b5Type", kPeak }, { "b5Freq", 4000.0f }, { "b5Gain", 2.5f }, { "b5Q", 0.9f },
                           { "b7On", 1.0f }, { "b7Type", kHighShelf }, { "b7Freq", 10000.0f }, { "b7Gain", 2.0f }, { "b7Q", kFlatShelfQ } } },
              { "comp",  { { "threshold", -24.0f }, { "ratio", 4.0f }, { "attack", 5.0f }, { "release", 150.0f },
                           { "knee", 6.0f }, { "makeup", 6.0f } } },
              { "deess", { { "frequency", 6500.0f }, { "threshold", -30.0f }, { "amount", 50.0f } } },
              { "limit", { { "ceiling", -1.0f }, { "release", 50.0f } } },
          } },

        // Stronger noise removal, a soft expander between words (turns the gaps down, doesn't chop them off) and a
        // higher rumble filter for fans and keyboards.
        { "noisy", "Noisy room", "Stronger noise removal and quieter gaps between words",
          {
              { "noise", { { "mode", kNoiseModeBest }, { "strength", 100.0f }, { "threshold", 85.0f }, { "hold", 200.0f } } },
              { "gate",  { { "threshold", -50.0f }, { "range", 12.0f }, { "attack", 5.0f }, { "hold", 200.0f },
                           { "release", 250.0f } } },
              { "eq",    { { "b1On", 1.0f }, { "b1Type", kLowCut }, { "b1Freq", 120.0f }, { "b1Slope", kSlope24LinkwitzRiley },
                           { "b5On", 1.0f }, { "b5Type", kPeak }, { "b5Freq", 3000.0f }, { "b5Gain", 1.5f }, { "b5Q", 1.0f } } },
              { "comp",  { { "threshold", -20.0f }, { "ratio", 3.0f }, { "attack", 10.0f }, { "release", 160.0f },
                           { "knee", 6.0f }, { "makeup", 4.0f } } },
              { "limit", { { "ceiling", -1.0f }, { "release", 50.0f } } },
          } },

        { "off", "Off", "No effects on the mic", {} },
    };

    return presets;
}

const Preset* findMicPreset (const juce::String& presetId)
{
    for (auto& preset : micPresets())
        if (preset.id == presetId)
            return &preset;

    return nullptr;
}
}
