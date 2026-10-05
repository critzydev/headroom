// Shared helpers for the engine and DSP unit tests: an engine harness, test processors and signal measurements.
#pragma once

#include "Juce.h"

#include "engine/AudioEngine.h"
#include "engine/FxChain.h"

#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <random>
#include <vector>

namespace hr::test
{
constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------------------------------
// Signals and measurements
// ---------------------------------------------------------------------------------------------------

inline std::vector<float> makeSine (int numSamples, double sampleRate, double frequency, double amplitude, double startPhase = 0.0)
{
    std::vector<float> v ((size_t) numSamples);

    for (int i = 0; i < numSamples; ++i)
        v[(size_t) i] = (float) (amplitude * std::sin (2.0 * kPi * frequency * (double) i / sampleRate + startPhase));

    return v;
}

inline std::vector<float> makeNoise (int numSamples, double amplitude, uint32_t seed = 1)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> dist (-1.0f, 1.0f);
    std::vector<float> v ((size_t) numSamples);

    for (auto& x : v)
        x = (float) amplitude * dist (rng);

    return v;
}

inline double rms (const float* x, int n)
{
    double sum = 0.0;

    for (int i = 0; i < n; ++i)
        sum += (double) x[i] * (double) x[i];

    return std::sqrt (sum / (double) juce::jmax (1, n));
}

inline double toDb (double linear)      { return 20.0 * std::log10 (juce::jmax (linear, 1.0e-12)); }

// Amplitude (peak-equivalent, in dB re 1.0) of the component at `frequency`, by correlation over whole cycles
// of the window (Hann-weighted to keep leakage out of the way).
inline double toneDb (const float* x, int n, double sampleRate, double frequency)
{
    double re = 0.0, im = 0.0, norm = 0.0;

    for (int i = 0; i < n; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * kPi * (double) i / (double) (n - 1));
        const double phase = 2.0 * kPi * frequency * (double) i / sampleRate;
        re += w * (double) x[i] * std::cos (phase);
        im += w * (double) x[i] * std::sin (phase);
        norm += w;
    }

    return toDb (2.0 * std::sqrt (re * re + im * im) / norm);
}

inline float peakAbs (const float* x, int n)
{
    float p = 0.0f;

    for (int i = 0; i < n; ++i)
        p = juce::jmax (p, std::fabs (x[i]));

    return p;
}

inline bool allFinite (const float* x, int n)
{
    for (int i = 0; i < n; ++i)
        if (! std::isfinite (x[i]))
            return false;

    return true;
}

// ---------------------------------------------------------------------------------------------------
// Running a processor on plain arrays, in blocks of a given size (0 = one big block)
// ---------------------------------------------------------------------------------------------------

inline void runProcessor (juce::AudioProcessor& p, float* l, float* r, int numSamples, int blockSize = 0)
{
    juce::MidiBuffer midi;
    const int block = blockSize > 0 ? blockSize : juce::jmax (1, numSamples);

    for (int offset = 0; offset < numSamples; offset += block)
    {
        const int n = juce::jmin (block, numSamples - offset);
        float* channels[2] { l + offset, r + offset };
        juce::AudioBuffer<float> buffer (channels, 2, n);
        p.processBlock (buffer, midi);
    }
}

// Mono-in-both-channels convenience: processes `signal` in place (both channels identical) and returns it.
inline std::vector<float> runMono (juce::AudioProcessor& p, std::vector<float> signal, int blockSize = 0)
{
    auto copy = signal;
    runProcessor (p, signal.data(), copy.data(), (int) signal.size(), blockSize);
    return signal;
}

// ---------------------------------------------------------------------------------------------------
// Test processors
// ---------------------------------------------------------------------------------------------------

// Base with the boilerplate every stand-in plugin needs.
class TestProcessor : public juce::AudioProcessor
{
public:
    TestProcessor()
        : juce::AudioProcessor (BusesProperties().withInput ("in", juce::AudioChannelSet::stereo(), true)
                                                 .withOutput ("out", juce::AudioChannelSet::stereo(), true)) {}

    const juce::String getName() const override                    { return "Test"; }
    void prepareToPlay (double sampleRate, int) override            { lastPreparedRate = sampleRate; ++prepareCount; }
    void releaseResources() override                                {}
    double getTailLengthSeconds() const override                    { return 0.0; }
    bool acceptsMidi() const override                               { return false; }
    bool producesMidi() const override                              { return false; }
    juce::AudioProcessorEditor* createEditor() override             { return nullptr; }
    bool hasEditor() const override                                 { return false; }
    int getNumPrograms() override                                   { return 1; }
    int getCurrentProgram() override                                { return 0; }
    void setCurrentProgram (int) override                           {}
    const juce::String getProgramName (int) override                { return {}; }
    void changeProgramName (int, const juce::String&) override      {}
    void getStateInformation (juce::MemoryBlock&) override          {}
    void setStateInformation (const void*, int) override            {}

    double lastPreparedRate = 0.0;
    int prepareCount = 0;
};

// Halves the signal: a stand-in for a hosted plugin.
class HalvingProcessor final : public TestProcessor
{
public:
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override    { b.applyGain (0.5f); }
};

// Writes a chosen bad value into the first samples of every block, otherwise passes the audio through.
class MisbehavingProcessor final : public TestProcessor
{
public:
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        for (int c = 0; c < b.getNumChannels(); ++c)
            for (int i = 0; i < juce::jmin (8, b.getNumSamples()); ++i)
                b.getWritePointer (c)[i] = badValue;
    }

    float badValue = std::numeric_limits<float>::quiet_NaN();
};

// ---------------------------------------------------------------------------------------------------
// Engine harness: owns the buffers behind an EngineIO so tests can say "slot 0 gets DC 0.1" and read outputs back.
// ---------------------------------------------------------------------------------------------------

struct EngineHarness
{
    explicit EngineHarness (int bufferFrames = 4096)
        : capacity (bufferFrames), engine (std::make_unique<AudioEngine>())
    {
        for (auto& v : outputs)
            v.assign ((size_t) capacity, 0.0f);
    }

    void setAppInput (int slot, float left, float right)
    {
        appBuffers[(size_t) slot][0].assign ((size_t) capacity, left);
        appBuffers[(size_t) slot][1].assign ((size_t) capacity, right);
        appLinked[(size_t) slot] = true;
    }

    void setAppInput (int slot, float value)        { setAppInput (slot, value, value); }

    void setAppSignal (int slot, const std::vector<float>& left, const std::vector<float>& right)
    {
        appBuffers[(size_t) slot][0] = left;
        appBuffers[(size_t) slot][1] = right;
        appBuffers[(size_t) slot][0].resize ((size_t) capacity, 0.0f);
        appBuffers[(size_t) slot][1].resize ((size_t) capacity, 0.0f);
        appLinked[(size_t) slot] = true;
    }

    void setAppRamp (int slot, float step)
    {
        for (int c = 0; c < 2; ++c)
        {
            appBuffers[(size_t) slot][(size_t) c].resize ((size_t) capacity);

            for (int i = 0; i < capacity; ++i)
                appBuffers[(size_t) slot][(size_t) c][(size_t) i] = step * (float) i;
        }

        appLinked[(size_t) slot] = true;
    }

    // Input channels (a second mic, a capture card): the same pairs as the app slots, in their own block.
    void setInputInput (int slot, float left, float right)
    {
        inputBuffers[(size_t) slot][0].assign ((size_t) capacity, left);
        inputBuffers[(size_t) slot][1].assign ((size_t) capacity, right);
        inputLinked[(size_t) slot] = true;
    }

    void setInputInput (int slot, float value)      { setInputInput (slot, value, value); }

    void setInputSignal (int slot, const std::vector<float>& left, const std::vector<float>& right)
    {
        inputBuffers[(size_t) slot][0] = left;
        inputBuffers[(size_t) slot][1] = right;
        inputBuffers[(size_t) slot][0].resize ((size_t) capacity, 0.0f);
        inputBuffers[(size_t) slot][1].resize ((size_t) capacity, 0.0f);
        inputLinked[(size_t) slot] = true;
    }

    void unlinkInput (int slot)                     { inputLinked[(size_t) slot] = false; }

    void setMic (float left, float* rightOrNull = nullptr)
    {
        micBuffers[0].assign ((size_t) capacity, left);

        if (rightOrNull != nullptr)
            micBuffers[1].assign ((size_t) capacity, *rightOrNull);

        micLinked[0] = true;
        micLinked[1] = rightOrNull != nullptr;
    }

    void setMicSignal (const std::vector<float>& mono)
    {
        micBuffers[0] = mono;
        micBuffers[0].resize ((size_t) capacity, 0.0f);
        micLinked[0] = true;
        micLinked[1] = false;
    }

    void setMicSilent()     { micBuffers[0].assign ((size_t) capacity, 0.0f); micLinked[0] = true; micLinked[1] = false; }
    void unlinkMic()        { micLinked[0] = micLinked[1] = false; }

    void poisonOutputs (float value = 7.0f)
    {
        for (auto& v : outputs)
            v.assign ((size_t) capacity, value);
    }

    EngineIO makeIO (int numFrames, double sampleRate = kRate)
    {
        EngineIO io;
        io.numFrames = numFrames;
        io.sampleRate = sampleRate;

        for (int k = 0; k < kMaxAppChannels; ++k)
            for (int c = 0; c < 2; ++c)
                io.appIn[k][c] = appLinked[(size_t) k] ? appBuffers[(size_t) k][(size_t) c].data() : nullptr;

        for (int k = 0; k < kMaxInputChannels; ++k)
            for (int c = 0; c < 2; ++c)
                io.inputIn[k][c] = inputLinked[(size_t) k] ? inputBuffers[(size_t) k][(size_t) c].data() : nullptr;

        io.micIn[0] = micLinked[0] ? micBuffers[0].data() : nullptr;
        io.micIn[1] = micLinked[1] ? micBuffers[1].data() : nullptr;
        io.monitorOut[0] = outputs[0].data();  io.monitorOut[1] = outputs[1].data();
        io.streamOut[0]  = outputs[2].data();  io.streamOut[1]  = outputs[3].data();
        io.micOut[0]     = outputs[4].data();  io.micOut[1]     = outputs[5].data();
        return io;
    }

    void run (int numFrames, double sampleRate = kRate)     { engine->process (makeIO (numFrames, sampleRate)); }

    // Runs enough audio for every 20 ms smoother (and the master limiter's delay) to have settled.
    void settle (double sampleRate = kRate)
    {
        for (int i = 0; i < 3; ++i)
            run (capacity, sampleRate);
    }

    // Pushes silence through app slot `slot` (and the master delay line) so the next signal starts from a clean stream.
    void flush (int slot)
    {
        setAppInput (slot, 0.0f);
        run (juce::jmin (capacity, 4 * kMaxBlock));
    }

    const std::vector<float>& monitorL() const  { return outputs[0]; }
    const std::vector<float>& monitorR() const  { return outputs[1]; }
    const std::vector<float>& streamL() const   { return outputs[2]; }
    const std::vector<float>& streamR() const   { return outputs[3]; }
    const std::vector<float>& micOutL() const   { return outputs[4]; }
    const std::vector<float>& micOutR() const   { return outputs[5]; }

    int capacity;
    std::unique_ptr<AudioEngine> engine;
    std::array<std::array<std::vector<float>, 2>, kMaxAppChannels> appBuffers;
    std::array<bool, kMaxAppChannels> appLinked {};
    std::array<std::array<std::vector<float>, 2>, kMaxInputChannels> inputBuffers;
    std::array<bool, kMaxInputChannels> inputLinked {};
    std::array<std::vector<float>, 2> micBuffers;
    std::array<bool, 2> micLinked {};
    std::array<std::vector<float>, 6> outputs;
};
}
