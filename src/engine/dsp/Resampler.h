// Streaming windowed-sinc resampler (fixed ratio, any block size, allocation-free after prepare()).
#pragma once

#include <cstdint>
#include <vector>

namespace hr::dsp
{
// Converts a stream from rateIn to rateOut with a polyphase Kaiser-windowed sinc (256 phases, linear interpolation
// between phases, 16 zero crossings per side, cut-off at 94 % of the lower Nyquist). Up to two channels share the
// kernel table but keep their own history.
//
// Positions are exact: the input step per output sample is the reduced fraction rateIn / rateOut (147/160 for
// 44.1 -> 48 kHz) advanced with integer arithmetic. So the result does not depend on how the stream is cut into
// blocks, and there is no drift however long it runs (a double accumulator would slowly eat into a FIFO).
//
// Timing: the kernel is centred on each output position, so output sample k describes the input at time k * step
// with no fixed delay in *sample index* terms; what a real-time caller sees is that an output only exists once
// getHalfWidth() further input samples have arrived. The first blocks therefore yield a few samples fewer than
// numIn / step, which the caller covers with a FIFO pre-fill.
class Resampler
{
public:
    void prepare (double rateIn, double rateOut, int numChannels, int maxInputBlock);
    void reset() noexcept;

    // Upper bound of what process() writes for numIn input samples.
    int maxOutputFor (int numIn) const noexcept;

    // Consumes numIn samples (<= maxInputBlock) and returns how many output samples were written to `out`.
    int process (int channel, const float* in, int numIn, float* out) noexcept;

    // Makes `to` continue exactly where `from` is (used when a mono-optimised channel needs its own history).
    void copyState (int from, int to) noexcept;

    int getHalfWidth() const noexcept       { return halfWidth; }
    double getStep() const noexcept         { return (double) stepWhole + (double) stepNum / (double) denominator; }

private:
    struct Channel
    {
        std::vector<float> buffer;
        int head = 0;               // valid samples in buffer
        int posWhole = 0;           // buffer index of the next output's centre ...
        int posNum = 0;             // ... plus posNum / denominator of a sample
    };

    void resetChannel (Channel& c) const noexcept;

    static constexpr int kPhases = 256;

    int stepWhole = 1;              // step = stepWhole + stepNum / denominator input samples per output sample
    int stepNum = 0;
    int denominator = 1;
    int halfWidth = 1;
    int taps = 2;
    int initialZeros = 0;
    std::vector<float> table;       // (kPhases + 1) rows of `taps` weights
    std::vector<Channel> channels;
};
}
