// Noise Removal: parameters, mode switching (background builder + cross-fade), blend, meters and "hear what's removed".
#include "engine/builtin/NoiseRemoval.h"

#include "app/Log.h"
#include "engine/denoise/CpuPlacement.h"
#include "engine/denoise/DfnEngine.h"
#include "engine/denoise/DfnRuntime.h"
#include "engine/denoise/RnnoiseEngine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <utility>

namespace hr
{
namespace
{
    constexpr double kMinRate = 8000.0, kMaxRate = 192000.0;
    constexpr double kFadeSeconds = 0.020;              // engine to engine, and the ramps of strength and "hear removed"
    constexpr double kPrimeSeconds = 0.060;             // a new engine sees live input this long (after its own latency) before it is heard
    constexpr float kLn10Over20 = 0.115129255f;

    // Speech protection (denoise::DfnEngine::Protection): after speech, what the network calls noise is not attenuated by more than
    // 9 dB (Best, 150 ms) or 12 dB (Fast, 100 ms). Best's network is the one that takes the weak ends of words for noise, so it gets
    // the stronger setting. Measured on real speech (tests/unit/SpeechCutTests.cpp): it takes the speech frames
    // cut by more than 6 dB at 5-10 dB signal-to-noise down by two thirds and costs 1-3 dB of noise removal in the pauses.
    constexpr denoise::DfnEngine::Protection kBestProtection { 0.0f, 9.0f, 15, 3.0f, 4 };
    constexpr denoise::DfnEngine::Protection kFastProtection { 0.0f, 12.0f, 10, 3.0f, 4 };

    constexpr int packProblem (int kind, int problem) noexcept  { return problem == 0 ? 0 : kind * 8 + problem; }
}

// ---------------------------------------------------------------------------------------------------
// Builder thread: loads models and builds engines so that none of it happens on the audio or message thread
// ---------------------------------------------------------------------------------------------------

class NoiseRemoval::Builder final : public juce::Thread
{
public:
    explicit Builder (NoiseRemoval& o) : juce::Thread ("hr-denoise-load"), owner (o) {}

    void run() override
    {
        // Loading a model is seconds of CPU on a cold start: keep it, like the worker, off the game's cores (CpuPlacement.h).
        denoise::cpu::pinCurrentThread();

        while (! threadShouldExit())
        {
            owner.builderStep();
            wait (100);         // woken early by notify(); otherwise it looks for retired engines to destroy every 100 ms
        }
    }

private:
    NoiseRemoval& owner;
};

// ---------------------------------------------------------------------------------------------------

NoiseRemoval::NoiseRemoval()
    : BuiltinProcessor (kId, "Noise Removal", createLayout())
{
    pMode = apvts.getRawParameterValue ("mode");
    pStrength = apvts.getRawParameterValue ("strength");
    pThreshold = apvts.getRawParameterValue ("threshold");
    pHold = apvts.getRawParameterValue ("hold");
    pPostFilter = apvts.getRawParameterValue ("postFilter");

    apvts.addParameterListener ("mode", this);
}

NoiseRemoval::~NoiseRemoval()
{
    apvts.removeParameterListener ("mode", this);

    // (No call to onHearRemovedRequest here: the panel lets go when it goes away, the app on quit, and after 30 s in any case.
    // The app may be in the middle of erasing this slot.)
    if (builder != nullptr)
    {
        builder->signalThreadShouldExit();
        builder->notify();
        builder->stopThread (60000);        // a model that is loading has to finish first
        builder.reset();
    }

    discardChains();

    while (auto** slot = retired.peek())
    {
        delete *slot;
        retired.pop();
    }
}

BuiltinProcessor::Layout NoiseRemoval::createLayout()
{
    Layout layout;

    // "mode" and "postFilter" are new: a saved state without them loads their defaults (Best, post-filter off), and
    // "strength" (the old wet/dry blend) is kept, now read as the attenuation limit. The choice indices are what a
    // saved state stores, so their order must not change; only the default moves (Fast was the default up to 0.2.1).
    layout.add (
        makeChoice ("mode", "Mode", { "Fast", "Best", "Light" }, (int) Mode::best),
        makeFloat ("strength", "Strength", 0.0f, 100.0f, 80.0f, "%"),
        makeFloat ("threshold", "Voice threshold", 0.0f, 95.0f, 60.0f, "%"),
        makeFloat ("hold", "Hold", 0.0f, 500.0f, 150.0f, "ms"),
        makeFloat ("postFilter", "Post-filter", 0.0f, 100.0f, 0.0f, "%"));

    return layout;
}

// ---- The setting -----------------------------------------------------------------------------------

float NoiseRemoval::attenuationDbForStrength (float strengthPercent) noexcept
{
    return juce::jlimit (0.0f, 100.0f, strengthPercent) * 0.01f * kMaxAttenuationDb;
}

float NoiseRemoval::dryMixForStrength (float strengthPercent) noexcept
{
    return std::exp (-attenuationDbForStrength (strengthPercent) * kLn10Over20);
}

NoiseRemoval::Mode NoiseRemoval::getRequestedMode() const noexcept
{
    return (Mode) juce::jlimit (0, 2, (int) std::lround (pMode->load()));
}

const char* NoiseRemoval::getModeName (Mode mode) noexcept
{
    switch (mode)
    {
        case Mode::fast:    return "Fast";
        case Mode::best:    return "Best";
        case Mode::light:   break;
    }

    return "Light";
}

int NoiseRemoval::getNominalLatencySamples (Mode mode) noexcept
{
    switch (mode)
    {
        case Mode::fast:    return 3 * kFrame;              // 480 collecting + 480 answer + 480 model
        case Mode::best:    return 5 * kFrame;              // ... + 1440 model
        case Mode::light:   break;
    }

    return kNativeLatency;
}

NoiseRemoval::Chain::Kind NoiseRemoval::kindFor (Mode mode) noexcept
{
    switch (mode)
    {
        case Mode::fast:    return Chain::Kind::fast;
        case Mode::best:    return Chain::Kind::best;
        case Mode::light:   break;
    }

    return Chain::Kind::light;
}

NoiseRemoval::Mode NoiseRemoval::modeFor (Chain::Kind kind) noexcept
{
    switch (kind)
    {
        case Chain::Kind::fast:     return Mode::fast;
        case Chain::Kind::best:     return Mode::best;
        case Chain::Kind::light:    break;
    }

    return Mode::light;
}

// ---- State of the engine ------------------------------------------------------------------------------

NoiseRemoval::Mode NoiseRemoval::getActiveMode() const noexcept
{
    return modeFor ((Chain::Kind) activeKind.load (std::memory_order_relaxed));
}

NoiseRemoval::Status NoiseRemoval::getStatus() const noexcept
{
    const auto wanted = kindFor (getRequestedMode());

    if (getRateMode() == RateMode::unsupported || (Chain::Kind) activeKind.load (std::memory_order_relaxed) == wanted)
        return Status::ready;

    const auto state = problemState.load (std::memory_order_relaxed);
    return state != 0 && state / 8 == (int) wanted ? Status::unavailable : Status::loading;
}

NoiseRemoval::Problem NoiseRemoval::getProblem() const noexcept
{
    const auto state = problemState.load (std::memory_order_relaxed);
    return state != 0 && state / 8 == (int) kindFor (getRequestedMode()) ? (Problem) (state % 8) : Problem::none;
}

juce::String NoiseRemoval::getStatusNote() const
{
    switch (getStatus())
    {
        case Status::ready:         return {};
        case Status::loading:       return juce::String (getModeName (getRequestedMode())) + " is getting ready. Light is running until then.";
        case Status::unavailable:   break;
    }

    switch (getProblem())
    {
        case Problem::notCompiledIn:    return "Fast and Best are not part of this build of Headroom. Using Light.";
        case Problem::modelsMissing:    return "The files for Fast and Best are missing. Using Light.";
        case Problem::loadFailed:       return "Fast and Best could not be loaded. Using Light.";
        case Problem::none:             break;
    }

    return {};
}

bool NoiseRemoval::isSwitching() const noexcept
{
    return pending.load (std::memory_order_relaxed) != nullptr || phase != Phase::steady;
}

// ---- Building chains -----------------------------------------------------------------------------------

std::unique_ptr<NoiseRemoval::Chain> NoiseRemoval::makeChain (Chain::Kind kind, double rate, Problem& problem, juce::String& detail)
{
    problem = Problem::none;
    std::unique_ptr<denoise::Denoiser48> engine;

    if (kind == Chain::Kind::light)
    {
        engine = denoise::RnnoiseEngine::create();

        if (engine == nullptr)
        {
            problem = Problem::loadFailed;
            detail = "RNNoise could not allocate its state";
        }
    }
    else
    {
        const auto model = kind == Chain::Kind::fast ? dfn::Model::fast : dfn::Model::best;

        if (! dfn::isCompiledIn())
        {
            problem = Problem::notCompiledIn;
        }
        else if (dfn::findModelFile (model) == juce::File())
        {
            problem = Problem::modelsMissing;
            detail = dfn::getModelsFolder().getChildFile (dfn::fileName (model)).getFullPathName();
        }
        else if (auto loaded = dfn::Models::get (model, detail))
        {
            auto dfn = denoise::DfnEngine::create (std::move (loaded), detail);

            if (dfn != nullptr)
                dfn->setProtection (kind == Chain::Kind::best ? kBestProtection : kFastProtection);

            engine = std::move (dfn);

            if (engine == nullptr)
                problem = Problem::loadFailed;
        }
        else
        {
            problem = Problem::loadFailed;
        }
    }

    if (engine == nullptr)
        return nullptr;

    return std::make_unique<Chain> (kind, std::move (engine), rate);
}

void NoiseRemoval::recordProblem (Chain::Kind kind, Problem problem) noexcept
{
    problemState.store (packProblem ((int) kind, (int) problem), std::memory_order_relaxed);
}

// Called by the builder thread: retire what the audio thread is done with, apply the latency, build what was asked for.
void NoiseRemoval::builderStep()
{
    while (auto** slot = retired.peek())
    {
        std::unique_ptr<Chain> gone (*slot);
        retired.pop();
    }

    if (latencyDirty.exchange (false))
        setLatencySamples (activeLatency.load());

    Chain::Kind kind;
    double rate;
    uint64_t wanted;

    {
        const std::lock_guard<std::mutex> lock (requestMutex);

        if (! buildWanted)
            return;

        buildWanted = false;
        kind = wantedKind;
        rate = wantedRate;
        wanted = generation;

        if (kind == lastPublishedKind)
            return;             // what is running (or on its way) already is what is wanted
    }

    Problem problem = Problem::none;
    juce::String detail;
    auto chain = makeChain (kind, rate, problem, detail);

    if (chain != nullptr)
    {
        recordProblem (kind, Problem::none);
    }
    else
    {
        recordProblem (kind, problem);
        Log::warn ("Noise removal: " + juce::String (getModeName (modeFor (kind))) + " is not available"
                   + (detail.isNotEmpty() ? " (" + detail + ")" : juce::String()) + "; using Light");

        // Fall back to Light unless it is what runs already.
        Problem ignored = Problem::none;

        {
            const std::lock_guard<std::mutex> lock (requestMutex);

            if (lastPublishedKind == Chain::Kind::light)
                return;
        }

        chain = makeChain (Chain::Kind::light, rate, ignored, detail);
    }

    if (chain == nullptr)
        return;

    {
        const std::lock_guard<std::mutex> lock (requestMutex);

        if (wanted != generation)
            return;             // prepared again (a new rate) while this was being built: it no longer fits

        lastPublishedKind = chain->kind();
    }

    std::unique_ptr<Chain> replaced (pending.exchange (chain.release(), std::memory_order_acq_rel));
}

void NoiseRemoval::requestBuild()
{
    if (sampleRate.load() <= 0.0)
        return;

    {
        const std::lock_guard<std::mutex> lock (requestMutex);
        buildWanted = true;
        wantedKind = kindFor (getRequestedMode());
        wantedRate = sampleRate.load();
    }

    if (builder == nullptr)
    {
        builder = std::make_unique<Builder> (*this);
        builder->startThread (juce::Thread::Priority::low);
    }
    else
    {
        builder->notify();
    }
}

void NoiseRemoval::parameterChanged (const juce::String& parameterId, float)
{
    if (parameterId == "mode" && sampleRate.load() > 0.0 && getRateMode() != RateMode::unsupported)
        requestBuild();
}

// ---- Message thread: prepare / release -----------------------------------------------------------------

void NoiseRemoval::discardChains()
{
    std::unique_ptr<Chain> a (std::exchange (active, nullptr));
    std::unique_ptr<Chain> b (std::exchange (incoming, nullptr));
    std::unique_ptr<Chain> c (std::exchange (stuck, nullptr));
    std::unique_ptr<Chain> d (pending.exchange (nullptr, std::memory_order_acq_rel));
    phase = Phase::steady;
}

void NoiseRemoval::prepareToPlay (double newSampleRate, int)
{
    JUCE_ASSERT_MESSAGE_THREAD

    // Whatever the builder is making now was meant for the old rate.
    {
        const std::lock_guard<std::mutex> lock (requestMutex);
        ++generation;
        buildWanted = false;
    }

    discardChains();
    sampleRate.store (newSampleRate);

    const double rate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    strength.reset (rate, kFadeSeconds);
    hearMix.reset (rate, kFadeSeconds);
    meters.prepare (rate);
    tap.sampleRate.store (rate);
    needsSnap = true;
    cpuSmoothed = 0.0f;
    cpuAudio.store (0.0f);
    cpuWorker.store (0.0f);
    worstFrame.store (0.0f);
    missed.store (0);
    underruns.store (0);
    gateLevel.store (1.0f);
    recordProblem (Chain::Kind::light, Problem::none);

    if (rate < kMinRate || rate > kMaxRate)
    {
        rateMode.store (RateMode::unsupported, std::memory_order_relaxed);
        activeLatency.store (0);
        setLatencySamples (0);
        return;
    }

    rateMode.store (std::abs (rate - 48000.0) < 0.5 ? RateMode::native : RateMode::resampled, std::memory_order_relaxed);

    // The engine that was asked for if it can start right now (Light, or a network that is loaded already),
    // otherwise Light in its place while the builder prepares the other one.
    const auto wanted = kindFor (getRequestedMode());
    Problem problem = Problem::none;
    juce::String detail;
    std::unique_ptr<Chain> chain;

    if (wanted == Chain::Kind::light)
    {
        chain = makeChain (wanted, rate, problem, detail);
    }
    else if (auto cached = dfn::Models::peek (wanted == Chain::Kind::fast ? dfn::Model::fast : dfn::Model::best);
             cached != nullptr && cached->isInstant())
    {
        chain = makeChain (wanted, rate, problem, detail);
    }
    else if (dfn::isCompiledIn() && dfn::findModelFile (wanted == Chain::Kind::fast ? dfn::Model::fast : dfn::Model::best) == juce::File())
    {
        problem = Problem::modelsMissing;           // no point in waiting for the builder to find that out
    }

    if (chain == nullptr && problem != Problem::none)
        recordProblem (wanted, problem);            // e.g. the files are gone: the panel can say so right away

    if (chain == nullptr)
    {
        Problem placeholderProblem = Problem::none;
        chain = makeChain (Chain::Kind::light, rate, placeholderProblem, detail);
    }

    jassert (chain != nullptr);

    if (chain == nullptr)
    {
        rateMode.store (RateMode::unsupported, std::memory_order_relaxed);
        setLatencySamples (0);
        return;
    }

    {
        const std::lock_guard<std::mutex> lock (requestMutex);
        lastPublishedKind = chain->kind();
    }

    active = chain.release();
    activeKind.store ((int) active->kind());
    activeLatency.store (active->getLatency());
    setLatencySamples (active->getLatency());

    if (active->kind() != wanted)
        requestBuild();
}

void NoiseRemoval::releaseResources()
{
    {
        const std::lock_guard<std::mutex> lock (requestMutex);
        ++generation;
        buildWanted = false;
    }

    discardChains();
    sampleRate.store (0.0);
}

// The audio thread is not running (tests, before audio starts): take a finished engine at once, no cross-fade.
void NoiseRemoval::adoptPendingWhileIdle()
{
    std::unique_ptr<Chain> next (pending.exchange (nullptr, std::memory_order_acq_rel));

    if (next == nullptr || next->getHostRate() != sampleRate.load())
        return;

    std::unique_ptr<Chain> old (std::exchange (active, next.release()));
    phase = Phase::steady;
    publishActive();
    setLatencySamples (active->getLatency());
    latencyDirty.store (false);
}

bool NoiseRemoval::waitUntilReady (int timeoutMs)
{
    const auto end = juce::Time::getMillisecondCounter() + (uint32_t) std::max (0, timeoutMs);

    while (true)
    {
        adoptPendingWhileIdle();

        if (const auto status = getStatus(); status != Status::loading)
            return status == Status::ready;

        if (juce::Time::getMillisecondCounter() >= end)
            return false;

        juce::Thread::sleep (5);
    }
}

void NoiseRemoval::requestHearRemoved (bool on)
{
    if (on == isHearingRemoved())
        return;

    if (onHearRemovedRequest)
        onHearRemovedRequest (on);

    setHearRemoved (on);
}

// ---- Audio thread ----------------------------------------------------------------------------------------

void NoiseRemoval::publishActive() noexcept
{
    activeKind.store ((int) active->kind(), std::memory_order_relaxed);
    activeLatency.store (active->getLatency(), std::memory_order_relaxed);
}

void NoiseRemoval::retire (Chain* chain) noexcept
{
    if (auto** slot = retired.claim())
    {
        *slot = chain;
        retired.publish();
    }
    else
    {
        stuck = chain;              // the builder is behind (it is not, in practice): keep it and try again next block
    }
}

void NoiseRemoval::adoptPending() noexcept
{
    if (auto* next = pending.exchange (nullptr, std::memory_order_acq_rel))
    {
        if (next->getHostRate() != sampleRate.load (std::memory_order_relaxed))
        {
            retire (next);          // built for a rate we have left
            return;
        }

        const double rate = next->getHostRate();
        incoming = next;
        phase = Phase::priming;
        primeExtra = (int) (kPrimeSeconds * rate);
        phaseLeft = next->getLatency() + primeExtra;
        fadeLength = std::max (1, (int) (kFadeSeconds * rate));
        fadeDone = 0;
    }
}

void NoiseRemoval::finishTransition() noexcept
{
    retire (active);
    active = incoming;
    incoming = nullptr;
    phase = Phase::steady;
    publishActive();
    latencyDirty.store (true, std::memory_order_release);
}

void NoiseRemoval::processChunk (float* l, float* r, int n) noexcept
{
    const bool stereo = r != nullptr && (forceStereo.load (std::memory_order_relaxed)
                                         || std::memcmp (l, r, (size_t) n * sizeof (float)) != 0);
    const int lanes = stereo ? 2 : 1;
    const float* in[2] { l, stereo ? r : l };

    if (stuck != nullptr)
    {
        auto* again = std::exchange (stuck, nullptr);
        retire (again);
    }

    if (incoming == nullptr)
        adoptPending();

    // Strength: the share of the aligned input that stays in the output. It moves over 20 ms.
    const float* ramp = nullptr;

    if (strength.isSmoothing())
    {
        for (int i = 0; i < n; ++i)
            mixRamp[(size_t) i] = std::exp (-strength.getNextValue() * kMaxAttenuationDb * kLn10Over20);

        ramp = mixRamp.data();
    }

    const float dryMix = std::exp (-strength.getTargetValue() * kMaxAttenuationDb * kLn10Over20);

    float* outAp[2] { outA[0].data(), outA[1].data() };
    float* dryAp[2] { dryA[0].data(), dryA[1].data() };

    active->process (in, outAp, dryAp, lanes, n, ramp, dryMix);

    if (incoming != nullptr)
    {
        float* outBp[2] { outB[0].data(), outB[1].data() };
        float* dryBp[2] { dryB[0].data(), dryB[1].data() };
        incoming->process (in, outBp, dryBp, lanes, n, ramp, dryMix);

        if (phase == Phase::priming)
        {
            phaseLeft -= n;

            if (phaseLeft <= 0)
            {
                phase = Phase::fading;
                fadeDone = 0;
            }
        }
        else
        {
            const float step = 1.0f / (float) fadeLength;

            for (int c = 0; c < lanes; ++c)
            {
                for (int i = 0; i < n; ++i)
                {
                    const float t = std::min (1.0f, (float) (fadeDone + i) * step);
                    outA[(size_t) c][(size_t) i] += (outB[(size_t) c][(size_t) i] - outA[(size_t) c][(size_t) i]) * t;
                    dryA[(size_t) c][(size_t) i] += (dryB[(size_t) c][(size_t) i] - dryA[(size_t) c][(size_t) i]) * t;
                }
            }

            fadeDone += n;

            if (fadeDone >= fadeLength)
                finishTransition();
        }
    }

    // Meters and the spectrum see what the effect makes of the input, whether or not it is being listened to.
    auto& engine = active->getEngine();
    meters.update (dryA[0].data(), outA[0].data(), n, engine.voice());
    tap.push (dryA[0].data(), outA[0].data(), n);
    gateLevel.store (engine.gateLevel(), std::memory_order_relaxed);

    // "Hear what's removed": input minus output, faded in and out over 20 ms.
    if (hearMix.isSmoothing() || hearMix.getTargetValue() > 0.5f)
    {
        const bool moving = hearMix.isSmoothing();

        if (moving)
            for (int i = 0; i < n; ++i)
                hearRamp[(size_t) i] = hearMix.getNextValue();

        for (int c = 0; c < lanes; ++c)
        {
            auto& o = outA[(size_t) c];
            const auto& d = dryA[(size_t) c];

            for (int i = 0; i < n; ++i)
            {
                const float m = moving ? hearRamp[(size_t) i] : 1.0f;
                o[(size_t) i] += (d[(size_t) i] - 2.0f * o[(size_t) i]) * m;      // out + (removed - out) * m, removed = dry - out
            }
        }
    }

    std::memcpy (l, outA[0].data(), (size_t) n * sizeof (float));

    if (r != nullptr)
        std::memcpy (r, outA[(size_t) (stereo ? 1 : 0)].data(), (size_t) n * sizeof (float));

    underruns.store (active->getUnderruns(), std::memory_order_relaxed);
}

void NoiseRemoval::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    if (active == nullptr)
        return;                                                     // untouched pass-through (rate not supported, or not prepared)

    const int n = buffer.getNumSamples();
    const int numChannels = juce::jmin (buffer.getNumChannels(), 2);

    if (numChannels < 1 || n <= 0)
        return;

    const auto started = std::chrono::steady_clock::now();

    const float strengthNow = pStrength->load() * 0.01f;
    const float hearNow = hearRemoved.load (std::memory_order_relaxed) ? 1.0f : 0.0f;

    if (needsSnap)
    {
        strength.setCurrentAndTargetValue (strengthNow);
        hearMix.setCurrentAndTargetValue (hearNow);
        needsSnap = false;
    }
    else
    {
        strength.setTargetValue (strengthNow);
        hearMix.setTargetValue (hearNow);
    }

    // Light's voice gate and DeepFilterNet's post-filter are parameters of the engine that runs.
    const float threshold = pThreshold->load() * 0.01f;
    const int holdFrames = (int) std::ceil ((double) pHold->load() / 10.0) + kGateLeadFrames;
    const float beta = pPostFilter->load() * 0.01f * kMaxPostFilterBeta;

    for (auto* chain : { active, incoming })
    {
        if (chain != nullptr)
        {
            chain->getEngine().setGate (threshold, holdFrames);
            chain->getEngine().setPostFilter (beta);
        }
    }

    float* l = buffer.getWritePointer (0);
    float* r = numChannels > 1 ? buffer.getWritePointer (1) : nullptr;

    for (int offset = 0; offset < n; offset += kChunk)
        processChunk (l + offset, r != nullptr ? r + offset : nullptr, juce::jmin (kChunk, n - offset));

    // CPU share: this thread's time for the block over the block's duration, smoothed (~0.5 s).
    const auto took = std::chrono::duration<double> (std::chrono::steady_clock::now() - started).count();
    const double rate = sampleRate.load (std::memory_order_relaxed);

    if (rate > 0.0)
    {
        const float share = (float) (took / ((double) n / rate));
        cpuSmoothed += (float) (1.0 - std::exp (-(double) n / (0.5 * rate))) * (share - cpuSmoothed);
        cpuAudio.store (cpuSmoothed, std::memory_order_relaxed);
    }

    auto& engine = active->getEngine();
    cpuWorker.store (engine.workerLoad(), std::memory_order_relaxed);
    worstFrame.store (engine.worstFrameMs(), std::memory_order_relaxed);
    missed.store (engine.misses(), std::memory_order_relaxed);
}
}
