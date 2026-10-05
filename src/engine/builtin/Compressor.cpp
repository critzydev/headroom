// Compressor: parameter layout, the Amount macro, the log-domain smooth-decoupled feed-forward compressor and its display tap.
#include "engine/builtin/Compressor.h"

#include "engine/RtUtils.h"

#include <cmath>

namespace hr
{
namespace
{
    constexpr float kFloorLevel = 1.0e-6f;                  // -120 dBFS: log10(0) protection
    constexpr float kDbToNeper = 0.115129255f;              // ln(10) / 20

    // The six detailed parameters, in the order `seen` keeps them.
    constexpr const char* kDetailedIds[6] { "threshold", "ratio", "attack", "release", "knee", "makeup" };

    // The Amount curve (see Compressor::settingsForAmount). Knee stays 6 dB.
    constexpr int kAmountPoints = 5;
    constexpr float kAmountAt[kAmountPoints]      { 0.0f, 25.0f, 50.0f, 75.0f, 100.0f };
    constexpr float kAmountThreshold[kAmountPoints] { 0.0f, -14.0f, -20.0f, -25.0f, -30.0f };
    constexpr float kAmountRatio[kAmountPoints]     { 1.0f, 2.0f, 3.0f, 4.5f, 6.0f };
    constexpr float kAmountMakeup[kAmountPoints]    { 0.0f, 1.5f, 4.0f, 6.5f, 9.0f };
    constexpr float kAmountKnee = 6.0f;

    float plainValue (const juce::RangedAudioParameter& p)
    {
        return p.convertFrom0to1 (p.getValue());
    }
}

Compressor::Compressor()
    : BuiltinProcessor (kId, "Compressor", createLayout())
{
    pThreshold = apvts.getRawParameterValue ("threshold");
    pRatio = apvts.getRawParameterValue ("ratio");
    pAttack = apvts.getRawParameterValue ("attack");
    pRelease = apvts.getRawParameterValue ("release");
    pKnee = apvts.getRawParameterValue ("knee");
    pMakeup = apvts.getRawParameterValue ("makeup");
    pAmount = apvts.getRawParameterValue ("amount");
    pCustom = apvts.getRawParameterValue ("amountCustom");

    rThreshold = apvts.getParameter ("threshold");
    rRatio = apvts.getParameter ("ratio");
    rAttack = apvts.getParameter ("attack");
    rRelease = apvts.getParameter ("release");
    rKnee = apvts.getParameter ("knee");
    rMakeup = apvts.getParameter ("makeup");
    rAmount = apvts.getParameter ("amount");
    rCustom = apvts.getParameter ("amountCustom");

    refreshSeen();

    for (const char* paramId : kDetailedIds)
        apvts.addParameterListener (paramId, this);

    apvts.addParameterListener ("amount", this);

    prepareToPlay (48000.0, 512);
}

Compressor::~Compressor()
{
    for (const char* paramId : kDetailedIds)
        apvts.removeParameterListener (paramId, this);

    apvts.removeParameterListener ("amount", this);
}

BuiltinProcessor::Layout Compressor::createLayout()
{
    Layout layout;

    // The first six are the presets' contract: ids and ranges never change. The Amount pair comes after them.
    layout.add (
        makeFloat ("threshold", "Threshold", -60.0f, 0.0f, -20.0f, "dB"),
        makeFloat ("ratio", "Ratio", 1.0f, 20.0f, 3.0f, ":1", 4.0f),
        makeFloat ("attack", "Attack", 0.1f, 100.0f, 10.0f, "ms", 10.0f),
        makeFloat ("release", "Release", 10.0f, 1000.0f, 160.0f, "ms", 150.0f),
        makeFloat ("knee", "Knee", 0.0f, 24.0f, 6.0f, "dB"),
        makeFloat ("makeup", "Makeup", -12.0f, 24.0f, 4.0f, "dB"),
        makeFloat ("amount", "Amount", 0.0f, 100.0f, kDefaultAmount, "%"),
        makeBool ("amountCustom", "Amount is custom", false));

    return layout;
}

// ---------------------------------------------------------------------------------------------------
// Amount macro
// ---------------------------------------------------------------------------------------------------

Compressor::AmountSettings Compressor::settingsForAmount (float percent) noexcept
{
    const float a = juce::jlimit (0.0f, 100.0f, percent);
    int i = 0;

    while (i < kAmountPoints - 2 && a > kAmountAt[i + 1])
        ++i;

    const float t = (a - kAmountAt[i]) / (kAmountAt[i + 1] - kAmountAt[i]);
    auto lerp = [t, i] (const float* table) { return table[i] + t * (table[i + 1] - table[i]); };

    return { lerp (kAmountThreshold), lerp (kAmountRatio), kAmountKnee, lerp (kAmountMakeup) };
}

void Compressor::setPlain (juce::RangedAudioParameter* p, float plain)
{
    p->setValueNotifyingHost (p->convertTo0to1 (plain));
}

void Compressor::setCustom (bool shouldBeCustom)
{
    rCustom->setValueNotifyingHost (shouldBeCustom ? 1.0f : 0.0f);
}

void Compressor::applyMacro (float percent)
{
    const juce::ScopedValueSetter<bool> guard (applying, true);
    const auto s = settingsForAmount (percent);

    setPlain (rThreshold, s.thresholdDb);
    setPlain (rRatio, s.ratio);
    setPlain (rKnee, s.kneeDb);
    setPlain (rMakeup, s.makeupDb);
    setCustom (false);
    refreshSeen();
}

void Compressor::applyAmount (float percent)
{
    percent = juce::jlimit (0.0f, 100.0f, percent);

    {
        const juce::ScopedValueSetter<bool> guard (applying, true);
        setPlain (rAmount, percent);
    }

    applyMacro (percent);
}

void Compressor::refreshSeen()
{
    seen = { pThreshold->load(), pRatio->load(), pAttack->load(), pRelease->load(), pKnee->load(), pMakeup->load() };
}

bool Compressor::detailedAtFactoryDefaults() const
{
    const juce::RangedAudioParameter* params[kNumDetailed] { rThreshold, rRatio, rAttack, rRelease, rKnee, rMakeup };

    for (int i = 0; i < kNumDetailed; ++i)
        if (std::abs (plainValue (*params[i]) - params[i]->convertFrom0to1 (params[i]->getDefaultValue())) > 0.05f)
            return false;

    return true;
}

void Compressor::parameterChanged (const juce::String& parameterId, float newValue)
{
    if (parameterId == "amount")
    {
        // Turning the Amount knob: the macro writes the detailed parameters (and clears Custom).
        if (! applying && ! loading)
            applyMacro (newValue);

        return;
    }

    for (int i = 0; i < kNumDetailed; ++i)
    {
        if (parameterId != kDetailedIds[i])
            continue;

        // The first notification of a parameter fires even if the value did not move (a preset re-applying a default).
        const bool moved = std::abs (seen[(size_t) i] - newValue) > 1.0e-4f;
        seen[(size_t) i] = newValue;

        if (moved && ! applying && ! loading)
            setCustom (true);

        return;
    }
}

void Compressor::setStateInformation (const void* data, int sizeInBytes)
{
    // A state saved before Amount existed has no `amount` entry: it counts as 50 %, or as Custom if anything differs
    // from the factory values. Newer states carry both Amount and the Custom flag and are restored exactly.
    bool valid = false, hadAmount = false;

    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        valid = xml->hasTagName (apvts.state.getType());
        hadAmount = valid && xml->getChildByAttribute ("id", "amount") != nullptr;
    }

    {
        const juce::ScopedValueSetter<bool> guard (loading, true);
        BuiltinProcessor::setStateInformation (data, sizeInBytes);
    }

    refreshSeen();

    if (valid && ! hadAmount)
    {
        const juce::ScopedValueSetter<bool> guard (loading, true);
        setPlain (rAmount, kDefaultAmount);
        setCustom (! detailedAtFactoryDefaults());
    }
}

// ---------------------------------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------------------------------

void Compressor::prepareToPlay (double newSampleRate, int)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    makeupDb.reset (sampleRate, 0.020);
    held = smoothed = 0.0f;
    lastGainDb = 0.0f;
    lastGain = 1.0f;
    needsSnap = true;
    meter.store (0.0f, std::memory_order_relaxed);

    tapPeriod = juce::jmax (1, juce::roundToInt (kDisplayRecordSeconds * sampleRate));
    tapCount = 0;
    tapIn = tapOut = tapGr = 0.0f;
    tapWasActive = false;
    detectorDb.store (kMinusInfDb, std::memory_order_relaxed);
    clearDisplay();
}

void Compressor::releaseResources()
{
}

template <bool Tap>
void Compressor::processSamples (float* l, float* r, int n, float threshold, float slope, float knee,
                                 float attackCoeff, float releaseCoeff, float quietLevel) noexcept
{
    float maxReduction = 0.0f;

    // Tap only: the window accumulators live in registers for the block and go back to the members at the end.
    float inPeak = tapIn, outPeak = tapOut, grPeak = tapGr, blockPeak = 0.0f;
    int count = tapCount;

    for (int i = 0; i < n; ++i)
    {
        float level = std::fabs (l[i]);

        if (r != nullptr)
            level = juce::jmax (level, std::fabs (r[i]));

        float wanted = 0.0f;                                     // reduction the static curve asks for, dB

        if (level > quietLevel)
        {
            const float xDb = 20.0f * std::log10 (juce::jmax (level, kFloorLevel));
            wanted = reductionFromOverDb (xDb - threshold, slope, knee);
        }

        // Smooth, decoupled peak detector: hold-and-release first, then attack smoothing.
        held = juce::jmax (wanted, releaseCoeff * held + (1.0f - releaseCoeff) * wanted);
        smoothed = attackCoeff * smoothed + (1.0f - attackCoeff) * held;

        if (held < 1.0e-7f)     held = 0.0f;
        if (smoothed < 1.0e-7f) smoothed = 0.0f;

        maxReduction = juce::jmax (maxReduction, smoothed);

        const float gainDb = makeupDb.getNextValue() - smoothed;

        if (gainDb != lastGainDb)
        {
            lastGainDb = gainDb;
            lastGain = std::exp (gainDb * kDbToNeper);
        }

        l[i] *= lastGain;

        if (r != nullptr)
            r[i] *= lastGain;

        if constexpr (Tap)
        {
            inPeak = juce::jmax (inPeak, level);
            outPeak = juce::jmax (outPeak, level * lastGain);
            grPeak = juce::jmax (grPeak, smoothed);
            blockPeak = juce::jmax (blockPeak, level);

            if (++count >= tapPeriod)
            {
                DisplayRecord record;
                record.inputPeakDb = linearToDb (inPeak);
                record.outputPeakDb = linearToDb (outPeak);
                record.gainReductionDb = grPeak > 0.0f ? -grPeak : 0.0f;
                pushRecord (record);

                count = 0;
                inPeak = outPeak = grPeak = 0.0f;
            }
        }
    }

    if constexpr (Tap)
    {
        tapIn = inPeak;
        tapOut = outPeak;
        tapGr = grPeak;
        tapCount = count;
        detectorDb.store (linearToDb (blockPeak), std::memory_order_relaxed);
    }

    meter.store (maxReduction > 0.0f ? -maxReduction : 0.0f, std::memory_order_relaxed);
}

void Compressor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int n = buffer.getNumSamples();
    const int numChannels = juce::jmin (buffer.getNumChannels(), 2);

    if (numChannels < 1 || n <= 0)
        return;

    float* l = buffer.getWritePointer (0);
    float* r = numChannels > 1 ? buffer.getWritePointer (1) : nullptr;

    const float threshold = pThreshold->load();
    const float ratio = juce::jmax (1.0f, pRatio->load());
    const float knee = pKnee->load();
    const float slope = 1.0f / ratio - 1.0f;                    // <= 0
    const float attackCoeff = (float) std::exp (-1.0 / (0.001 * (double) juce::jmax (0.01f, pAttack->load()) * sampleRate));
    const float releaseCoeff = (float) std::exp (-1.0 / (0.001 * (double) juce::jmax (1.0f, pRelease->load()) * sampleRate));

    if (needsSnap)
    {
        makeupDb.setCurrentAndTargetValue (pMakeup->load());
        needsSnap = false;
    }
    else
    {
        makeupDb.setTargetValue (pMakeup->load());
    }

    // Below this level the static curve gives no reduction, so the log10 can be skipped.
    const float quietLevel = std::pow (10.0f, (threshold - 0.5f * knee) * 0.05f);

    if (displayActive.load (std::memory_order_relaxed))
    {
        if (! tapWasActive)                                     // (re)started: no stale half window
        {
            tapWasActive = true;
            tapCount = 0;
            tapIn = tapOut = tapGr = 0.0f;
        }

        processSamples<true> (l, r, n, threshold, slope, knee, attackCoeff, releaseCoeff, quietLevel);
    }
    else
    {
        tapWasActive = false;
        processSamples<false> (l, r, n, threshold, slope, knee, attackCoeff, releaseCoeff, quietLevel);
    }
}

// ---------------------------------------------------------------------------------------------------
// Display ring
// ---------------------------------------------------------------------------------------------------

bool Compressor::pushRecord (const DisplayRecord& record) noexcept
{
    const auto head = ringHead.load (std::memory_order_relaxed);
    const auto tail = ringTail.load (std::memory_order_acquire);

    if (head - tail >= (uint64_t) kDisplayRingSize)             // full: drop, never wait for the reader
    {
        ringDropped.fetch_add (1, std::memory_order_relaxed);
        return false;
    }

    ring[(size_t) (head & (uint64_t) (kDisplayRingSize - 1))] = record;
    ringHead.store (head + 1, std::memory_order_release);
    return true;
}

int Compressor::drainDisplay (DisplayRecord* out, int maxRecords) noexcept
{
    const auto tail = ringTail.load (std::memory_order_relaxed);
    const auto head = ringHead.load (std::memory_order_acquire);
    const int count = (int) juce::jmin ((uint64_t) juce::jmax (0, maxRecords), head - tail);

    for (int i = 0; i < count; ++i)
        out[i] = ring[(size_t) ((tail + (uint64_t) i) & (uint64_t) (kDisplayRingSize - 1))];

    ringTail.store (tail + (uint64_t) count, std::memory_order_release);
    return count;
}

void Compressor::clearDisplay() noexcept
{
    ringTail.store (ringHead.load (std::memory_order_acquire), std::memory_order_release);
}
}
