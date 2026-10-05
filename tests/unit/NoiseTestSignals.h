// Signals for the noise removal tests: speech-like and room-noise stand-ins.
#pragma once

#include <algorithm>

#include "TestHelpers.h"

#include <cmath>
#include <random>
#include <vector>

namespace hr::test
{
    struct Resonator
    {
        void set (double frequency, double bandwidth, double sampleRate)
        {
            const double r = std::exp (-kPi * bandwidth / sampleRate);
            a1 = 2.0 * r * std::cos (2.0 * kPi * frequency / sampleRate);
            a2 = -r * r;
            b0 = 1.0 - r;
        }

        double tick (double x) noexcept
        {
            const double y = b0 * x + a1 * y1 + a2 * y2;
            y2 = y1;
            y1 = y;
            return y;
        }

        double a1 = 0.0, a2 = 0.0, b0 = 1.0, y1 = 0.0, y2 = 0.0;
    };

    // A stand-in for a talking person: a wobbling 120 Hz voice through three formants that change from syllable to
    // syllable (a, e, i, o, u), in phrases of two to four syllables with pauses between them. RMS 1.
    inline std::vector<float> makeSpeech (int numSamples, uint32_t seed = 3, double sampleRate = kRate)
    {
        std::mt19937 rng (seed);
        std::uniform_real_distribution<double> uniform (0.0, 1.0);

        struct Vowel { double f1, f2, f3; };
        const Vowel vowels[] { { 730, 1090, 2440 }, { 530, 1840, 2480 }, { 270, 2290, 3010 }, { 570, 840, 2410 }, { 300, 870, 2240 } };

        struct Syllable { int start, length; int vowel; };
        std::vector<Syllable> syllables;

        for (double t = 0.1; t < (double) numSamples / sampleRate - 0.4;)
        {
            const int inPhrase = 2 + (int) (uniform (rng) * 3.0);

            for (int i = 0; i < inPhrase; ++i)
            {
                const double length = 0.16 + 0.10 * uniform (rng);
                syllables.push_back ({ (int) (t * sampleRate), (int) (length * sampleRate), (int) (uniform (rng) * 5.0) % 5 });
                t += length + 0.02;
            }

            t += 1.2 + 1.0 * uniform (rng);
        }

        std::vector<float> out ((size_t) numSamples, 0.0f);
        Resonator formants[3];
        double phase = 0.0;
        size_t next = 0;
        int current = -1;

        for (int i = 0; i < numSamples; ++i)
        {
            const double t = (double) i / sampleRate;
            phase += 2.0 * kPi * (120.0 + 12.0 * std::sin (2.0 * kPi * 0.7 * t) + 4.0 * std::sin (2.0 * kPi * 3.1 * t)) / sampleRate;

            while (next < syllables.size() && i >= syllables[next].start + syllables[next].length)
                ++next;

            double envelope = 0.0;

            if (next < syllables.size() && i >= syllables[next].start)
            {
                const auto& s = syllables[next];
                envelope = std::pow (std::sin (kPi * (double) (i - s.start) / (double) s.length), 0.7);

                if (current != (int) next)
                {
                    current = (int) next;
                    const auto& v = vowels[s.vowel];
                    formants[0].set (v.f1, 80.0, sampleRate);
                    formants[1].set (v.f2, 110.0, sampleRate);
                    formants[2].set (v.f3, 160.0, sampleRate);
                }
            }

            // Harmonics of the voice, rolled off, by the Chebyshev recurrence.
            const double c = std::cos (phase);
            double sPrev = 0.0, sNow = std::sin (phase), source = 0.0;

            for (int h = 1; h <= 40; ++h)
            {
                source += sNow / std::pow ((double) h, 1.3);
                const double sNext = 2.0 * c * sNow - sPrev;
                sPrev = sNow;
                sNow = sNext;
            }

            double y = source;

            for (auto& f : formants)
                y = f.tick (y);

            out[(size_t) i] = (float) (y * envelope);
        }

        const double level = rms (out.data(), numSamples);

        for (auto& v : out)
            v = (float) ((double) v / juce::jmax (1.0e-9, level));

        return out;
    }

    // White noise and a fan: low-passed noise plus the blade-pass hum and its harmonics. RMS 1.
    inline std::vector<float> makeRoomNoise (int numSamples, uint32_t seed = 4, double sampleRate = kRate)
    {
        std::mt19937 rng (seed);
        std::normal_distribution<double> gauss (0.0, 1.0);
        std::vector<double> white ((size_t) numSamples), fan ((size_t) numSamples);
        double y1 = 0.0, y2 = 0.0;
        const double r = std::exp (-2.0 * kPi * 900.0 / sampleRate);

        for (int i = 0; i < numSamples; ++i)
        {
            white[(size_t) i] = gauss (rng);
            const double y = (1.0 - r) * (1.0 - r) * white[(size_t) i] + 2.0 * r * y1 - r * r * y2;      // two poles at 900 Hz
            y2 = y1;
            y1 = y;
            fan[(size_t) i] = y;
        }

        auto normalise = [] (std::vector<double>& v)
        {
            double e = 0.0;
            for (auto x : v) e += x * x;
            const double s = 1.0 / std::sqrt (juce::jmax (1.0e-30, e / (double) v.size()));
            for (auto& x : v) x *= s;
        };

        normalise (fan);

        for (int i = 0; i < numSamples; ++i)
            for (int k = 1; k <= 5; ++k)
                fan[(size_t) i] += 0.5 / k * std::sin (2.0 * kPi * 95.0 * k * (double) i / sampleRate + k);

        normalise (fan);
        normalise (white);

        std::vector<double> mixed ((size_t) numSamples);

        for (int i = 0; i < numSamples; ++i)
            mixed[(size_t) i] = 0.6 * white[(size_t) i] + fan[(size_t) i];

        normalise (mixed);
        std::vector<float> out (mixed.size());
        std::transform (mixed.begin(), mixed.end(), out.begin(), [] (double v) { return (float) v; });
        return out;
    }

}
