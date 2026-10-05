// SpectrumAnalyzer implementation.
#include "engine/dsp/SpectrumAnalyzer.h"

#include <algorithm>
#include <cmath>

namespace hr::dsp
{
namespace
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr size_t kMaxMappings = 4;
}

SpectrumAnalyzer::SpectrumAnalyzer()
{
    prepare (48000.0);
}

SpectrumAnalyzer::~SpectrumAnalyzer() = default;

void SpectrumAnalyzer::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;

    // About 85 ms of signal per frame whatever the rate, in a power-of-two size.
    int order = 12;

    if (sampleRate > 64000.0)   ++order;
    if (sampleRate > 128000.0)  ++order;

    fftSize = 1 << order;
    hop = fftSize / 4;                                  // 75 % overlap
    fft = std::make_unique<juce::dsp::FFT> (order);

    const int half = fftSize / 2;

    window.assign ((size_t) fftSize, 0.0f);
    double sum = 0.0;

    for (int i = 0; i < fftSize; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * kPi * (double) i / (double) fftSize);      // periodic Hann
        window[(size_t) i] = (float) w;
        sum += w;
    }

    windowSum = (float) sum;

    ring.assign ((size_t) fftSize, 0.0f);
    fftBuffer.assign ((size_t) fftSize * 2, 0.0f);
    levelDb.assign ((size_t) half + 1, kFloorDb);
    frameDb.assign ((size_t) half + 1, kFloorDb);
    cumulative.assign ((size_t) half + 2, 0.0);

    // 1/6 octave around each bin (bins narrower than that are not smoothed at all).
    smoothLow.assign ((size_t) half + 1, 0);
    smoothHigh.assign ((size_t) half + 1, 0);
    const double edge = std::pow (2.0, 0.5 * kSmoothingOctaves);

    for (int k = 0; k <= half; ++k)
    {
        smoothLow[(size_t) k] = juce::jmax (1, (int) std::floor ((double) k / edge));
        smoothHigh[(size_t) k] = juce::jmin (half, (int) std::ceil ((double) k * edge));

        if (k == 0)
            smoothLow[0] = smoothHigh[0] = 1;
    }

    mappings.clear();
    mappings.reserve (kMaxMappings);
    nextMappingSlot = 0;

    reset();
}

void SpectrumAnalyzer::reset()
{
    std::fill (levelDb.begin(), levelDb.end(), kFloorDb);
    std::fill (frameDb.begin(), frameDb.end(), kFloorDb);
    std::fill (ring.begin(), ring.end(), 0.0f);
    writePos = filled = sinceFrame = 0;
    framesDone = 0;
}

void SpectrumAnalyzer::setTilt (float dbPerOctave)
{
    tiltDbPerOct = dbPerOctave;
}

void SpectrumAnalyzer::push (const float* mono, int numSamples)
{
    const int mask = fftSize - 1;

    for (int i = 0; i < numSamples; ++i)
    {
        ring[(size_t) writePos] = mono[i];
        writePos = (writePos + 1) & mask;

        if (filled < fftSize)
            ++filled;

        if (++sinceFrame >= hop && filled >= fftSize)
        {
            sinceFrame = 0;
            processFrame();
        }
    }
}

void SpectrumAnalyzer::processFrame()
{
    const int half = fftSize / 2;
    const int mask = fftSize - 1;

    // The ring holds the newest fftSize samples; writePos is where the oldest one sits.
    for (int i = 0, idx = writePos; i < fftSize; ++i, idx = (idx + 1) & mask)
        fftBuffer[(size_t) i] = ring[(size_t) idx] * window[(size_t) i];

    fft->performFrequencyOnlyForwardTransform (fftBuffer.data(), true);

    // Power of the equivalent sine (a full-scale sine is 1.0 = 0 dB), as a running sum for the smoothing.
    const float scale = 2.0f / windowSum;
    double running = 0.0;

    cumulative[0] = 0.0;

    for (int k = 0; k <= half; ++k)
    {
        const double a = (double) (fftBuffer[(size_t) k] * scale);
        running += a * a;
        cumulative[(size_t) k + 1] = running;
    }

    const float fall = kFallDbPerSecond * (float) hop / (float) sampleRate;

    for (int k = 1; k <= half; ++k)
    {
        const int lo = smoothLow[(size_t) k], hi = smoothHigh[(size_t) k];
        const double mean = (cumulative[(size_t) hi + 1] - cumulative[(size_t) lo]) / (double) (hi - lo + 1);
        const float db = juce::jmax (kFloorDb, (float) (10.0 * std::log10 (mean + 1.0e-13)));

        frameDb[(size_t) k] = db;
        levelDb[(size_t) k] = juce::jmax (db, levelDb[(size_t) k] - fall);      // fast rise, slow fall
    }

    levelDb[0] = levelDb[1];
    ++framesDone;
}

const SpectrumAnalyzer::Mapping& SpectrumAnalyzer::mappingFor (int numColumns, float minHz, float maxHz) const
{
    for (const auto& m : mappings)
        if (m.numColumns == numColumns && m.minHz == minHz && m.maxHz == maxHz && m.tilt == tiltDbPerOct)
            return m;

    Mapping m;
    m.numColumns = numColumns;
    m.minHz = minHz;
    m.maxHz = maxHz;
    m.tilt = tiltDbPerOct;
    m.first.resize ((size_t) numColumns);
    m.last.resize ((size_t) numColumns);
    m.blend.resize ((size_t) numColumns);
    m.tiltDb.resize ((size_t) numColumns);

    const int half = fftSize / 2;
    const double binsPerHz = (double) fftSize / sampleRate;
    const double ratio = (double) maxHz / (double) minHz;

    for (int x = 0; x < numColumns; ++x)
    {
        const double fLow = (double) minHz * std::pow (ratio, (double) x / (double) numColumns);
        const double fHigh = (double) minHz * std::pow (ratio, (double) (x + 1) / (double) numColumns);
        const double kLow = fLow * binsPerHz, kHigh = fHigh * binsPerHz;

        int first = juce::jlimit (1, half, (int) std::ceil (kLow - 1.0e-6));
        int last = juce::jlimit (0, half, (int) std::floor (kHigh + 1.0e-6));
        float blend = 0.0f;

        if (last < first)
        {
            // No bin inside this pixel: read between the two that surround it.
            const double centre = juce::jlimit (1.0, (double) half - 1.0e-3, 0.5 * (kLow + kHigh));
            first = (int) std::floor (centre);
            last = first - 1;
            blend = (float) (centre - (double) first);
        }

        m.first[(size_t) x] = first;
        m.last[(size_t) x] = last;
        m.blend[(size_t) x] = blend;
        m.tiltDb[(size_t) x] = tiltDbPerOct * (float) std::log2 (std::sqrt (fLow * fHigh) / 1000.0);
    }

    if (mappings.size() < kMaxMappings)
    {
        mappings.push_back (std::move (m));
        return mappings.back();
    }

    mappings[nextMappingSlot] = std::move (m);
    const auto& stored = mappings[nextMappingSlot];
    nextMappingSlot = (nextMappingSlot + 1) % kMaxMappings;
    return stored;
}

void SpectrumAnalyzer::getColumns (float* out, int numColumns, float minHz, float maxHz) const
{
    if (numColumns <= 0)
        return;

    const auto& m = mappingFor (numColumns, minHz, maxHz);
    const int half = fftSize / 2;

    for (int x = 0; x < numColumns; ++x)
    {
        const int first = m.first[(size_t) x], last = m.last[(size_t) x];
        float db;

        if (last >= first)
        {
            db = levelDb[(size_t) first];

            for (int k = first + 1; k <= last; ++k)
                db = juce::jmax (db, levelDb[(size_t) k]);
        }
        else
        {
            const float b = m.blend[(size_t) x];
            db = levelDb[(size_t) first] * (1.0f - b) + levelDb[(size_t) juce::jmin (first + 1, half)] * b;
        }

        out[x] = db + m.tiltDb[(size_t) x];
    }
}
}
