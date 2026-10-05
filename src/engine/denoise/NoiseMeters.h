// NoiseMeters: what the noise removal panel shows: noise reduced, speaking, voice level, room noise level.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>

namespace hr::denoise
{
// Updated once per audio block from lane 0 (the mic is one lane), read by the UI through atomics.
//
//  - voiceActive: the engine's voice probability >= 0.5, held for 250 ms so it does not flicker between words.
//  - noiseFloorDb: level (RMS, dBFS) of the input while nobody speaks, smoothed over ~1.5 s of such time.
//  - voiceLevelDb: level of the input while somebody speaks, smoothed over ~1 s of such time.
//    Voice minus noise floor is the signal-to-noise ratio of the room.
//  - reductionDb: how far the noise was turned down: input energy over output energy while nobody speaks
//    (smoothed over ~0.5 s of such time; it holds its value while you speak).
// Levels below -119 dB mean "not measured yet".
class NoiseMeters
{
public:
    static constexpr float kUnknownDb = -120.0f;

    void prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
        reset();
    }

    void reset() noexcept
    {
        holdLeft = 0;
        haveVoice = haveNoise = haveReduction = false;
        voiceEnergy = noiseEnergy = inEnergy = outEnergy = 0.0;
        active.store (false, std::memory_order_relaxed);
        reduction.store (0.0f, std::memory_order_relaxed);
        noiseFloor.store (kUnknownDb, std::memory_order_relaxed);
        voiceLevel.store (kUnknownDb, std::memory_order_relaxed);
        probability.store (0.0f, std::memory_order_relaxed);
    }

    // Audio thread. `dry` is the input aligned with `out`.
    void update (const float* dry, const float* out, int n, float voiceProbability) noexcept
    {
        double eIn = 0.0, eOut = 0.0;

        for (int i = 0; i < n; ++i)
        {
            eIn += (double) dry[i] * (double) dry[i];
            eOut += (double) out[i] * (double) out[i];
        }

        eIn /= (double) n;
        eOut /= (double) n;

        if (voiceProbability >= 0.5f)
            holdLeft = (int) (0.25 * sampleRate);
        else
            holdLeft = std::max (0, holdLeft - n);

        const bool voiced = holdLeft > 0;
        probability.store (voiceProbability, std::memory_order_relaxed);
        active.store (voiced, std::memory_order_relaxed);

        if (voiced)
        {
            voiceEnergy = haveVoice ? voiceEnergy + coefficient (n, 1.0) * (eIn - voiceEnergy) : eIn;
            haveVoice = true;
            voiceLevel.store (toDb (voiceEnergy), std::memory_order_relaxed);
        }
        else
        {
            noiseEnergy = haveNoise ? noiseEnergy + coefficient (n, 1.5) * (eIn - noiseEnergy) : eIn;
            haveNoise = true;
            noiseFloor.store (toDb (noiseEnergy), std::memory_order_relaxed);

            const double a = haveReduction ? coefficient (n, 0.5) : 1.0;
            inEnergy += a * (eIn - inEnergy);
            outEnergy += a * (eOut - outEnergy);
            haveReduction = true;
            reduction.store (std::max (0.0f, 10.0f * (float) std::log10 ((inEnergy + 1.0e-12) / (outEnergy + 1.0e-12))),
                             std::memory_order_relaxed);
        }
    }

    bool isVoiceActive() const noexcept         { return active.load (std::memory_order_relaxed); }
    float getReductionDb() const noexcept       { return reduction.load (std::memory_order_relaxed); }
    float getNoiseFloorDb() const noexcept      { return noiseFloor.load (std::memory_order_relaxed); }
    float getVoiceLevelDb() const noexcept      { return voiceLevel.load (std::memory_order_relaxed); }
    float getVoiceProbability() const noexcept  { return probability.load (std::memory_order_relaxed); }

private:
    static float toDb (double energy) noexcept  { return std::max (kUnknownDb, 10.0f * (float) std::log10 (energy + 1.0e-12)); }

    double coefficient (int n, double seconds) const noexcept
    {
        return 1.0 - std::exp (-(double) n / (seconds * sampleRate));
    }

    double sampleRate = 48000.0;
    int holdLeft = 0;
    bool haveVoice = false, haveNoise = false, haveReduction = false;
    double voiceEnergy = 0.0, noiseEnergy = 0.0, inEnergy = 0.0, outEnergy = 0.0;

    std::atomic<bool> active { false };
    std::atomic<float> reduction { 0.0f }, noiseFloor { kUnknownDb }, voiceLevel { kUnknownDb }, probability { 0.0f };
};
}
