// AudioEngine: platform-independent mixer core. A backend calls process() from its RT thread.
#pragma once

#include "Juce.h"
#include "engine/ChannelProcessor.h"
#include "engine/EngineTypes.h"
#include "engine/FxChain.h"
#include "engine/Meters.h"
#include "engine/RtUtils.h"
#include "engine/dsp/LookaheadLimiter.h"

#include <array>
#include <atomic>
#include <vector>

namespace hr
{
class AudioEngine
{
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine (const AudioEngine&) = delete;
    AudioEngine& operator= (const AudioEngine&) = delete;

    // ================================================================================================
    // RT thread
    // ================================================================================================

    // Mixes one cycle. Never blocks, allocates or logs. Every non-null output pointer is fully written.
    // If io.sampleRate differs from the prepared rate the outputs are silence and requestedRate is set.
    void process (const EngineIO& io) noexcept;

    // ================================================================================================
    // Message thread only
    // ================================================================================================

    // (Re)prepares every live FX processor (release + prepare, no re-instantiation), the smoothers and the master
    // limiters. The RT thread is suspended (silence) while it does so and resumes when this returns true.
    // Returns false, having changed nothing, if the RT thread never left its cycle within prepareTimeoutMs (stuck in a plugin).
    // Until this has been called once, process() outputs silence and requests the backend's rate.
    bool prepare (double sampleRate);
    double getPreparedRate() const noexcept                 { return preparedRate.load (std::memory_order_acquire); }

    // RT sets this when it sees a rate other than the prepared one; a 50 ms message-thread timer
    // (AppController) polls it (exchange with 0) and calls prepare().
    std::atomic<double> requestedRate { 0.0 };

    // How long prepare() waits for a cycle that is still running when it suspends the RT thread (message thread;
    // tests shorten it).
    uint32_t prepareTimeoutMs = 500;

    // Channel index: 0..kMaxAppChannels-1 = app slot, kMicChannel = mic, kInputChannel0 + k = input slot k (a second mic, a
    // capture card, a line-in). Inactive channels are skipped.
    void setChannelActive (int channel, bool active);
    void setChannelGainDb (int channel, Mix mix, float db);     // db <= kMinusInfDb (-100) means -inf
    void setChannelMuted (int channel, Mix mix, bool muted);
    void setChannelInputTrimDb (int channel, float db);

    void setMasterGainDb (Mix mix, float db);
    void setMasterMuted (Mix mix, bool muted);
    void setLimiterEnabled (Mix mix, bool enabled);            // safety brickwall at -0.3 dBFS (1.5 ms lookahead, 50 ms release), default on
    void setMicOutMuted (bool muted);                          // mutes the processed mic sent to "Headroom Mic"

    // Publishes a new FX chain snapshot for a channel (nullptr = no effects). The previous snapshot is retired
    // and destroyed by collectGarbage() (which setChain() also runs), never on the RT thread: it is kept until the
    // RT thread has provably left every cycle that could have seen it (proof in AudioEngine.cpp).
    // Slots that stay in the chain keep their state; new slots warm up dry and fade in (see FxSlot). A slot that
    // leaves the chain is cut at once: to remove one without a tick, set its `bypassed` flag first and rebuild the
    // chain about 30 ms later.
    void setChain (int channel, FxChain::Ptr chain);
    FxChain::Ptr getChain (int channel) const;

    // Destroys retired snapshots the RT thread has moved past. Call from a timer (AppController: 50 ms).
    void collectGarbage();

    // ================================================================================================
    // Meters (any thread, normally the UI at 30 Hz). Each call returns the peak since the previous call.
    // ================================================================================================
    ChannelLevels readChannelLevels (int channel) noexcept;    // input = post-trim, pre-FX, pre-fader; monitor/stream = post-fader
    PeakPair readMasterLevels (Mix mix) noexcept;              // post master gain and limiter

    // Delay the master limiters add to the Headphones and Stream mixes (constant, also while a limiter is switched
    // off, so toggling never shifts the audio). The processed mic (micOut) is not delayed.
    int getMasterLatencySamples() const noexcept                { return masterLatency.load (std::memory_order_relaxed); }

    // ================================================================================================
    // Mic watchdog inputs for the backend
    // ================================================================================================
    // All three describe the mic *input* buffer as delivered by the backend (before trim and FX) and only move
    // while that buffer is non-null; an unlinked mic resets silent-ms and live-cycles to 0 (never counts as silent).
    std::atomic<uint32_t> micSilentMs { 0 };        // consecutive ms of exactly-zero mic samples (|x| < 1e-9)
    std::atomic<uint32_t> micSeenSignal { 0 };      // 1 once any cycle had signal since resetMicWatch()
    std::atomic<uint32_t> micLiveCycles { 0 };      // consecutive cycles with a non-null mic input buffer
    void resetMicWatch() noexcept;                  // call when capture is (re)started

    // The same idea for the input slots, which the backend uses to open playback on an Elgato device only after the capture
    // of an input channel on that same device flows: consecutive cycles in which the slot had a buffer (0 =
    // not linked). RT writes, any thread reads.
    std::array<std::atomic<uint32_t>, kMaxInputChannels> inputLiveCycles {};

    uint64_t getCycleCount() const noexcept         { return cycleCounter.load(); }

private:
    struct MixMaster
    {
        std::atomic<float> gain { 1.0f };
        std::atomic<bool> muted { false };
        std::atomic<bool> limiterOn { true };
        SmoothedGain smoothed;
        dsp::LookaheadLimiter limiter;
        StereoPeak meter;
    };

    struct Retired
    {
        FxChain::Ptr chain;
        uint64_t epoch;
    };

    void processChunk (const EngineIO& chunk) noexcept;
    void updateMicWatch (const EngineIO& io) noexcept;
    void updateInputWatch (const EngineIO& io) noexcept;
    void finishMix (Mix mix, float* const* out, int n) noexcept;
    static void writeSilence (const EngineIO& io) noexcept;

    ChannelProcessor& channelAt (int index) noexcept;

    std::array<ChannelProcessor, kNumChannels> channels;
    std::array<MixMaster, kNumMixes> masters;

    SmoothedGain micOutGain;
    std::atomic<bool> micOutMuted { false };

    std::array<std::array<std::array<float, kMaxBlock>, 2>, kNumMixes> buses {};    // [mix][L/R]
    std::array<float, kMaxBlock> micMono {};
    std::array<float, kMaxBlock> rampBuffer {};

    // Synchronisation between the RT thread and the message thread. All of these are sequentially consistent on
    // purpose; the reasoning is in AudioEngine.cpp ("Why snapshots are never freed under the RT thread").
    std::atomic<double> preparedRate { 0.0 };
    std::atomic<bool> suspendFx { false };      // message thread: "do not touch any processor"
    std::atomic<uint32_t> suspendAck { 0 };     // RT: bumped once per cycle that saw suspendFx
    std::atomic<bool> rtBusy { false };         // RT: inside process()
    std::atomic<uint64_t> cycleCounter { 0 };   // incremented at the start of every process(); the epoch clock
    std::atomic<int> masterLatency { 0 };

    // RT-only mic watch state
    std::atomic<bool> micResetRequested { false };
    uint64_t micSilentSamples = 0;

    // Message-thread only
    std::array<FxChain::Ptr, kNumChannels> currentChains;
    std::vector<Retired> retired;
};
}
