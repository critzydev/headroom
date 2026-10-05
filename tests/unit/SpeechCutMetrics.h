// Measuring whether a noise remover "cuts the voice": real speech clips, noises, a mix at a chosen signal-to-noise
// ratio, and per-10 ms-frame statistics of what the remover did to the speech. The clips are in tests/data/speech (VCTK Corpus, CC BY 4.0;
// credits in THIRD-PARTY-NOTICES.txt).
//
// The measure, frame by frame (10 ms = 480 samples at 48 kHz), in the speech band (100 Hz - 7 kHz) and with the output
// aligned to the clean speech by the effect's reported latency:
//
//   active frame   the clean speech's energy is within 30 dB of the loudest frame of the sequence
//   level          10 log10 (energy of the output / energy of the clean speech)
//   cut frame      an active frame whose level is below -6 dB (or -12 dB): the speech got quieter by that much
//   onset / ending the first / last four active frames (40 ms) of a stretch of speech
//   pause frame    the clean speech is 50 dB below the peak and no active frame is within 200 ms
//   pause removal  10 log10 (energy of the noisy input / energy of the output) over the pause frames, all frequencies
//   gap removal    the same over the silent frames that are within 200 ms of speech (between words, after a phrase)
//   SNR gain       over the active frames, in the speech band: 10 log10 (energy of input - clean / energy of output - clean),
//                  i.e. noise removed and speech damaged counted together
//
// "Level" is a ratio of energies and not a projection of the output on the clean speech on purpose: noise left in the
// output can only make it larger, so an untouched or perfectly cleaned signal is never counted as cut, while a frame
// that the network muted is (the noise in it is muted just as much). It can miss a cut only where the speech is more
// than ~10 dB below the noise in that very frame.
#pragma once

#include "Juce.h"

#include "NoiseTestSignals.h"
#include "TestHelpers.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace hr::test::cut
{
    constexpr int kFrame = 480;

    // ---- Files ---------------------------------------------------------------------------------------

    // Reads a WAV file (PCM 8/16/24/32 bit or float32, any number of channels: the first one) at 48 kHz.
    inline bool readWav48k (const juce::File& file, std::vector<float>& out)
    {
        juce::MemoryBlock bytes;

        if (! file.existsAsFile() || ! file.loadFileAsData (bytes) || bytes.getSize() < 44)
            return false;

        const auto* p = static_cast<const uint8_t*> (bytes.getData());
        const size_t size = bytes.getSize();

        auto u16 = [&] (size_t at) { return (uint32_t) p[at] | ((uint32_t) p[at + 1] << 8); };
        auto u32 = [&] (size_t at) { return u16 (at) | (u16 (at + 2) << 16); };

        if (std::memcmp (p, "RIFF", 4) != 0 || std::memcmp (p + 8, "WAVE", 4) != 0)
            return false;

        uint32_t format = 0, channels = 0, rate = 0, bits = 0;
        size_t dataAt = 0, dataSize = 0;

        for (size_t at = 12; at + 8 <= size;)
        {
            const size_t chunk = u32 (at + 4);

            if (std::memcmp (p + at, "fmt ", 4) == 0 && at + 8 + 16 <= size)
            {
                format = u16 (at + 8);
                channels = u16 (at + 10);
                rate = u32 (at + 12);
                bits = u16 (at + 22);

                if (format == 0xFFFE && chunk >= 26)            // WAVE_FORMAT_EXTENSIBLE: the real tag is in the sub-format
                    format = u16 (at + 8 + 24);
            }
            else if (std::memcmp (p + at, "data", 4) == 0)
            {
                dataAt = at + 8;
                dataSize = std::min (chunk, size - dataAt);
                break;
            }

            at += 8 + chunk + (chunk & 1);
        }

        if (dataAt == 0 || channels == 0 || rate != 48000 || (format != 1 && format != 3))
            return false;

        const size_t bytesPerSample = bits / 8;

        if (bytesPerSample == 0 || (format == 3 && bits != 32))
            return false;

        const size_t frames = dataSize / (bytesPerSample * channels);
        out.assign (frames, 0.0f);

        for (size_t i = 0; i < frames; ++i)
        {
            const uint8_t* s = p + dataAt + i * bytesPerSample * channels;
            float v = 0.0f;

            if (format == 3)
            {
                const uint32_t raw = (uint32_t) s[0] | ((uint32_t) s[1] << 8) | ((uint32_t) s[2] << 16) | ((uint32_t) s[3] << 24);
                std::memcpy (&v, &raw, 4);
            }
            else if (bits == 8)
            {
                v = ((float) s[0] - 128.0f) / 128.0f;
            }
            else
            {
                int32_t raw = 0;

                for (size_t b = 0; b < bytesPerSample; ++b)
                    raw |= (int32_t) ((uint32_t) s[b] << (8 * (4 - bytesPerSample + b)));       // left-justified in 32 bits

                v = (float) ((double) raw / 2147483648.0);
            }

            out[i] = v;
        }

        return true;
    }

    // ---- Signals -------------------------------------------------------------------------------------

    // Clips one after the other: `lead` of silence, then each clip followed by `gap` of silence (exact zeros). A 3 ms
    // fade at both ends of each clip keeps the cut itself from being a click.
    inline std::vector<float> makeSequence (const std::vector<std::vector<float>>& clips, double lead = 0.8, double gap = 1.0)
    {
        std::vector<float> out ((size_t) (lead * kRate), 0.0f);
        const int fade = (int) (0.003 * kRate);

        for (const auto& clip : clips)
        {
            const size_t start = out.size();
            out.insert (out.end(), clip.begin(), clip.end());

            for (int i = 0; i < fade && (size_t) i < clip.size(); ++i)
            {
                const float g = 0.5f - 0.5f * std::cos ((float) kPi * (float) i / (float) fade);
                out[start + (size_t) i] *= g;
                out[out.size() - 1 - (size_t) i] *= g;
            }

            out.insert (out.end(), (size_t) (gap * kRate), 0.0f);
        }

        return out;
    }

    // Typing: bursts of 4-12 key strokes 110-230 ms apart, then 0.8-3 s of quiet. A stroke is a click (a few ms of damped
    // resonances between 1.2 and 6 kHz and a noise burst) followed by a smaller one when the key comes back up. RMS 1.
    inline std::vector<float> makeKeyboard (int numSamples, uint32_t seed = 5)
    {
        std::mt19937 rng (seed);
        std::uniform_real_distribution<double> uniform (0.0, 1.0);
        std::normal_distribution<double> gauss (0.0, 1.0);
        std::vector<double> v ((size_t) numSamples, 0.0);

        auto click = [&] (double at, double amplitude)
        {
            const int start = (int) at;
            double f[3], decay[3];

            for (int k = 0; k < 3; ++k)
            {
                f[k] = 1200.0 + 4800.0 * uniform (rng);
                decay[k] = 1.0 / (0.0006 + 0.0012 * uniform (rng));
            }

            for (int i = 0; i < 480 && start + i < numSamples; ++i)
            {
                const double t = (double) i / kRate;
                double s = 0.5 * gauss (rng) * std::exp (-t * 2500.0);

                for (int k = 0; k < 3; ++k)
                    s += std::sin (2.0 * kPi * f[k] * t) * std::exp (-t * decay[k]);

                v[(size_t) (start + i)] += amplitude * s;
            }
        };

        double t = 0.3;

        while (t * kRate < numSamples - 600)
        {
            const int keys = 4 + (int) (uniform (rng) * 9.0);

            for (int k = 0; k < keys && t * kRate < numSamples - 600; ++k)
            {
                const double a = 0.5 + 0.5 * uniform (rng);
                click (t * kRate, a);
                click ((t + 0.045 + 0.03 * uniform (rng)) * kRate, 0.35 * a);
                t += 0.11 + 0.12 * uniform (rng);
            }

            t += 0.8 + 2.2 * uniform (rng);
        }

        double e = 0.0;

        for (auto x : v)
            e += x * x;

        const double scale = 1.0 / std::sqrt (juce::jmax (1.0e-30, e / (double) numSamples));
        std::vector<float> out ((size_t) numSamples);
        std::transform (v.begin(), v.end(), out.begin(), [scale] (double x) { return (float) (x * scale); });
        return out;
    }

    // Room tone / air conditioning: pink noise with a slow swell. RMS 1.
    inline std::vector<float> makeRoomTone (int numSamples, uint32_t seed = 6)
    {
        std::mt19937 rng (seed);
        std::normal_distribution<double> gauss (0.0, 1.0);
        std::vector<double> v ((size_t) numSamples);
        double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;                  // Paul Kellet's pink filter

        for (int i = 0; i < numSamples; ++i)
        {
            const double w = gauss (rng);
            b0 = 0.99886 * b0 + w * 0.0555179;
            b1 = 0.99332 * b1 + w * 0.0750759;
            b2 = 0.96900 * b2 + w * 0.1538520;
            b3 = 0.86650 * b3 + w * 0.3104856;
            b4 = 0.55000 * b4 + w * 0.5329522;
            b5 = -0.7616 * b5 - w * 0.0168980;
            v[(size_t) i] = (b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362) * (1.0 + 0.25 * std::sin (2.0 * kPi * 0.13 * (double) i / kRate));
            b6 = w * 0.115926;
        }

        double e = 0.0;

        for (auto x : v)
            e += x * x;

        const double scale = 1.0 / std::sqrt (juce::jmax (1.0e-30, e / (double) numSamples));
        std::vector<float> out ((size_t) numSamples);
        std::transform (v.begin(), v.end(), out.begin(), [scale] (double x) { return (float) (x * scale); });
        return out;
    }

    // ---- Frames and filters --------------------------------------------------------------------------

    struct Biquad
    {
        void highPass (double f, double q = 0.70710678)     { set (f, q, true); }
        void lowPass (double f, double q = 0.70710678)      { set (f, q, false); }

        void set (double f, double q, bool high)
        {
            const double w = 2.0 * kPi * f / kRate, c = std::cos (w), alpha = std::sin (w) / (2.0 * q);
            const double a0 = 1.0 + alpha;
            b0 = (high ? (1.0 + c) : (1.0 - c)) / 2.0 / a0;
            b1 = (high ? -(1.0 + c) : (1.0 - c)) / a0;
            b2 = b0;
            a1 = -2.0 * c / a0;
            a2 = (1.0 - alpha) / a0;
        }

        double tick (double x) noexcept
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }

        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    };

    // 100 Hz - 7 kHz, fourth order on each side.
    inline std::vector<double> speechBand (const std::vector<float>& x)
    {
        Biquad hp[2], lp[2];

        for (auto& f : hp) f.highPass (100.0);
        for (auto& f : lp) f.lowPass (7000.0);

        std::vector<double> y (x.size());

        for (size_t i = 0; i < x.size(); ++i)
        {
            double v = (double) x[i];

            for (auto& f : hp) v = f.tick (v);
            for (auto& f : lp) v = f.tick (v);

            y[i] = v;
        }

        return y;
    }

    inline std::vector<double> frameEnergies (const std::vector<double>& x, int frames, int offset = 0)
    {
        std::vector<double> e ((size_t) frames, 0.0);

        for (int k = 0; k < frames; ++k)
            for (int i = 0; i < kFrame; ++i)
            {
                const size_t at = (size_t) (offset + k * kFrame + i);

                if (at < x.size())
                    e[(size_t) k] += x[at] * x[at];
            }

        return e;
    }

    // Which frames are speech: within `rangeDb` of the loudest one.
    inline std::vector<char> activeFrames (const std::vector<double>& energy, double rangeDb = 30.0)
    {
        const double peak = *std::max_element (energy.begin(), energy.end());
        std::vector<char> active (energy.size(), 0);

        for (size_t k = 0; k < energy.size(); ++k)
            active[k] = energy[k] > peak * std::pow (10.0, -rangeDb / 10.0) ? 1 : 0;

        return active;
    }

    // Mean power (per sample) over the active frames of the speech.
    inline double activePower (const std::vector<float>& speech)
    {
        std::vector<double> d (speech.begin(), speech.end());
        const int frames = (int) speech.size() / kFrame;
        const auto energy = frameEnergies (d, frames);
        const auto active = activeFrames (energy);
        double sum = 0.0;
        int n = 0;

        for (int k = 0; k < frames; ++k)
            if (active[(size_t) k])
            {
                sum += energy[(size_t) k];
                ++n;
            }

        return sum / (double) juce::jmax (1, n * kFrame);
    }

    struct Mixed
    {
        std::vector<float> clean, noisy;
    };

    // Speech scaled so that its active power is `speechDbfs` (RMS, dBFS), plus the noise at `snrDb` below that. The noise
    // starts at `noiseOffset` samples into `noise` (wrapping), so a long recording gives different stretches.
    inline Mixed mixAtSnr (const std::vector<float>& speech, const std::vector<float>& noise, double snrDb,
                           double speechDbfs = -26.0, size_t noiseOffset = 0)
    {
        Mixed m;
        m.clean.resize (speech.size());
        m.noisy.resize (speech.size());

        const double speechGain = std::sqrt (std::pow (10.0, speechDbfs / 10.0) / juce::jmax (1.0e-30, activePower (speech)));

        double noisePower = 0.0;

        for (size_t i = 0; i < speech.size(); ++i)
        {
            const double n = (double) noise[(noiseOffset + i) % noise.size()];
            noisePower += n * n;
        }

        noisePower /= (double) speech.size();
        const double noiseGain = std::sqrt (std::pow (10.0, (speechDbfs - snrDb) / 10.0) / juce::jmax (1.0e-30, noisePower));

        for (size_t i = 0; i < speech.size(); ++i)
        {
            m.clean[i] = (float) (speechGain * (double) speech[i]);
            m.noisy[i] = m.clean[i] + (float) (noiseGain * (double) noise[(noiseOffset + i) % noise.size()]);
        }

        return m;
    }

    // ---- The statistics ------------------------------------------------------------------------------

    struct Counts
    {
        int active = 0, cut6 = 0, cut12 = 0;
        int onset = 0, onsetCut6 = 0, onsetCut12 = 0;
        int ending = 0, endingCut6 = 0, endingCut12 = 0;
        int pause = 0;
        double pauseNoisyEnergy = 0.0, pauseOutEnergy = 0.0;
        int gap = 0;
        double gapNoisyEnergy = 0.0, gapOutEnergy = 0.0;
        double errorIn = 0.0, errorOut = 0.0;       // speech band, active frames: energy of (input - clean), (output - clean)
        double levelSum = 0.0;                      // dB, over the active frames
        double worstLevel = 0.0;

        Counts& operator+= (const Counts& o)
        {
            active += o.active; cut6 += o.cut6; cut12 += o.cut12;
            onset += o.onset; onsetCut6 += o.onsetCut6; onsetCut12 += o.onsetCut12;
            ending += o.ending; endingCut6 += o.endingCut6; endingCut12 += o.endingCut12;
            pause += o.pause; pauseNoisyEnergy += o.pauseNoisyEnergy; pauseOutEnergy += o.pauseOutEnergy;
            gap += o.gap; gapNoisyEnergy += o.gapNoisyEnergy; gapOutEnergy += o.gapOutEnergy;
            errorIn += o.errorIn; errorOut += o.errorOut;
            levelSum += o.levelSum; worstLevel = std::min (worstLevel, o.worstLevel);
            return *this;
        }

        static double percent (int part, int whole)     { return whole > 0 ? 100.0 * part / whole : 0.0; }

        double cut6Percent() const                      { return percent (cut6, active); }
        double cut12Percent() const                     { return percent (cut12, active); }
        double onsetCut12Percent() const                { return percent (onsetCut12, onset); }
        double endingCut12Percent() const               { return percent (endingCut12, ending); }
        double meanLevelDb() const                      { return active > 0 ? levelSum / active : 0.0; }
        double gapRemovalDb() const                     { return gap > 0 ? 10.0 * std::log10 (juce::jmax (1.0e-30, gapNoisyEnergy) / juce::jmax (1.0e-30, gapOutEnergy)) : 0.0; }
        double snrGainDb() const                        { return errorIn > 0.0 ? 10.0 * std::log10 (errorIn / juce::jmax (1.0e-30, errorOut)) : 0.0; }
        double pauseRemovalDb() const                   { return pause > 0 ? 10.0 * std::log10 (juce::jmax (1.0e-30, pauseNoisyEnergy) / juce::jmax (1.0e-30, pauseOutEnergy)) : 0.0; }
    };

    // `y` is the effect's output for `noisy` (same length plus whatever the latency adds): y[t + latency] belongs to noisy[t].
    inline Counts analyse (const Mixed& m, const std::vector<float>& y, int latency)
    {
        const int frames = (int) m.clean.size() / kFrame;
        const auto cleanBand = speechBand (m.clean);
        const auto outBand = speechBand (y);
        const auto noisyBand = speechBand (m.noisy);
        const auto cleanE = frameEnergies (cleanBand, frames);
        const auto outE = frameEnergies (outBand, frames, latency);
        const auto active = activeFrames (cleanE);

        const double peak = *std::max_element (cleanE.begin(), cleanE.end());

        // Broadband energies for the pauses.
        std::vector<double> noisyAll (m.noisy.begin(), m.noisy.end()), outAll (y.begin(), y.end());
        const auto noisyE = frameEnergies (noisyAll, frames);
        const auto outAllE = frameEnergies (outAll, frames, latency);

        auto isActive = [&] (int k) { return k >= 0 && k < frames && active[(size_t) k] != 0; };

        // A stretch of speech begins at an active frame with no active frame in the seven before it (70 ms: a stop closure
        // inside a word is shorter) and ends at one with none in the seven after it. Its first / last four frames are the
        // onset / the ending.
        std::vector<char> onset ((size_t) frames, 0), ending ((size_t) frames, 0);

        for (int k = 0; k < frames; ++k)
        {
            if (! isActive (k))
                continue;

            bool first = true, last = true;

            for (int j = 1; j <= 7; ++j)
            {
                first = first && ! isActive (k - j);
                last = last && ! isActive (k + j);
            }

            for (int j = 0; j < 4; ++j)
            {
                if (first && isActive (k + j)) onset[(size_t) (k + j)] = 1;
                if (last && isActive (k - j))  ending[(size_t) (k - j)] = 1;
            }
        }

        Counts c;

        for (int k = 0; k < frames; ++k)
        {
            if (active[(size_t) k])
            {
                for (int i = 0; i < kFrame; ++i)
                {
                    const size_t at = (size_t) (k * kFrame + i);
                    const double in = noisyBand[at] - cleanBand[at];
                    const double out = (at + (size_t) latency < outBand.size() ? outBand[at + (size_t) latency] : 0.0) - cleanBand[at];
                    c.errorIn += in * in;
                    c.errorOut += out * out;
                }

                const double level = 10.0 * std::log10 (juce::jmax (1.0e-30, outE[(size_t) k]) / cleanE[(size_t) k]);

                ++c.active;
                c.levelSum += level;
                c.worstLevel = std::min (c.worstLevel, level);

                if (level < -6.0)  ++c.cut6;
                if (level < -12.0) ++c.cut12;

                if (onset[(size_t) k])
                {
                    ++c.onset;
                    if (level < -6.0)  ++c.onsetCut6;
                    if (level < -12.0) ++c.onsetCut12;
                }

                if (ending[(size_t) k])
                {
                    ++c.ending;
                    if (level < -6.0)  ++c.endingCut6;
                    if (level < -12.0) ++c.endingCut12;
                }
            }
            else if (cleanE[(size_t) k] < peak * 1.0e-5)
            {
                bool far = true;

                for (int j = -20; j <= 20 && far; ++j)
                    far = ! isActive (k + j);

                if (far)
                {
                    ++c.pause;
                    c.pauseNoisyEnergy += noisyE[(size_t) k];
                    c.pauseOutEnergy += outAllE[(size_t) k];
                }
                else
                {
                    ++c.gap;
                    c.gapNoisyEnergy += noisyE[(size_t) k];
                    c.gapOutEnergy += outAllE[(size_t) k];
                }
            }
        }

        return c;
    }
}
