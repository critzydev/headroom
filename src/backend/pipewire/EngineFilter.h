// The pw_filter that carries all audio through hr::AudioEngine: fixed port set, RT process callback.
#pragma once

#include "PwCommon.h"

#include "engine/AudioEngine.h"

namespace hr::pw
{

/** What the engine's mic watchdog has seen since the last resetMicProbe(). */
struct MicProbeReading
{
    bool valid = false;               // false until the RT thread has run since the reset
    double silentMs = 0.0;            // consecutive ms of exact digital silence on the mic input
    uint32_t liveCycles = 0;          // current run of cycles in which the mic port had a buffer
    uint64_t cycles = 0;              // cycles the filter has run since the reset
    bool seenSignal = false;          // any non-silent mic sample since the reset
};

/**
    "headroom.engine": a class-less pw_filter (invisible to pavucontrol/KDE, never auto-linked by
    WirePlumber) with a fixed set of ports that is created once and never changed:
      inputs  app0_L app0_R ... app15_L app15_R mic_L mic_R in0_L in0_R ... in7_L in7_R
      outputs monitor_L monitor_R stream_L stream_R micout_L micout_R
    process() builds an EngineIO from the DSP buffers and calls AudioEngine::process; nothing
    else happens on the RT thread. The mic watchdog lives in the engine (micSilentMs, micSeenSignal,
    micLiveCycles); this class only wraps reset and read.
*/
class EngineFilter
{
public:
    static constexpr int kInputPorts = hr::kMaxAppChannels * 2 + 2 + hr::kMaxInputChannels * 2;
    static constexpr int kOutputPorts = 6;

    explicit EngineFilter (hr::AudioEngine& engineToDrive);
    ~EngineFilter();

    EngineFilter (const EngineFilter&) = delete;
    EngineFilter& operator= (const EngineFilter&) = delete;

    /** Loop thread, lock held. */
    bool create (pw_core* core, int quantumFrames);
    void destroy();
    bool exists() const noexcept { return filter != nullptr; }

    void setQuantum (int frames);

    /** SPA_ID_INVALID until the node has been created on the server. */
    uint32_t nodeId() const;
    bool hasFailed() const;

    // Port names, in the order the ports were created.
    static std::string inputPortName (int index);
    static std::string outputPortName (int index);
    static std::string appPortName (int slot, bool right);
    static std::string inputSlotPortName (int slot, bool right);        // "in0_L": the input channels' pairs, after the mic's

    // ---- mic probe (WaveGuard) --------------------------------------------------------------
    /** Loop thread. Restarts the engine's mic watchdog; returns the token to pass to readMicProbe(). */
    uint32_t resetMicProbe();

    /** Loop thread. Not valid until the RT thread has run a couple of cycles after the reset. */
    MicProbeReading readMicProbe (uint32_t token) const;

    // ---- facts published by the RT thread ----------------------------------------------------
    uint32_t lastSampleRate() const noexcept { return pubRate.load (std::memory_order_relaxed); }

    /** Consecutive cycles in which an input channel's slot had a buffer (0 = nothing linked): CaptureGate's evidence. */
    uint32_t inputLiveCycles (int slot) const noexcept
    {
        return juce::isPositiveAndBelow (slot, hr::kMaxInputChannels) ? engine.inputLiveCycles[(size_t) slot].load (std::memory_order_relaxed) : 0u;
    }
    uint32_t lastQuantum() const noexcept { return pubQuantum.load (std::memory_order_relaxed); }

    /** Called on the loop thread when the filter changes state. */
    std::function<void()> onStateChanged;

private:
    static void onProcess (void* data, spa_io_position* position);
    static void onStateChangedCb (void* data, pw_filter_state oldState, pw_filter_state newState, const char* error);

    void process (spa_io_position* position) noexcept;

    const float* inputBuffer (int index, uint32_t frames) const noexcept
    {
        return static_cast<const float*> (pw_filter_get_dsp_buffer (inPorts[index], frames));
    }

    float* outputBuffer (int index, uint32_t frames) const noexcept
    {
        return static_cast<float*> (pw_filter_get_dsp_buffer (outPorts[index], frames));
    }

    hr::AudioEngine& engine;

    pw_filter* filter = nullptr;
    spa_hook listener {};
    bool listenerActive = false;
    pw_filter_state currentState = PW_FILTER_STATE_UNCONNECTED;

    void* inPorts[kInputPorts] {};
    void* outPorts[kOutputPorts] {};

    // Loop thread only.
    uint32_t probeToken = 0;
    uint64_t resetCycleMark = 0;

    // RT -> loop thread, for the status line.
    std::atomic<uint32_t> pubRate { 0 }, pubQuantum { 0 };
};

} // namespace hr::pw
