// Spectrum analyzer for the EQ view: Hann-windowed FFT, 1/6-octave smoothing, fast-rise / slow-fall ballistics, log axis.
#pragma once

#include "Juce.h"

#include <memory>
#include <vector>

namespace hr::dsp
{
// UI-thread object; nothing here is real-time. Everything the per-frame path needs is allocated in prepare() (and
// the column maps, which are built once per view width, the first time a width is asked for).
//
//   push()        feeds mono samples; every hop (75 % overlap) a frame is analysed and the ballistics advance
//   getColumns()  one value per horizontal pixel of a log frequency axis: the loudest bin under the pixel, or the
//                 interpolated level where a pixel is narrower than a bin, plus a slope compensation for display
//
// Levels are dB relative to a full-scale sine (a 0 dBFS sine peaks at 0 dB before smoothing).
class SpectrumAnalyzer
{
public:
    static constexpr float kFloorDb = -120.0f;
    static constexpr float kFallDbPerSecond = 20.0f;
    static constexpr double kSmoothingOctaves = 1.0 / 6.0;

    SpectrumAnalyzer();
    ~SpectrumAnalyzer();

    void prepare (double sampleRate);           // (re)allocates; forgets everything
    void reset();                               // back to the floor, buffers empty
    void push (const float* mono, int numSamples);

    // dB per bin of slope compensation around 1 kHz (voice and music fall away towards the top: a flat-looking pink
    // spectrum makes the shape easier to read). Display only; the smoothed bins are untouched.
    void setTilt (float dbPerOctave);

    void getColumns (float* out, int numColumns, float minHz, float maxHz) const;

    int getFftSize() const noexcept             { return fftSize; }
    int getHopSize() const noexcept             { return hop; }
    int getNumBins() const noexcept             { return fftSize / 2 + 1; }
    double getSampleRate() const noexcept       { return sampleRate; }
    const std::vector<float>& getBinLevelsDb() const noexcept   { return levelDb; }
    uint64_t getNumFrames() const noexcept      { return framesDone; }

private:
    struct Mapping
    {
        int numColumns = 0;
        float minHz = 0.0f, maxHz = 0.0f, tilt = 0.0f;
        std::vector<int> first, last;           // bin range under each column (last < first: none)
        std::vector<float> blend, tiltDb;       // interpolation position between first-1 and first; slope offset
    };

    void processFrame();
    const Mapping& mappingFor (int numColumns, float minHz, float maxHz) const;

    double sampleRate = 48000.0;
    int fftSize = 0, hop = 0;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> window, ring, fftBuffer, levelDb, frameDb;
    std::vector<double> cumulative;
    std::vector<int> smoothLow, smoothHigh;
    int writePos = 0, filled = 0, sinceFrame = 0;
    float windowSum = 1.0f, tiltDbPerOct = 0.0f;
    uint64_t framesDone = 0;

    mutable std::vector<Mapping> mappings;
    mutable size_t nextMappingSlot = 0;
};
}
