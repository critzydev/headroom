// AudioEngine: chunking, mic watch, channel summing, master stage and message-thread control.
#include "engine/AudioEngine.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace hr
{
namespace
{
    constexpr float kSilenceThreshold = 1.0e-9f;    // the mic watch treats a buffer as "exactly zero" below this

    // True if any sample is not (almost) exactly zero. NaN counts as signal: a broken device is not the
    // capture-order firmware bug the watchdog exists for.
    bool hasSignal (const float* data, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
            if (! (std::fabs (data[i]) < kSilenceThreshold))
                return true;

        return false;
    }

    // Marks the RT thread as inside process() for the whole call, whichever way it returns.
    struct BusyScope
    {
        explicit BusyScope (std::atomic<bool>& flag) noexcept : busy (flag)    { busy.store (true); }
        ~BusyScope() noexcept                                                   { busy.store (false); }

        BusyScope (const BusyScope&) = delete;
        BusyScope& operator= (const BusyScope&) = delete;

        std::atomic<bool>& busy;
    };
}

AudioEngine::AudioEngine()
{
    for (auto& m : masters)
        m.limiter.setCeilingDb (-0.3f);

    // The mic is always present; app slots are activated as channels are created.
    channels[(size_t) kMicChannel].setActive (true);
}

AudioEngine::~AudioEngine()
{
    // The backend must have been stopped first, so nothing is inside process() any more.
    for (auto& c : channels)
        c.exchangeChain (nullptr);

    retired.clear();
    currentChains = {};
}

ChannelProcessor& AudioEngine::channelAt (int index) noexcept
{
    jassert (juce::isPositiveAndBelow (index, kNumChannels));
    return channels[(size_t) juce::jlimit (0, kNumChannels - 1, index)];
}

// ---------------------------------------------------------------------------------------------------
// RT thread
// ---------------------------------------------------------------------------------------------------

void AudioEngine::writeSilence (const EngineIO& io) noexcept
{
    float* const* groups[] { io.monitorOut, io.streamOut, io.micOut };

    for (auto* group : groups)
        for (int c = 0; c < 2; ++c)
            if (group[c] != nullptr)
                juce::FloatVectorOperations::clear (group[c], io.numFrames);
}

void AudioEngine::updateMicWatch (const EngineIO& io) noexcept
{
    if (micResetRequested.exchange (false))
    {
        micSilentSamples = 0;
        micSilentMs.store (0, std::memory_order_relaxed);
        micSeenSignal.store (0, std::memory_order_relaxed);
        micLiveCycles.store (0, std::memory_order_relaxed);
    }

    if (io.micIn[0] == nullptr)
    {
        // Not linked (yet): nothing to judge, so don't let the watchdog count this as silence.
        micLiveCycles.store (0, std::memory_order_relaxed);
        micSilentSamples = 0;
        micSilentMs.store (0, std::memory_order_relaxed);
        return;
    }

    micLiveCycles.fetch_add (1, std::memory_order_relaxed);

    const bool signal = hasSignal (io.micIn[0], io.numFrames)
                        || (io.micIn[1] != nullptr && hasSignal (io.micIn[1], io.numFrames));

    if (signal)
    {
        micSeenSignal.store (1, std::memory_order_relaxed);
        micSilentSamples = 0;
        micSilentMs.store (0, std::memory_order_relaxed);
    }
    else
    {
        micSilentSamples += (uint64_t) io.numFrames;

        const auto rate = io.sampleRate > 0.0 ? io.sampleRate : 48000.0;
        const auto ms = (double) micSilentSamples * 1000.0 / rate;
        micSilentMs.store ((uint32_t) juce::jmin (ms, 4.0e9), std::memory_order_relaxed);
    }
}

void AudioEngine::updateInputWatch (const EngineIO& io) noexcept
{
    for (int k = 0; k < kMaxInputChannels; ++k)
    {
        auto& live = inputLiveCycles[(size_t) k];

        if (io.inputIn[k][0] != nullptr || io.inputIn[k][1] != nullptr)
            live.fetch_add (1, std::memory_order_relaxed);
        else if (live.load (std::memory_order_relaxed) != 0)
            live.store (0, std::memory_order_relaxed);
    }
}

void AudioEngine::process (const EngineIO& io) noexcept
{
    juce::ScopedNoDenormals noDenormals;

    // Both of these are sequentially consistent on purpose: see "Why snapshots are never freed" below.
    const BusyScope busyScope (rtBusy);
    cycleCounter.fetch_add (1);

    if (io.numFrames <= 0)
        return;

    updateMicWatch (io);
    updateInputWatch (io);

    if (suspendFx.load())
    {
        writeSilence (io);
        suspendAck.fetch_add (1);
        return;
    }

    if (std::abs (io.sampleRate - preparedRate.load (std::memory_order_acquire)) > 0.5)
    {
        writeSilence (io);
        requestedRate.store (io.sampleRate, std::memory_order_relaxed);
        return;
    }

    for (int offset = 0; offset < io.numFrames; offset += kMaxBlock)
    {
        EngineIO chunk = io;
        chunk.numFrames = juce::jmin (kMaxBlock, io.numFrames - offset);

        auto advance = [offset] (auto& ptr)
        {
            if (ptr != nullptr)
                ptr += offset;
        };

        for (auto& pair : chunk.appIn)  for (auto& p : pair)    advance (p);
        for (auto& p : chunk.micIn)                             advance (p);
        for (auto& pair : chunk.inputIn)  for (auto& p : pair)  advance (p);
        for (auto& p : chunk.monitorOut)                        advance (p);
        for (auto& p : chunk.streamOut)                         advance (p);
        for (auto& p : chunk.micOut)                            advance (p);

        processChunk (chunk);
    }
}

void AudioEngine::processChunk (const EngineIO& io) noexcept
{
    const int n = io.numFrames;
    jassert (n > 0 && n <= kMaxBlock);

    for (auto& mix : buses)
        for (auto& side : mix)
            juce::FloatVectorOperations::clear (side.data(), n);

    auto& monitorBus = buses[(size_t) Mix::Monitor];
    auto& streamBus  = buses[(size_t) Mix::Stream];

    // Mic: mono (L, or (L+R)/2 when both sides are linked) duplicated to stereo. When it isn't linked we still
    // run the chain on silence so effect tails ring out instead of being cut.
    auto& mic = channels[(size_t) kMicChannel];
    bool micRan = false;

    if (io.micIn[0] != nullptr)
    {
        if (io.micIn[1] != nullptr)
        {
            for (int i = 0; i < n; ++i)
                micMono[(size_t) i] = 0.5f * (io.micIn[0][i] + io.micIn[1][i]);
        }
        else
        {
            juce::FloatVectorOperations::copy (micMono.data(), io.micIn[0], n);
        }

        micRan = mic.process (micMono.data(), micMono.data(), n);
    }
    else
    {
        micRan = mic.process (nullptr, nullptr, n);
    }

    for (int k = 0; k < kMaxAppChannels; ++k)
    {
        auto& channel = channels[(size_t) k];

        if (channel.process (io.appIn[k][0], io.appIn[k][1], n))
        {
            channel.addToMix (Mix::Monitor, monitorBus[0].data(), monitorBus[1].data(), n);
            channel.addToMix (Mix::Stream,  streamBus[0].data(),  streamBus[1].data(),  n);
        }
    }

    if (micRan)
    {
        mic.addToMix (Mix::Monitor, monitorBus[0].data(), monitorBus[1].data(), n);
        mic.addToMix (Mix::Stream,  streamBus[0].data(),  streamBus[1].data(),  n);
    }

    // Input channels (a second mic, a capture card, a line-in): stereo pairs like the app slots, after the mic so that the sums
    // of a mixer without any input are exactly what they were. An inactive slot returns at once.
    for (int k = 0; k < kMaxInputChannels; ++k)
    {
        auto& channel = channels[(size_t) (kInputChannel0 + k)];

        if (channel.process (io.inputIn[k][0], io.inputIn[k][1], n))
        {
            channel.addToMix (Mix::Monitor, monitorBus[0].data(), monitorBus[1].data(), n);
            channel.addToMix (Mix::Stream,  streamBus[0].data(),  streamBus[1].data(),  n);
        }
    }

    // Processed mic for apps (Discord etc.): post-FX, unity, mono, only the mute applies.
    if (io.micOut[0] != nullptr || io.micOut[1] != nullptr)
    {
        micOutGain.setTarget (micOutMuted.load (std::memory_order_relaxed) ? 0.0f : 1.0f);

        float* const l = io.micOut[0] != nullptr ? io.micOut[0] : io.micOut[1];

        if (micRan)
        {
            const auto* srcL = mic.getOutputL();
            const auto* srcR = mic.getOutputR();

            for (int i = 0; i < n; ++i)
                l[i] = 0.5f * (srcL[i] + srcR[i]);

            if (micOutGain.fillRamp (rampBuffer.data(), n))
                juce::FloatVectorOperations::multiply (l, rampBuffer.data(), n);
            else if (micOutGain.getTarget() != 1.0f)
                juce::FloatVectorOperations::multiply (l, micOutGain.getTarget(), n);
        }
        else
        {
            juce::FloatVectorOperations::clear (l, n);
            micOutGain.fillRamp (rampBuffer.data(), n);     // keep the smoother running
        }

        if (io.micOut[0] != nullptr && io.micOut[1] != nullptr)
            juce::FloatVectorOperations::copy (io.micOut[1], io.micOut[0], n);
    }

    finishMix (Mix::Monitor, io.monitorOut, n);
    finishMix (Mix::Stream, io.streamOut, n);
}

// Master gain/mute, safety limiter, master meter, then copy to the (optional) output pointers.
void AudioEngine::finishMix (Mix mix, float* const* out, int n) noexcept
{
    auto& master = masters[(size_t) mix];
    auto& bus = buses[(size_t) mix];
    auto* l = bus[0].data();
    auto* r = bus[1].data();

    master.smoothed.setTarget (master.muted.load (std::memory_order_relaxed) ? 0.0f
                                                                              : master.gain.load (std::memory_order_relaxed));

    if (master.smoothed.fillRamp (rampBuffer.data(), n))
    {
        juce::FloatVectorOperations::multiply (l, rampBuffer.data(), n);
        juce::FloatVectorOperations::multiply (r, rampBuffer.data(), n);
    }
    else if (master.smoothed.getTarget() != 1.0f)
    {
        juce::FloatVectorOperations::multiply (l, master.smoothed.getTarget(), n);
        juce::FloatVectorOperations::multiply (r, master.smoothed.getTarget(), n);
    }

    // The limiter always runs so its delay is constant; when switched off it just stops attenuating.
    master.limiter.process (l, r, n, master.limiterOn.load (std::memory_order_relaxed));

    master.meter.update (peakOf (l, n), peakOf (r, n));

    if (out[0] != nullptr)  juce::FloatVectorOperations::copy (out[0], l, n);
    if (out[1] != nullptr)  juce::FloatVectorOperations::copy (out[1], r, n);
}

// ---------------------------------------------------------------------------------------------------
// Message thread
// ---------------------------------------------------------------------------------------------------

bool AudioEngine::prepare (double sampleRate)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (sampleRate > 0.0);

    // Suspend the RT thread: from now on every cycle that starts sees suspendFx == true, writes
    // silence and never calls into a processor. What is left is a cycle that is already running. Dekker-style,
    // with everything sequentially consistent: either the RT thread has bumped suspendAck (it started a cycle after
    // our store, so all earlier cycles are over) or rtBusy reads false (no cycle is running, and the next one will
    // see the flag). Waiting for either needs no cooperation from a backend whose RT thread is not running yet.
    const auto ackBefore = suspendAck.load();
    suspendFx.store (true);

    const auto deadline = juce::Time::getMillisecondCounter() + prepareTimeoutMs;

    while (suspendAck.load() == ackBefore && rtBusy.load())
    {
        if (juce::Time::getMillisecondCounter() > deadline)
        {
            // The RT thread is stuck inside a processor. Touching that processor now would be a data race, so
            // back off; it will request the rate again as soon as it returns.
            suspendFx.store (false);
            return false;
        }

        juce::Thread::sleep (1);
    }

    for (auto& c : channels)
        c.prepare (sampleRate);

    for (auto& m : masters)
    {
        m.smoothed.prepare (sampleRate);
        m.smoothed.snapTo (m.muted.load (std::memory_order_relaxed) ? 0.0f : m.gain.load (std::memory_order_relaxed));
        m.limiter.prepare (sampleRate, 1.5);
        m.limiter.setReleaseMs (50.0f);
    }

    masterLatency.store (masters[0].limiter.getLatencySamples(), std::memory_order_relaxed);

    micOutGain.prepare (sampleRate);
    micOutGain.snapTo (micOutMuted.load (std::memory_order_relaxed) ? 0.0f : 1.0f);

    // Live processors are released and prepared again in place, not re-instantiated.
    for (auto& chain : currentChains)
        if (chain != nullptr)
            chain->reprepare (sampleRate);

    preparedRate.store (sampleRate, std::memory_order_release);
    suspendFx.store (false);
    return true;
}

void AudioEngine::setChannelActive (int channel, bool active)
{
    channelAt (channel).setActive (active);
}

void AudioEngine::setChannelGainDb (int channel, Mix mix, float db)
{
    channelAt (channel).setGain (mix, dbToLinear (db));
}

void AudioEngine::setChannelMuted (int channel, Mix mix, bool muted)
{
    channelAt (channel).setMuted (mix, muted);
}

void AudioEngine::setChannelInputTrimDb (int channel, float db)
{
    channelAt (channel).setInputTrim (db <= kMinusInfDb ? 0.0f : dbToLinear (db));
}

void AudioEngine::setMasterGainDb (Mix mix, float db)
{
    masters[(size_t) mix].gain.store (dbToLinear (db), std::memory_order_relaxed);
}

void AudioEngine::setMasterMuted (Mix mix, bool muted)
{
    masters[(size_t) mix].muted.store (muted, std::memory_order_relaxed);
}

void AudioEngine::setLimiterEnabled (Mix mix, bool enabled)
{
    masters[(size_t) mix].limiterOn.store (enabled, std::memory_order_relaxed);
}

void AudioEngine::setMicOutMuted (bool muted)
{
    micOutMuted.store (muted, std::memory_order_relaxed);
}

// Why snapshots are never freed under the RT thread
// --------------------------------------------------
// The RT thread never owns a chain: ChannelProcessor::process() loads the raw pointer once per chunk and uses
// it until the end of process(). Ownership stays with the message thread (currentChains + retired), so the only
// question is when a retired snapshot may be released. process() does, in this order (all seq_cst):
//     rtBusy = true; ++cycleCounter; ... chain.load() ... ; rtBusy = false
// and setChain() does, in this order (all seq_cst): chain.exchange(new); epoch = cycleCounter.load().
// Claim: a retired snapshot with epoch e may be released once cycleCounter > e, or once rtBusy reads false.
//  - Counter: a cycle that could have loaded the old pointer loaded it before the exchange, so its increment (which
//    precedes its load) is ordered before the exchange and before our counter read: it is cycle number <= e.
//    Cycles are sequential, so cycleCounter > e means cycle e has ended and no later cycle can see the old pointer.
//  - Idle: if rtBusy reads false after the exchange, no cycle is running (the store of `false` that we read ended
//    the last one) and any cycle that starts later sets rtBusy before it loads the chain, i.e. after the exchange.
// The second rule lets snapshots go while the backend is stopped and the counter is frozen; the first bounds the
// wait when the RT thread is busy most of the time. Neither needs the RT thread to do anything but its normal work.
// The RT thread itself never allocates, locks or frees for any of this: chain.load() is one atomic read.
void AudioEngine::setChain (int channel, FxChain::Ptr chain)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (juce::isPositiveAndBelow (channel, kNumChannels));

    auto& slot = currentChains[(size_t) channel];
    auto previous = std::exchange (slot, chain);     // we hold the new snapshot before the RT thread can see it

    channelAt (channel).exchangeChain (chain.get());

    if (previous != nullptr)
        retired.push_back ({ std::move (previous), cycleCounter.load() });

    collectGarbage();
}

FxChain::Ptr AudioEngine::getChain (int channel) const
{
    return currentChains[(size_t) juce::jlimit (0, kNumChannels - 1, channel)];
}

void AudioEngine::collectGarbage()
{
    JUCE_ASSERT_MESSAGE_THREAD

    // Read after every retirement in the list has happened (same thread), see above.
    const auto now = cycleCounter.load();
    const bool rtIdle = ! rtBusy.load();

    retired.erase (std::remove_if (retired.begin(), retired.end(),
                                   [now, rtIdle] (const Retired& r) { return rtIdle || now > r.epoch; }),
                   retired.end());
}

ChannelLevels AudioEngine::readChannelLevels (int channel) noexcept
{
    return channelAt (channel).meters.readAndReset();
}

PeakPair AudioEngine::readMasterLevels (Mix mix) noexcept
{
    return masters[(size_t) mix].meter.readAndReset();
}

void AudioEngine::resetMicWatch() noexcept
{
    micResetRequested.store (true);
}
}
