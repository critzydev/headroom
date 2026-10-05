// FxSlot layout negotiation / RT processing (crash tracking, sanitising, bypass crossfade) and FxChain iteration.
#include "engine/FxChain.h"
#include "engine/dsp/Sanitise.h"
#include "plugins/CrashGuard.h"

#include <algorithm>

namespace hr
{
namespace
{
    constexpr double kWarmupSeconds = 0.040;        // extra feeding time on top of the processor's latency
    constexpr double kMaxWarmupSeconds = 0.500;
    constexpr double kBypassRampSeconds = 0.010;
}

FxSlot::FxSlot (std::shared_ptr<juce::AudioProcessor> processorToWrap, juce::String slotUid, uint32_t slotCrashId)
    : processor (std::move (processorToWrap)), uid (std::move (slotUid)), crashId (slotCrashId)
{
    jassert (processor != nullptr);
}

void FxSlot::negotiateLayout()
{
    auto& p = *processor;

    // Order matters: buses first, then rate/size details, then prepare.
    p.disableNonMainBuses();

    auto tryLayout = [&p] (const juce::AudioChannelSet& in, const juce::AudioChannelSet& out)
    {
        auto layout = p.getBusesLayout();

        if (p.getBusCount (true) > 0)   layout.getChannelSet (true, 0)  = in;
        if (p.getBusCount (false) > 0)  layout.getChannelSet (false, 0) = out;

        return p.checkBusesLayoutSupported (layout) && p.setBusesLayout (layout);
    };

    const auto stereo = juce::AudioChannelSet::stereo();
    const auto mono = juce::AudioChannelSet::mono();

    if (! tryLayout (stereo, stereo))
        if (! tryLayout (mono, stereo))
            tryLayout (mono, mono);     // if even this fails we keep whatever the plugin defaults to
}

void FxSlot::configure (double sampleRate, int maxBlock)
{
    JUCE_ASSERT_MESSAGE_THREAD

    auto& p = *processor;

    if (configured)
        p.releaseResources();
    else
        negotiateLayout();

    p.setNonRealtime (false);
    p.setRateAndBufferSizeDetails (sampleRate, maxBlock);
    p.prepareToPlay (sampleRate, maxBlock);

    numIn = p.getTotalNumInputChannels();
    numOut = p.getTotalNumOutputChannels();

    work.setSize (juce::jmax (numIn, numOut, 2), maxBlock, false, true, false);
    work.clear();
    midi.ensureSize (4096);
    midi.clear();

    preparedRate = sampleRate;
    warmupSamples = computeWarmupSamples();
    rampStep = (float) (1.0 / juce::jmax (1.0, kBypassRampSeconds * sampleRate));

    wetMix = 0.0f;
    warm = false;
    warmupLeft = 0;
    faulted.store (false, std::memory_order_relaxed);

    configured = true;
}

// Warm-up = latency + 40 ms (capped): long enough for latency effects to emit real audio and for envelope
// followers to converge before the crossfade starts. Looked up again at every switch-on, because a processor may
// change its latency while it runs (Noise Removal does when its mode changes).
int FxSlot::computeWarmupSamples() const noexcept
{
    const double seconds = juce::jmin (kMaxWarmupSeconds,
                                       (double) juce::jmax (0, processor->getLatencySamples()) / preparedRate + kWarmupSeconds);
    return (int) (seconds * preparedRate);
}

void FxSlot::process (float* l, float* r, int n) noexcept
{
    // numIn == 0: a synth or generator. It has nothing to process, and its (silent, without MIDI) output would replace
    // the channel's signal, so leave the signal alone. PluginManager::createInstance refuses these; this is the backstop.
    // Bypass is handled in processChunk (it crossfades).
    if (! configured || numIn == 0)
        return;

    const int capacity = work.getNumSamples();

    while (n > 0 && capacity > 0)
    {
        const int chunk = juce::jmin (n, capacity);
        processChunk (l, r, chunk);
        l += chunk;
        r += chunk;
        n -= chunk;
    }
}

void FxSlot::processChunk (float* l, float* r, int n) noexcept
{
    if (faulted.load (std::memory_order_relaxed))
        return;

    const bool wantWet = ! bypassed.load (std::memory_order_relaxed);

    if (! wantWet && wetMix <= 0.0f)
    {
        warm = false;           // fully bypassed: free, and the next switch-on starts with a warm-up
        return;
    }

    if (wantWet && ! warm)
    {
        warm = true;
        warmupSamples = computeWarmupSamples();
        warmupLeft = warmupSamples;
    }

    auto* const* w = work.getArrayOfWritePointers();
    const int numChannels = work.getNumChannels();

    if (numIn >= 1)  juce::FloatVectorOperations::copy (w[0], l, n);
    if (numIn >= 2)  juce::FloatVectorOperations::copy (w[1], r, n);

    for (int c = juce::jmin (numIn, 2); c < numChannels; ++c)
        juce::FloatVectorOperations::clear (w[c], n);

    // A view over preallocated memory: no allocation for <= 32 channels.
    juce::AudioBuffer<float> view (w, juce::jmax (numIn, numOut, 1), n);
    midi.clear();

    CrashGuard::currentRtSlot.store (crashId, std::memory_order_relaxed);

    try
    {
        processor->processBlock (view, midi);
    }
    catch (...)
    {
        // A throwing plugin is taken out of the signal path for good instead of taking the app down.
        CrashGuard::currentRtSlot.store (0, std::memory_order_relaxed);
        faulted.store (true, std::memory_order_relaxed);
        wetMix = 0.0f;
        warm = false;
        return;
    }

    CrashGuard::currentRtSlot.store (0, std::memory_order_relaxed);

    if (numOut < 1)
        return;

    bool changed = dsp::sanitise (w[0], n);

    if (numOut >= 2)
        changed = dsp::sanitise (w[1], n) || changed;

    if (changed)
        sanitisedBlocks.fetch_add (1, std::memory_order_relaxed);

    if (wantWet && warmupLeft > 0)
    {
        warmupLeft -= n;        // still warming up: the processed audio is discarded, the dry signal plays on
        return;
    }

    const float* wetL = w[0];
    const float* wetR = numOut >= 2 ? w[1] : w[0];        // mono processor: duplicate
    const float target = wantWet ? 1.0f : 0.0f;

    if (wetMix == target)
    {
        juce::FloatVectorOperations::copy (l, wetL, n);
        juce::FloatVectorOperations::copy (r, wetR, n);
        return;
    }

    for (int i = 0; i < n; ++i)
    {
        wetMix = wantWet ? juce::jmin (1.0f, wetMix + rampStep) : juce::jmax (0.0f, wetMix - rampStep);

        l[i] += (wetL[i] - l[i]) * wetMix;
        r[i] += (wetR[i] - r[i]) * wetMix;
    }

    if (! wantWet && wetMix <= 0.0f)
        warm = false;
}

void FxChain::process (float* l, float* r, int numFrames) noexcept
{
    for (const auto& slot : slots)
        slot->process (l, r, numFrames);
}

void FxChain::reprepare (double sampleRate)
{
    for (const auto& slot : slots)
        slot->configure (sampleRate, kMaxBlock);
}
}
