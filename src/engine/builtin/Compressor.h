// Compressor built-in effect (id "comp"): feed-forward, stereo-linked, soft knee, log-domain smooth-decoupled detector.
#pragma once

#include "engine/builtin/BuiltinProcessor.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace hr
{
// Defaults match the first-run mic chain.
//
// DSP (after Giannoulis, Massberg and Reiss, "Digital Dynamic Range Compressor Design", 2012):
//  - level = max(|L|, |R|) per sample, in dBFS (sample peak, so a -10 dBFS sine reads -10 dB);
//  - static curve with a quadratic soft knee gives the wanted gain reduction in dB;
//  - the reduction is smoothed by a peak-holding release stage (time constant = Release) followed by an attack
//    stage (time constant = Attack), so a steady tone settles at exactly the static-curve reduction and the gain
//    does not ripple at audio rate;
//  - makeup is applied through a 20 ms dB ramp.
// getMeterValue() is the largest gain reduction of the last block in dB (<= 0), excluding makeup.
//
// Two things exist for the panel (src/ui/fx/CompPanel):
//  - the "Amount" macro: parameter `amount` (0-100 %) sets threshold, ratio, knee and makeup together along a voice
//    curve (settingsForAmount). Editing any of the six detailed parameters afterwards sets the `amountCustom` flag
//    (shown as "Custom"), moving Amount clears it again. Both are ordinary parameters, so they live in the state;
//  - a display tap: while `displayActive` is set the audio thread writes one DisplayRecord per ~5 ms into a
//    preallocated lock-free ring (dropping when it is full) and keeps the current detector level. While it is clear
//    the audio thread does none of that work.
class Compressor final : public BuiltinProcessor,
                         private juce::AudioProcessorValueTreeState::Listener
{
public:
    static constexpr const char* kId = "comp";

    Compressor();
    ~Compressor() override;

    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    float getMeterValue() const noexcept override   { return meter.load (std::memory_order_relaxed); }

    // ---- Static gain computer: the DSP and the panel's transfer curve both use exactly this ---------------------
    // Gain reduction (dB, >= 0, makeup not included) the static curve asks for at `overDb` = input level - threshold.
    // `slope` = 1 / ratio - 1 (<= 0). Quadratic soft knee of width `kneeDb` centred on the threshold.
    static float reductionFromOverDb (float overDb, float slope, float kneeDb) noexcept
    {
        if (kneeDb < 0.01f)
            return overDb > 0.0f ? -slope * overDb : 0.0f;

        if (2.0f * overDb > kneeDb)
            return -slope * overDb;

        if (2.0f * overDb >= -kneeDb)
        {
            const float t = overDb + 0.5f * kneeDb;
            return -slope * t * t / (2.0f * kneeDb);
        }

        return 0.0f;
    }

    static float reductionDb (float inputDb, float thresholdDb, float ratio, float kneeDb) noexcept
    {
        const float slope = 1.0f / (ratio > 1.0f ? ratio : 1.0f) - 1.0f;
        return reductionFromOverDb (inputDb - thresholdDb, slope, kneeDb);
    }

    // Steady-state output level (dB) for an input level, makeup included.
    static float outputDb (float inputDb, float thresholdDb, float ratio, float kneeDb, float makeupDb) noexcept
    {
        return inputDb - reductionDb (inputDb, thresholdDb, ratio, kneeDb) + makeupDb;
    }

    // ---- The Amount macro ---------------------------------------------------------------------------------------
    struct AmountSettings
    {
        float thresholdDb, ratio, kneeDb, makeupDb;
    };

    static constexpr float kDefaultAmount = 50.0f;

    // 0 % = transparent (0 dB, 1:1, no makeup), 50 % = the factory default (-20 dB, 3:1, +4 dB), 100 % = -30 dB, 6:1,
    // +9 dB; knee 6 dB throughout. Piecewise linear between 0, 25, 50, 75 and 100 %.
    static AmountSettings settingsForAmount (float percent) noexcept;

    // Sets `amount`, the four detailed parameters it drives, and clears the Custom flag. Message thread.
    void applyAmount (float percent);
    float getAmount() const noexcept                { return pAmount->load (std::memory_order_relaxed); }
    bool isAmountCustom() const noexcept            { return pCustom->load (std::memory_order_relaxed) >= 0.5f; }

    // ---- Display tap ---------------------------------------------------------------------------------------------
    struct DisplayRecord
    {
        float inputPeakDb = -100.0f;        // largest detector level in the ~5 ms window (-100 = silence)
        float outputPeakDb = -100.0f;       // the same after gain reduction and makeup
        float gainReductionDb = 0.0f;       // largest gain reduction in the window (<= 0, makeup not included)
    };

    static constexpr int kDisplayRingSize = 1024;               // records (about 5 s), a power of two
    static constexpr double kDisplayRecordSeconds = 0.005;

    // Set by the UI while the panel is on screen. Clear = the audio thread does no extra work at all.
    std::atomic<bool> displayActive { false };

    // Consumer side (message thread): moves up to `maxRecords` records, oldest first, into `out`.
    int drainDisplay (DisplayRecord* out, int maxRecords) noexcept;
    void clearDisplay() noexcept;                               // throws away whatever is queued

    // Detector level (dB) of the most recent block; only updated while displayActive.
    float getDetectorLevelDb() const noexcept       { return detectorDb.load (std::memory_order_relaxed); }

    // Test hooks: records written / dropped since construction.
    uint64_t getDisplayRecordsWritten() const noexcept   { return ringHead.load (std::memory_order_relaxed); }
    uint64_t getDisplayRecordsDropped() const noexcept   { return ringDropped.load (std::memory_order_relaxed); }

private:
    static Layout createLayout();
    static constexpr int kNumDetailed = 6;

    // AudioProcessorValueTreeState::Listener
    void parameterChanged (const juce::String& parameterId, float newValue) override;

    template <bool Tap>
    void processSamples (float* l, float* r, int n, float threshold, float slope, float knee,
                         float attackCoeff, float releaseCoeff, float quietLevel) noexcept;

    void applyMacro (float percent);
    void setPlain (juce::RangedAudioParameter* p, float plainValue);
    void setCustom (bool shouldBeCustom);
    void refreshSeen();
    bool detailedAtFactoryDefaults() const;
    bool pushRecord (const DisplayRecord& record) noexcept;

    std::atomic<float> *pThreshold, *pRatio, *pAttack, *pRelease, *pKnee, *pMakeup, *pAmount, *pCustom;
    juce::RangedAudioParameter *rThreshold, *rRatio, *rAttack, *rRelease, *rKnee, *rMakeup, *rAmount, *rCustom;

    // Macro bookkeeping (message thread): who is writing the detailed parameters right now, and the last value seen
    // for each, so a parameter that is "set" to the value it already has (a preset) does not count as an edit.
    bool applying = false, loading = false;
    std::array<float, kNumDetailed> seen {};

    double sampleRate = 48000.0;
    float held = 0.0f;              // peak-held gain reduction (dB, >= 0)
    float smoothed = 0.0f;          // attack-smoothed gain reduction (dB, >= 0)
    float lastGainDb = 0.0f;        // cache: skip the exp() while the gain does not move
    float lastGain = 1.0f;
    bool needsSnap = true;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> makeupDb { 0.0f };
    std::atomic<float> meter { 0.0f };

    // Tap: window accumulators (audio thread only), the ring (single producer = audio thread, single consumer = UI).
    int tapPeriod = 240, tapCount = 0;
    float tapIn = 0.0f, tapOut = 0.0f, tapGr = 0.0f;
    bool tapWasActive = false;
    std::atomic<float> detectorDb { -100.0f };
    std::array<DisplayRecord, kDisplayRingSize> ring {};
    std::atomic<uint64_t> ringHead { 0 }, ringTail { 0 }, ringDropped { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Compressor)
};
}
