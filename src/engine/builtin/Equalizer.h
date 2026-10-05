// EQ built-in effect (id "eq"): eight generic bands (peak, shelves, cuts, notch, band pass, tilt), an analyzer feed and band solo.
#pragma once

#include "engine/builtin/BuiltinProcessor.h"
#include "engine/dsp/AnalyzerFifo.h"
#include "engine/dsp/Biquad.h"
#include "engine/dsp/EqDesign.h"

#include <array>
#include <atomic>

namespace hr
{
// Eight identical bands, each with: on, type, frequency (20 Hz - 20 kHz), gain (+-24 dB), Q (0.1 - 18) and, for the
// cuts, a slope (6 / 12 / 24 / 48 dB/oct). Parameter ids are "b1On", "b1Type", "b1Freq", "b1Gain", "b1Q", "b1Slope"
// ... "b8Slope". Defaults match the first-run mic chain: band 1 is a 12 dB/oct low cut at 80 Hz and on,
// the other seven are off but already sit at 100, 250, 1k, 3k, 6k, 10k and 16 kHz so they are one click from useful.
// The model stores these slots with empty state, so the defaults here ARE the preset.
//
// DSP (dsp/EqDesign): matched-magnitude biquads, state in double. A band's controls glide (frequency and Q
// exponentially, gain linearly, ~12 ms) and its coefficients are recomputed every 32 samples while, and only while,
// something moves. Switching a band on or off, or changing its type or cut slope, crossfades over 10 ms between two
// voices, so nothing clicks. Both channels run linked; when a block is bit-identical on both (the mono mic) only one
// runs and the result is copied. Bands that are off, and gain bands sitting at 0 dB, cost nothing.
//
// Analyzer: while `analyzerActive` is set (the UI does that while an EQ view is on screen) the RT thread pushes the
// pre- and post-EQ mono sum into a lock-free FIFO. While it is clear the audio path does nothing extra.
//
// Solo: setSoloBand (n) plays only what band n acts on (a band pass around a bell or notch, a low-pass for a low
// shelf or low cut, a high-pass for a high shelf or high cut) instead of the EQ output, until setSoloBand (-1).
class Equalizer final : public BuiltinProcessor
{
public:
    static constexpr const char* kId = "eq";
    static constexpr int kNumBands = 8;
    static constexpr int kNumTypes = dsp::eq::kNumTypes;
    static constexpr int kNumSlopes = dsp::eq::kNumSlopes;

    static constexpr float kMinFreq = 20.0f, kMaxFreq = 20000.0f;
    static constexpr float kMaxGainDb = 24.0f;
    static constexpr float kMinQ = 0.1f, kMaxQ = 18.0f;

    using Type = dsp::eq::Type;

    struct BandSettings
    {
        bool on = false;
        Type type = Type::peak;
        float freq = 1000.0f, gainDb = 0.0f, q = 1.0f;
        int slope = dsp::eq::kSlope12;
    };

    Equalizer();
    ~Equalizer() override = default;

    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    // ---- Parameters -------------------------------------------------------------------------------
    static juce::String bandParamId (int band, const char* what);   // band 0-based: bandParamId (0, "Freq") = "b1Freq"
    static const char* typeName (Type type) noexcept;                // "Peak", "Low shelf", ...
    static const char* slopeName (int slope) noexcept;               // "12 dB/oct"
    static bool hasGain (Type type) noexcept;                        // peak, shelves and tilt
    static bool isCut (Type type) noexcept                           { return type == Type::lowCut || type == Type::highCut; }
    static BandSettings getDefaultBand (int band) noexcept;

    BandSettings getBandSettings (int band) const noexcept;         // the live parameter values

    // ---- Old states --------------------------------------------------------------------------------
    // Versions up to 0.1.1 had a fixed layout (hpf*, lowShelf*, bell1-3*, highShelf*, lpf*). setStateInformation
    // converts such a state into bands (high-pass -> band 1 low cut, low shelf -> 2, bells -> 3-5, high shelf -> 6,
    // low-pass -> 7 high cut) with the same sound; the old two-stage 24 dB/oct high-pass keeps its shape as the
    // "24 dB/oct (LR)" slope. Returns true if `state` was in the old layout (and has been rewritten).
    static bool migrateLegacyState (juce::ValueTree& state);
    void setStateInformation (const void* data, int sizeInBytes) override;

    // ---- Response, for the panel's curves ------------------------------------------------------------------------
    // Magnitude in dB at `freq`. Built by dsp::eq::design from the current parameter values, the very function the
    // DSP uses. sampleRate 0 = the rate this processor is prepared for. A band that is off contributes 0 dB.
    float getResponseDb (double freq, double sampleRate = 0.0) const;
    float getBandResponseDb (int band, double freq, double sampleRate = 0.0) const;
    void getBandResponseDb (int band, const float* freqs, float* outDb, int numPoints, double sampleRate = 0.0) const;
    double getPreparedRate() const noexcept                         { return preparedRate.load (std::memory_order_relaxed); }

    // ---- Analyzer and solo ---------------------------------------------------------------------------
    std::atomic<bool> analyzerActive { false };
    dsp::AnalyzerFifo& getAnalyzerFifo() noexcept                   { return analyzerFifo; }

    void setSoloBand (int band) noexcept                            { soloBand.store (band, std::memory_order_relaxed); }
    int getSoloBand() const noexcept                                { return soloBand.load (std::memory_order_relaxed); }

    static dsp::eq::Sections designFor (const BandSettings& settings, double sampleRate) noexcept;

private:
    static Layout createLayout();

    static constexpr int kUpdateInterval = 32;      // samples between coefficient updates while something glides
    static constexpr int kChunk = 1024;             // analyzer scratch size; longer blocks are processed in pieces

    // One filter with its own state. A band has two so that a type change can crossfade from one to the other.
    struct Voice
    {
        dsp::eq::Sections coeffs;
        dsp::BiquadState state[dsp::eq::kMaxSections][2];
        Type type = Type::peak;
        int slope = dsp::eq::kSlope12;
        float freq = 1000.0f, gainDb = 0.0f, q = 1.0f;     // the smoothed values the coefficients were built from
        float mix = 0.0f, mixTarget = 0.0f;                // crossfade: 0 = out of the path, 1 = fully in

        bool idle() const noexcept      { return mix <= 0.0f && mixTarget <= 0.0f; }
        void resetState() noexcept;
        void load (const BandSettings& target, double sampleRate) noexcept;
    };

    struct Band
    {
        std::array<Voice, 2> voice;
        int active = 0;
    };

    struct Raw
    {
        std::atomic<float> *on, *type, *freq, *gain, *q, *slope;
    };

    void updateBands (bool snap) noexcept;
    void updateSolo (bool snap) noexcept;
    void processChunk (float* l, float* r, int n, bool identical) noexcept;
    void runBand (Band& band, float* l, float* r, int n) noexcept;
    void runSolo (float* l, float* r, int n) noexcept;
    void blendVoice (Voice& v, float* l, float* r, const float* dryL, const float* dryR, int n) noexcept;
    void copyLeftStateToRight() noexcept;

    std::array<Raw, kNumBands> raw {};
    std::array<Band, kNumBands> bands;
    Voice solo;
    int soloWas = -1;

    double sampleRate = 48000.0;
    std::atomic<double> preparedRate { 48000.0 };
    float smoothCoeff = 0.05f;          // per update
    float mixStep = 0.002f;             // per sample
    bool needsSnap = true;
    bool rightStale = false;            // a mono block ran since the right channel's state was last in step with the left's
    int untilUpdate = 0;

    std::atomic<int> soloBand { -1 };

    dsp::AnalyzerFifo analyzerFifo;
    std::array<float, kChunk> analysisPre {}, analysisPost {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Equalizer)
};
}
