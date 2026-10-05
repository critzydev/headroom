// EQ: parameter layout, old-state migration, the band DSP (two-voice crossfades, mono sharing, analyzer feed, solo) and the response.
#include "engine/builtin/Equalizer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace hr
{
namespace
{
    constexpr double kSmoothSeconds = 0.012;
    constexpr double kMixSeconds = 0.010;
    constexpr float kButterworthQ = 0.70710678f;

    struct BandDefault
    {
        bool on;
        Equalizer::Type type;
        float freq, q;
    };

    using Ty = Equalizer::Type;

    // Band 1 is the first-run high-pass; the rest wait, off, where a voice EQ usually wants them.
    constexpr BandDefault kBandDefaults[Equalizer::kNumBands] = {
        { true,  Ty::lowCut,    80.0f,    kButterworthQ },
        { false, Ty::lowShelf,  100.0f,   kButterworthQ },
        { false, Ty::peak,      250.0f,   1.0f },
        { false, Ty::peak,      1000.0f,  1.0f },
        { false, Ty::peak,      3000.0f,  1.0f },
        { false, Ty::peak,      6000.0f,  1.0f },
        { false, Ty::peak,      10000.0f, 1.0f },
        { false, Ty::highShelf, 16000.0f, kButterworthQ },
    };

    const char* const kTypeNames[Equalizer::kNumTypes] = { "Peak", "Low shelf", "High shelf", "Low cut", "High cut", "Notch", "Band pass", "Tilt shelf" };
    const char* const kSlopeNames[Equalizer::kNumSlopes] = { "6 dB/oct", "12 dB/oct", "24 dB/oct", "48 dB/oct", "24 dB/oct (LR)" };

    // ---- The pre-0.2 layout ------------------------------------------------------------------------
    struct LegacyParam
    {
        const char* id;
        float defaultValue;
    };

    constexpr LegacyParam kLegacy[] = {
        { "hpfOn", 1.0f },          { "hpfFreq", 80.0f },       { "hpfSlope", 0.0f },
        { "lowShelfFreq", 120.0f }, { "lowShelfGain", 0.0f },
        { "bell1Freq", 250.0f },    { "bell1Gain", 0.0f },      { "bell1Q", 1.0f },
        { "bell2Freq", 1000.0f },   { "bell2Gain", 0.0f },      { "bell2Q", 1.0f },
        { "bell3Freq", 4000.0f },   { "bell3Gain", 0.0f },      { "bell3Q", 1.0f },
        { "highShelfFreq", 8000.0f }, { "highShelfGain", 0.0f },
        { "lpfOn", 0.0f },          { "lpfFreq", 18000.0f },
    };

    inline float sumToMonoSample (float a, float b) noexcept    { return 0.5f * (a + b); }

    void sumToMono (const float* l, const float* r, int n, float* out) noexcept
    {
        if (r == nullptr)
            std::memcpy (out, l, (size_t) n * sizeof (float));
        else
            for (int i = 0; i < n; ++i)
                out[i] = sumToMonoSample (l[i], r[i]);
    }
}

Equalizer::Equalizer()
    : BuiltinProcessor (kId, "EQ", createLayout())
{
    for (int b = 0; b < kNumBands; ++b)
    {
        auto p = [this, b] (const char* what) { return apvts.getRawParameterValue (bandParamId (b, what)); };
        auto& r = raw[(size_t) b];

        r.on = p ("On");
        r.type = p ("Type");
        r.freq = p ("Freq");
        r.gain = p ("Gain");
        r.q = p ("Q");
        r.slope = p ("Slope");
    }

    prepareToPlay (48000.0, 512);   // valid state even if nobody prepares us (response curve, tests)
}

// ---------------------------------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------------------------------

juce::String Equalizer::bandParamId (int band, const char* what)
{
    return "b" + juce::String (band + 1) + what;
}

const char* Equalizer::typeName (Type type) noexcept
{
    return kTypeNames[juce::jlimit (0, kNumTypes - 1, (int) type)];
}

const char* Equalizer::slopeName (int slope) noexcept
{
    return kSlopeNames[juce::jlimit (0, kNumSlopes - 1, slope)];
}

bool Equalizer::hasGain (Type type) noexcept
{
    return type == Type::peak || type == Type::lowShelf || type == Type::highShelf || type == Type::tiltShelf;
}

Equalizer::BandSettings Equalizer::getDefaultBand (int band) noexcept
{
    const auto& d = kBandDefaults[juce::jlimit (0, kNumBands - 1, band)];

    BandSettings s;
    s.on = d.on;
    s.type = d.type;
    s.freq = d.freq;
    s.q = d.q;
    return s;
}

BuiltinProcessor::Layout Equalizer::createLayout()
{
    Layout layout;

    juce::StringArray types, slopes;

    for (const auto* name : kTypeNames)
        types.add (name);

    for (const auto* name : kSlopeNames)
        slopes.add (name);

    for (int b = 0; b < kNumBands; ++b)
    {
        const auto& d = kBandDefaults[b];
        const auto n = juce::String (b + 1);

        layout.add (
            makeBool (bandParamId (b, "On"), "Band " + n + " on", d.on),
            makeChoice (bandParamId (b, "Type"), "Band " + n + " type", types, (int) d.type),
            makeFloat (bandParamId (b, "Freq"), "Band " + n + " frequency", kMinFreq, kMaxFreq, d.freq, "Hz", 1000.0f),
            makeFloat (bandParamId (b, "Gain"), "Band " + n + " gain", -kMaxGainDb, kMaxGainDb, 0.0f, "dB"),
            makeFloat (bandParamId (b, "Q"), "Band " + n + " Q", kMinQ, kMaxQ, d.q, "", 1.0f),
            makeChoice (bandParamId (b, "Slope"), "Band " + n + " slope", slopes, dsp::eq::kSlope12));
    }

    return layout;
}

Equalizer::BandSettings Equalizer::getBandSettings (int band) const noexcept
{
    const auto& p = raw[(size_t) juce::jlimit (0, kNumBands - 1, band)];

    BandSettings s;
    s.on = p.on->load() > 0.5f;
    s.type = (Type) juce::jlimit (0, kNumTypes - 1, juce::roundToInt (p.type->load()));
    s.freq = p.freq->load();
    s.gainDb = p.gain->load();
    s.q = p.q->load();
    s.slope = juce::jlimit (0, kNumSlopes - 1, juce::roundToInt (p.slope->load()));
    return s;
}

// ---------------------------------------------------------------------------------------------------
// Old states
// ---------------------------------------------------------------------------------------------------

bool Equalizer::migrateLegacyState (juce::ValueTree& state)
{
    std::map<juce::String, float> old;
    bool isLegacy = false;

    for (const auto& child : state)
    {
        const auto childId = child["id"].toString();

        for (const auto& lp : kLegacy)
            if (childId == lp.id)
            {
                old[childId] = (float) (double) child["value"];
                isLegacy = true;
            }
    }

    if (! isLegacy)
        return false;

    auto value = [&old] (const char* key)
    {
        if (auto it = old.find (key); it != old.end())
            return it->second;

        for (const auto& lp : kLegacy)
            if (juce::String (key) == lp.id)
                return lp.defaultValue;

        return 0.0f;
    };

    std::array<BandSettings, kNumBands> bandValues;

    for (int b = 0; b < kNumBands; ++b)
        bandValues[(size_t) b] = getDefaultBand (b);

    // The old shelves and bells were always in the path and simply flat at 0 dB: they are on when they do something.
    auto gainBand = [&] (int band, Type type, const char* freqId, const char* gainId, const char* qId)
    {
        auto& s = bandValues[(size_t) band];
        s.type = type;
        s.freq = value (freqId);
        s.gainDb = value (gainId);
        s.q = qId != nullptr ? value (qId) : kButterworthQ;
        s.on = std::abs (s.gainDb) > 1.0e-4f;
    };

    {
        auto& hpf = bandValues[0];
        hpf.type = Type::lowCut;
        hpf.on = value ("hpfOn") > 0.5f;
        hpf.freq = value ("hpfFreq");
        hpf.q = kButterworthQ;
        hpf.slope = value ("hpfSlope") > 0.5f ? dsp::eq::kSlopeLinkwitzRiley24 : dsp::eq::kSlope12;    // two identical 12 dB/oct stages
    }

    gainBand (1, Type::lowShelf, "lowShelfFreq", "lowShelfGain", nullptr);
    gainBand (2, Type::peak, "bell1Freq", "bell1Gain", "bell1Q");
    gainBand (3, Type::peak, "bell2Freq", "bell2Gain", "bell2Q");
    gainBand (4, Type::peak, "bell3Freq", "bell3Gain", "bell3Q");
    gainBand (5, Type::highShelf, "highShelfFreq", "highShelfGain", nullptr);

    {
        auto& lpf = bandValues[6];
        lpf.type = Type::highCut;
        lpf.on = value ("lpfOn") > 0.5f;
        lpf.freq = value ("lpfFreq");
        lpf.q = kButterworthQ;
    }

    state.removeAllChildren (nullptr);

    auto add = [&state] (const juce::String& paramId, float v)
    {
        juce::ValueTree param ("PARAM");
        param.setProperty ("id", paramId, nullptr);
        param.setProperty ("value", (double) v, nullptr);
        state.appendChild (param, nullptr);
    };

    for (int b = 0; b < kNumBands; ++b)
    {
        const auto& s = bandValues[(size_t) b];
        add (bandParamId (b, "On"), s.on ? 1.0f : 0.0f);
        add (bandParamId (b, "Type"), (float) (int) s.type);
        add (bandParamId (b, "Freq"), s.freq);
        add (bandParamId (b, "Gain"), s.gainDb);
        add (bandParamId (b, "Q"), s.q);
        add (bandParamId (b, "Slope"), (float) s.slope);
    }

    return true;
}

void Equalizer::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto tree = juce::ValueTree::fromXml (*xml);
            migrateLegacyState (tree);
            apvts.replaceState (tree);
        }
    }
}

// ---------------------------------------------------------------------------------------------------
// Response (message thread; from the same design function the DSP runs)
// ---------------------------------------------------------------------------------------------------

dsp::eq::Sections Equalizer::designFor (const BandSettings& s, double sampleRate) noexcept
{
    dsp::eq::BandParams p;
    p.type = s.type;
    p.freq = s.freq;
    p.gainDb = s.gainDb;
    p.q = s.q;
    p.slope = s.slope;
    return dsp::eq::design (p, sampleRate);
}

float Equalizer::getBandResponseDb (int band, double freq, double sr) const
{
    float out = 0.0f;
    const float f = (float) freq;
    getBandResponseDb (band, &f, &out, 1, sr);
    return out;
}

void Equalizer::getBandResponseDb (int band, const float* freqs, float* outDb, int numPoints, double sr) const
{
    const double rate = sr > 0.0 ? sr : getPreparedRate();
    const auto settings = getBandSettings (band);

    if (! settings.on)
    {
        std::fill (outDb, outDb + numPoints, 0.0f);
        return;
    }

    const auto sections = designFor (settings, rate);

    for (int i = 0; i < numPoints; ++i)
        outDb[i] = (float) dsp::eq::responseDb (sections, (double) freqs[i], rate);
}

float Equalizer::getResponseDb (double freq, double sr) const
{
    const double rate = sr > 0.0 ? sr : getPreparedRate();
    const auto point = dsp::eq::FreqPoint::fromHz (freq, rate);
    double db = 0.0;

    for (int b = 0; b < kNumBands; ++b)
    {
        const auto s = getBandSettings (b);

        if (s.on)
            db += dsp::eq::responseDb (designFor (s, rate), point);
    }

    return (float) juce::jmax (-120.0, db);
}

// ---------------------------------------------------------------------------------------------------
// DSP
// ---------------------------------------------------------------------------------------------------

void Equalizer::Voice::resetState() noexcept
{
    for (auto& s : state)
        for (auto& channel : s)
            channel.reset();
}

// A fresh start at the target values: coefficients built, filter memory cleared. The caller sets the crossfade.
void Equalizer::Voice::load (const BandSettings& t, double sampleRate) noexcept
{
    type = t.type;
    slope = t.slope;
    freq = t.freq;
    gainDb = t.gainDb;
    q = t.q;
    coeffs = designFor (t, sampleRate);
    resetState();
}

void Equalizer::prepareToPlay (double newSampleRate, int)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    preparedRate.store (sampleRate, std::memory_order_relaxed);
    smoothCoeff = (float) (1.0 - std::exp (-(double) kUpdateInterval / (sampleRate * kSmoothSeconds)));
    mixStep = (float) (1.0 / (sampleRate * kMixSeconds));
    needsSnap = true;
    rightStale = false;
    untilUpdate = 0;
}

void Equalizer::releaseResources()
{
}

// Moves every band one step towards its parameters. Runs on the audio thread every kUpdateInterval samples.
void Equalizer::updateBands (bool snap) noexcept
{
    auto approachLog = [this] (float current, float target) noexcept
    {
        const float d = std::log (target / current);
        return std::fabs (d) < 1.0e-4f ? target : current * std::exp (d * smoothCoeff);
    };

    auto approachLinear = [this] (float current, float target) noexcept
    {
        const float d = target - current;
        return std::fabs (d) < 0.002f ? target : current + d * smoothCoeff;
    };

    for (int b = 0; b < kNumBands; ++b)
    {
        auto& band = bands[(size_t) b];
        auto& a = band.voice[(size_t) band.active];
        auto& other = band.voice[(size_t) (1 - band.active)];
        const auto t = getBandSettings (b);

        if (snap)
        {
            a.load (t, sampleRate);
            a.mix = a.mixTarget = t.on ? 1.0f : 0.0f;
            other = Voice();
            continue;
        }

        if (! t.on)
        {
            a.mixTarget = 0.0f;                 // fades out, and stops costing anything once it is gone
            continue;
        }

        if (a.idle())
        {
            a.load (t, sampleRate);             // switched on: start at the current values, fade in
            a.mix = 0.0f;
            a.mixTarget = 1.0f;
            continue;
        }

        if (t.type != a.type || t.slope != a.slope)
        {
            // A different filter shape: crossfade to a fresh voice. If the last crossfade has not finished, wait for it.
            if (other.idle())
            {
                other.load (t, sampleRate);
                other.mix = 0.0f;
                other.mixTarget = 1.0f;
                a.mixTarget = 0.0f;
                band.active = 1 - band.active;
            }

            continue;
        }

        a.mixTarget = 1.0f;
        bool changed = false;

        if (a.freq != t.freq)       { a.freq = approachLog (a.freq, t.freq);            changed = true; }
        if (a.gainDb != t.gainDb)   { a.gainDb = approachLinear (a.gainDb, t.gainDb);   changed = true; }
        if (a.q != t.q)             { a.q = approachLog (a.q, t.q);                     changed = true; }

        if (changed)
        {
            BandSettings current = t;
            current.freq = a.freq;
            current.gainDb = a.gainDb;
            current.q = a.q;
            a.coeffs = designFor (current, sampleRate);
        }
    }

    updateSolo (snap);
}

// The solo filter is a band pass around a bell / notch / band pass, a low-pass for a low shelf or low cut and a
// high-pass for a high shelf or high cut. It follows the band's frequency and Q, and fades in and out.
void Equalizer::updateSolo (bool snap) noexcept
{
    const int want = soloBand.load (std::memory_order_relaxed);
    const bool valid = want >= 0 && want < kNumBands && getBandSettings (want).on;

    if (! valid || snap)
    {
        solo.mixTarget = 0.0f;

        if (snap || solo.idle())
        {
            solo.mix = solo.mixTarget = 0.0f;
            solo.resetState();
            soloWas = -1;
        }

        return;
    }

    const auto t = getBandSettings (want);

    BandSettings s;
    s.on = true;
    s.freq = t.freq;
    s.q = juce::jlimit (0.5f, 12.0f, t.q);
    s.slope = 2;                                    // 24 dB/oct for the low- and high-pass flavours

    switch (t.type)
    {
        case Type::lowShelf:
        case Type::lowCut:      s.type = Type::highCut; break;
        case Type::highShelf:
        case Type::highCut:     s.type = Type::lowCut; break;
        default:                s.type = Type::bandPass; s.q = juce::jlimit (0.7f, 12.0f, 1.4f * t.q); break;
    }

    if (soloWas != want || solo.idle())
    {
        solo.load (s, sampleRate);
        solo.mix = solo.idle() ? 0.0f : solo.mix;
        soloWas = want;
    }
    else
    {
        // Same band: glide to the new frequency and Q (a band moves under the mouse while it is soloed).
        const float d = std::log (s.freq / solo.freq);
        solo.freq = std::fabs (d) < 1.0e-4f ? s.freq : solo.freq * std::exp (d * smoothCoeff);
        const float dq = std::log (s.q / solo.q);
        solo.q = std::fabs (dq) < 1.0e-4f ? s.q : solo.q * std::exp (dq * smoothCoeff);

        s.freq = solo.freq;
        s.q = solo.q;
        solo.type = s.type;
        solo.coeffs = designFor (s, sampleRate);
    }

    // A band pass of this steepness is two of the sections.
    if (s.type == Type::bandPass)
    {
        solo.coeffs.s[1] = solo.coeffs.s[0];
        solo.coeffs.count = 2;
    }

    solo.mixTarget = 1.0f;
}

void Equalizer::copyLeftStateToRight() noexcept
{
    auto copy = [] (Voice& v)
    {
        for (auto& s : v.state)
            s[1] = s[0];
    };

    for (auto& band : bands)
        for (auto& v : band.voice)
            copy (v);

    copy (solo);
}

// Runs every section of one voice in place: two channels interleaved (their chains are independent, so the CPU
// overlaps them), or just the left one.
static void runSections (const dsp::eq::Sections& coeffs, dsp::BiquadState (&state)[dsp::eq::kMaxSections][2], float* l, float* r, int n) noexcept
{
    for (int s = 0; s < coeffs.count; ++s)
    {
        const auto& c = coeffs.s[(size_t) s];
        const double b0 = c.b0, b1 = c.b1, b2 = c.b2, a1 = c.a1, a2 = c.a2;
        double l1 = state[s][0].z1, l2 = state[s][0].z2;

        if (r != nullptr)
        {
            double r1 = state[s][1].z1, r2 = state[s][1].z2;

            for (int i = 0; i < n; ++i)
            {
                const double xl = (double) l[i], xr = (double) r[i];
                const double yl = b0 * xl + l1, yr = b0 * xr + r1;
                l1 = b1 * xl - a1 * yl + l2;
                l2 = b2 * xl - a2 * yl;
                r1 = b1 * xr - a1 * yr + r2;
                r2 = b2 * xr - a2 * yr;
                l[i] = (float) yl;
                r[i] = (float) yr;
            }

            state[s][1].z1 = r1;
            state[s][1].z2 = r2;
        }
        else
        {
            for (int i = 0; i < n; ++i)
            {
                const double xl = (double) l[i];
                const double yl = b0 * xl + l1;
                l1 = b1 * xl - a1 * yl + l2;
                l2 = b2 * xl - a2 * yl;
                l[i] = (float) yl;
            }
        }

        state[s][0].z1 = l1;
        state[s][0].z2 = l2;
    }
}

// Adds (filtered - dry) * mix of a voice that is fading in or out, sample by sample. `l` / `r` hold the running sum,
// `dry*` what the band received. The voice's own copy of the signal is filtered here.
void Equalizer::blendVoice (Voice& v, float* l, float* r, const float* dryL, const float* dryR, int n) noexcept
{
    float wetL[kUpdateInterval], wetR[kUpdateInterval];

    std::memcpy (wetL, dryL, (size_t) n * sizeof (float));

    if (r != nullptr)
        std::memcpy (wetR, dryR, (size_t) n * sizeof (float));

    runSections (v.coeffs, v.state, wetL, r != nullptr ? wetR : nullptr, n);

    for (int i = 0; i < n; ++i)
    {
        v.mix = v.mixTarget > v.mix ? juce::jmin (v.mixTarget, v.mix + mixStep)
                                    : juce::jmax (v.mixTarget, v.mix - mixStep);

        l[i] += (wetL[i] - dryL[i]) * v.mix;

        if (r != nullptr)
            r[i] += (wetR[i] - dryR[i]) * v.mix;
    }

    if (v.idle())
        v.resetState();             // gone: a voice that comes back later starts from a clean state (under its fade-in)
}

void Equalizer::runBand (Band& band, float* l, float* r, int n) noexcept
{
    auto& a = band.voice[(size_t) band.active];
    auto& other = band.voice[(size_t) (1 - band.active)];

    if (other.idle() && a.mix == a.mixTarget)
    {
        if (a.mix >= 1.0f)      { runSections (a.coeffs, a.state, l, r, n); return; }
        if (a.mix <= 0.0f)      return;
    }

    float dryL[kUpdateInterval], dryR[kUpdateInterval];
    std::memcpy (dryL, l, (size_t) n * sizeof (float));

    if (r != nullptr)
        std::memcpy (dryR, r, (size_t) n * sizeof (float));

    if (! other.idle())
        blendVoice (other, l, r, dryL, dryR, n);

    if (! a.idle())
        blendVoice (a, l, r, dryL, dryR, n);
}

void Equalizer::runSolo (float* l, float* r, int n) noexcept
{
    if (solo.idle())
        return;

    float dryL[kUpdateInterval], dryR[kUpdateInterval];
    std::memcpy (dryL, l, (size_t) n * sizeof (float));

    if (r != nullptr)
        std::memcpy (dryR, r, (size_t) n * sizeof (float));

    blendVoice (solo, l, r, dryL, dryR, n);
}

void Equalizer::processChunk (float* l, float* r, int n, bool identical) noexcept
{
    // Both channels identical (the mono mic): run one and copy it. The right channel's filter memory then lags, so
    // the next time the channels differ it starts from the left one's, which is exactly what it would have had.
    float* right = identical ? nullptr : r;

    if (right != nullptr && rightStale)
    {
        copyLeftStateToRight();
        rightStale = false;
    }

    if (identical && r != nullptr)
        rightStale = true;

    int pos = 0;

    while (pos < n)
    {
        if (untilUpdate <= 0)
        {
            updateBands (false);
            untilUpdate = kUpdateInterval;
        }

        const int len = juce::jmin (n - pos, untilUpdate);

        for (auto& band : bands)
            runBand (band, l + pos, right != nullptr ? right + pos : nullptr, len);

        runSolo (l + pos, right != nullptr ? right + pos : nullptr, len);

        pos += len;
        untilUpdate -= len;
    }

    if (identical && r != nullptr)
        std::memcpy (r, l, (size_t) n * sizeof (float));
}

void Equalizer::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int n = buffer.getNumSamples();
    const int numChannels = juce::jmin (buffer.getNumChannels(), 2);

    if (numChannels < 1 || n <= 0)
        return;

    float* l = buffer.getWritePointer (0);
    float* r = numChannels > 1 ? buffer.getWritePointer (1) : nullptr;

    if (needsSnap)
    {
        updateBands (true);
        needsSnap = false;
        untilUpdate = kUpdateInterval;
    }

    const bool analyse = analyzerActive.load (std::memory_order_relaxed);

    for (int start = 0; start < n; start += kChunk)
    {
        const int len = juce::jmin (kChunk, n - start);
        float* chunkL = l + start;
        float* chunkR = r != nullptr ? r + start : nullptr;

        if (analyse)
            sumToMono (chunkL, chunkR, len, analysisPre.data());

        const bool identical = chunkR == nullptr || std::memcmp (chunkL, chunkR, (size_t) len * sizeof (float)) == 0;
        processChunk (chunkL, chunkR, len, identical);

        if (analyse)
        {
            sumToMono (chunkL, chunkR, len, analysisPost.data());
            analyzerFifo.push (analysisPre.data(), analysisPost.data(), len);
        }
    }
}
}
