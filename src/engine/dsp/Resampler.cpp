// Resampler: kernel design and streaming convolution.
#include "engine/dsp/Resampler.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace hr::dsp
{
namespace
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr int kZeroCrossings = 16;
    constexpr double kKaiserBeta = 9.0;
    constexpr double kCutoffFraction = 0.94;

    double besselI0 (double x) noexcept
    {
        double sum = 1.0, term = 1.0;
        const double q = 0.25 * x * x;

        for (int k = 1; k < 60; ++k)
        {
            term *= q / ((double) k * (double) k);
            sum += term;

            if (term < 1.0e-14 * sum)
                break;
        }

        return sum;
    }

    float dot (const float* a, const float* b, int n) noexcept
    {
        float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
        int i = 0;

        for (; i + 4 <= n; i += 4)
        {
            s0 += a[i] * b[i];
            s1 += a[i + 1] * b[i + 1];
            s2 += a[i + 2] * b[i + 2];
            s3 += a[i + 3] * b[i + 3];
        }

        for (; i < n; ++i)
            s0 += a[i] * b[i];

        return (s0 + s1) + (s2 + s3);
    }
}

void Resampler::prepare (double rateIn, double rateOut, int numChannels, int maxInputBlock)
{
    // Rates are whole numbers of Hz in practice; the reduced fraction gives exact, periodic phases.
    const auto hzIn = std::max<long long> (1, std::llround (rateIn));
    const auto hzOut = std::max<long long> (1, std::llround (rateOut));
    const auto divisor = std::gcd (hzIn, hzOut);
    const auto fractionNum = hzIn / divisor;
    const auto fractionDen = hzOut / divisor;

    stepWhole = (int) (fractionNum / fractionDen);
    stepNum = (int) (fractionNum % fractionDen);
    denominator = (int) fractionDen;

    const double step = getStep();

    // Cut-off in cycles per input sample. Down-conversion has to filter below the output Nyquist, which makes the
    // kernel longer in input samples.
    const double fc = 0.5 * std::min (1.0, 1.0 / step) * kCutoffFraction;
    halfWidth = std::max (2, (int) std::ceil ((double) kZeroCrossings / (2.0 * fc)));
    taps = 2 * halfWidth;
    initialZeros = halfWidth + 2;

    table.assign ((size_t) ((kPhases + 1) * taps), 0.0f);
    const double i0Beta = besselI0 (kKaiserBeta);

    for (int p = 0; p <= kPhases; ++p)
    {
        const double frac = (double) p / (double) kPhases;
        double sum = 0.0;
        float* row = table.data() + (size_t) p * (size_t) taps;

        for (int t = 0; t < taps; ++t)
        {
            // Distance (in input samples) from the output centre to input tap t.
            const double d = (double) (t - halfWidth + 1) - frac;
            const double x = 2.0 * fc * d;
            const double sinc = std::fabs (x) < 1.0e-12 ? 1.0 : std::sin (kPi * x) / (kPi * x);
            const double u = d / (double) halfWidth;
            const double window = std::fabs (u) >= 1.0 ? 0.0 : besselI0 (kKaiserBeta * std::sqrt (1.0 - u * u)) / i0Beta;
            const double w = 2.0 * fc * sinc * window;

            row[t] = (float) w;
            sum += w;
        }

        // Unity gain at DC for every phase, so the fractional position never modulates the level.
        for (int t = 0; t < taps; ++t)
            row[t] = (float) ((double) row[t] / sum);
    }

    channels.assign ((size_t) std::max (1, numChannels), Channel {});

    for (auto& c : channels)
    {
        c.buffer.assign ((size_t) (taps + maxInputBlock + initialZeros + 16), 0.0f);
        resetChannel (c);
    }
}

void Resampler::resetChannel (Channel& c) const noexcept
{
    std::fill (c.buffer.begin(), c.buffer.end(), 0.0f);
    c.head = initialZeros;                  // history before the first real sample is silence
    c.posWhole = initialZeros;              // first output is centred on the first real input sample
    c.posNum = 0;
}

void Resampler::reset() noexcept
{
    for (auto& c : channels)
        resetChannel (c);
}

int Resampler::maxOutputFor (int numIn) const noexcept
{
    return (int) std::ceil ((double) numIn / getStep()) + 2;
}

int Resampler::process (int channel, const float* in, int numIn, float* out) noexcept
{
    auto& c = channels[(size_t) channel];
    float* buf = c.buffer.data();

    std::memcpy (buf + c.head, in, (size_t) numIn * sizeof (float));
    c.head += numIn;

    int produced = 0;
    int whole = c.posWhole;
    int num = c.posNum;

    for (;;)
    {
        if (whole + halfWidth >= c.head)
            break;

        const int scaled = num * kPhases;
        const int phase = scaled / denominator;
        const float a = (float) (scaled - phase * denominator) / (float) denominator;

        const float* x = buf + (whole - halfWidth + 1);
        const float* row0 = table.data() + (size_t) phase * (size_t) taps;
        const float* row1 = row0 + taps;

        out[produced++] = (1.0f - a) * dot (row0, x, taps) + a * dot (row1, x, taps);

        whole += stepWhole;
        num += stepNum;

        if (num >= denominator)
        {
            num -= denominator;
            ++whole;
        }
    }

    // Drop what the next output can no longer touch.
    const int keepFrom = std::max (0, whole - halfWidth + 1);

    if (keepFrom > 0)
    {
        std::memmove (buf, buf + keepFrom, (size_t) (c.head - keepFrom) * sizeof (float));
        c.head -= keepFrom;
        whole -= keepFrom;
    }

    c.posWhole = whole;
    c.posNum = num;
    return produced;
}

void Resampler::copyState (int from, int to) noexcept
{
    if (from == to)
        return;

    const auto& src = channels[(size_t) from];
    auto& dst = channels[(size_t) to];

    std::copy (src.buffer.begin(), src.buffer.end(), dst.buffer.begin());
    dst.head = src.head;
    dst.posWhole = src.posWhole;
    dst.posNum = src.posNum;
}
}
