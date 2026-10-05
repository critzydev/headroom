// Wave:3 gain maths, no dependencies: the pactl-style cubic slider <-> dB mapping and the "Safe gain" plan.
#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

namespace hr::wave
{
constexpr double kHardwareMaxDb = 40.0;         // the Wave:3's "Mic Capture Volume" runs 0 ... +40 dB
constexpr double kSafeHardwareDb = 24.0;        // what Safe gain leaves the mic itself at
constexpr double kMaxMakeupDb = 24.0;           // Headroom never adds more than this back
constexpr double kGainToleranceDb = 0.75;       // more than one hardware step (0.25 / 0.5 dB): "it is not at the target"

// ---- The cubic volume helper --------------------------------------------------------------------------
// PipeWire keeps a device volume as a linear amplitude and pactl shows its cube root as a percentage. The mic's
// ALSA range is 0 ... +40 dB, and ALSA's "base volume" rule puts the top of that range at 100 % (0 dB), so the
// hardware step at `db` is
//     linear = 10 ^ ((db - 40) / 20)        slider = cbrt (linear) = 10 ^ ((db - 40) / 60)
// +40 dB is 1.0 (100 %), +24 dB is 0.5412 (54 %), and the bottom of the range, 0 dB, is 0.2154. The mic slider
// takes `slider`; DeviceControl::sliderToVolume / volumeToSlider are these two functions.
inline float sliderToVolume (float slider) noexcept
{
    const float s = std::clamp (slider, 0.0f, 1.0f);
    return s * s * s;
}

inline float volumeToSlider (float volume) noexcept
{
    return std::cbrt (std::clamp (volume, 0.0f, 1.0f));
}

inline double hardwareDbToSlider (double db) noexcept
{
    return std::pow (10.0, (std::clamp (db, 0.0, kHardwareMaxDb) - kHardwareMaxDb) / 60.0);
}

inline double sliderToHardwareDb (double slider) noexcept
{
    if (slider <= 0.0)
        return 0.0;

    return std::clamp (kHardwareMaxDb + 60.0 * std::log10 (std::min (slider, 1.0)), 0.0, kHardwareMaxDb);
}

// The same slider for a model whose gain range tops out elsewhere (the Wave XLR's 0 ... 75 dB over USB): its top
// is 1.0, and each 60 dB below is a factor of 10, as above. Only a position on the slider: there is no ALSA base volume behind
// it, the model has no ALSA gain control.
inline double hardwareDbToSlider (double db, double maxDb) noexcept
{
    return std::pow (10.0, (std::clamp (db, 0.0, maxDb) - maxDb) / 60.0);
}

inline double sliderToHardwareDb (double slider, double maxDb) noexcept
{
    if (slider <= 0.0)
        return 0.0;

    return std::clamp (maxDb + 60.0 * std::log10 (std::min (slider, 1.0)), 0.0, maxDb);
}

// ---- Safe gain ---------------------------------------------------------------------------------------------
// The mic keeps a lower hardware gain (so loud moments do not clip its converter) and Headroom adds the difference
// back in the mic channel, so the level in Discord stays what it was.
//
//   baselineDb   the hardware gain before Safe gain was switched on (what the level is kept at)
//   measuredDb   the hardware gain now, if known
//
// The makeup is baseline - max (measured, target): it never adds more than the mic itself gave up. If the mic did not
// take the lower gain (it is still at 40 dB), the makeup is 0 instead of piling +16 dB on top of it; if someone
// turns the hardware dial up, the makeup shrinks so the level stays the same; and if the dial is turned down below
// the target, the makeup stays where it was, so turning it down still makes things quieter.
struct SafeGainPlan
{
    double targetDb = kSafeHardwareDb;     // where the mic's own gain should be
    double makeupDb = 0.0;                 // what Headroom adds (mic channel input trim)
    bool hardwareTooHigh = false;          // measured, and above the target: the mic has to be told again
};

inline SafeGainPlan planSafeGain (double baselineDb, std::optional<double> measuredDb)
{
    SafeGainPlan plan;
    const double baseline = std::clamp (baselineDb, 0.0, kHardwareMaxDb);
    plan.targetDb = std::min (kSafeHardwareDb, baseline);

    if (! measuredDb.has_value())
        return plan;            // nothing is known about the mic yet: add nothing

    const double measured = std::clamp (*measuredDb, 0.0, kHardwareMaxDb);
    plan.hardwareTooHigh = measured > plan.targetDb + kGainToleranceDb;

    const double makeup = std::clamp (baseline - std::max (measured, plan.targetDb), 0.0, kMaxMakeupDb);
    plan.makeupDb = std::round (makeup * 10.0) / 10.0;      // tenths of a dB: no churn, tidy numbers in the UI
    return plan;
}
}
