// One mixer channel on the RT thread: input trim -> FX chain -> per-mix smoothed gain/mute + meters.
#pragma once

#include "Juce.h"
#include "engine/EngineTypes.h"
#include "engine/FxChain.h"
#include "engine/Meters.h"
#include "engine/RtUtils.h"

#include <array>
#include <atomic>

namespace hr
{
class ChannelProcessor
{
public:
    ChannelProcessor() = default;

    // ---- Message thread -------------------------------------------------------------------------
    // Re-arms the smoothers for a sample rate. Only while the RT thread is suspended or stopped.
    void prepare (double sampleRate);

    void setActive (bool shouldBeActive) noexcept;
    bool isActive() const noexcept                          { return active.load (std::memory_order_relaxed); }
    void setGain (Mix mix, float linear) noexcept           { gain[(int) mix].store (linear, std::memory_order_relaxed); }
    void setMuted (Mix mix, bool shouldMute) noexcept       { muted[(int) mix].store (shouldMute, std::memory_order_relaxed); }
    void setInputTrim (float linear) noexcept               { inputTrim.store (linear, std::memory_order_relaxed); }

    // Publishes a new chain (nullptr = none) and returns the previous raw pointer. Ownership stays with
    // AudioEngine, which keeps the snapshot alive until the RT thread has moved on.
    FxChain* exchangeChain (FxChain* newChain) noexcept     { return chain.exchange (newChain); }

    // ---- RT thread ------------------------------------------------------------------------------
    // Trim + FX into the internal buffer. inL/inR may be nullptr (silence). Returns false if the channel
    // has nothing to contribute this chunk (inactive, or unlinked with no effects); then skip addToMix().
    bool process (const float* inL, const float* inR, int n) noexcept;

    // Adds the processed signal, scaled by the smoothed fader/mute of `mix`, into dst and meters the result.
    void addToMix (Mix mix, float* dstL, float* dstR, int n) noexcept;

    // Post-FX, pre-fader signal of the last process() call.
    const float* getOutputL() const noexcept                { return work[0].data(); }
    const float* getOutputR() const noexcept                { return work[1].data(); }

    ChannelMeters meters;                                   // read by the UI via AudioEngine::readChannelLevels

private:
    std::atomic<bool> active { false };
    std::atomic<bool> snapPending { true };                 // first block after (re)activation: no ramp from stale gain
    std::atomic<float> gain[kNumMixes] { 1.0f, 1.0f };      // linear
    std::atomic<bool> muted[kNumMixes] { false, false };
    std::atomic<float> inputTrim { 1.0f };
    std::atomic<FxChain*> chain { nullptr };
    static_assert (std::atomic<FxChain*>::is_always_lock_free);

    SmoothedGain mixGain[kNumMixes];
    SmoothedGain trim;

    std::array<float, kMaxBlock> work[2] {};
    std::array<float, kMaxBlock> ramp {};
};
}
