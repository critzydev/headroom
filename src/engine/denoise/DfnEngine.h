// DfnEngine: DeepFilterNet on a worker thread, behind the fixed-delay Denoiser48 interface.
#pragma once

#include "Juce.h"
#include "engine/denoise/Denoiser48.h"
#include "engine/denoise/DfnRuntime.h"

#include <memory>

namespace hr::denoise
{
// Lets the platform backend give the worker thread realtime priority the way the audio thread got its own (on Linux
// PipeWire's rtkit support, which works for an ordinary user where sched_setscheduler does not). Returns true on success.
// Called on the worker thread itself when it starts; set it once, early, and leave it.
using ThreadPromoter = bool (*) (int priority) noexcept;
void setWorkerThreadPromoter (ThreadPromoter promoter) noexcept;

// Why a worker thread and not a call inside process(): one DeepFilterNet frame allocates about 1300 blocks of memory
// (~0.7 MB) through tract and takes 0.3-2.2 ms, which the audio thread must never pay.
//
// Every 480 samples the audio thread hands the finished frame to a wait-free queue and wakes the worker (an atomic
// counter + futex, no lock). The worker runs the network and queues the answer. The answer for a frame is *played*
// exactly one frame (10 ms) after the frame was handed in, so the worker has 10 ms for it and the delay never
// changes:
//
//     latency() = 480 (collecting the frame) + 480 (waiting for the answer) + model latency (480 or 1440)
//               = 1440 samples (30 ms) with the low-latency model, 2400 (50 ms) with the standard one.
//
// If an answer is not there when its frame is due (worker starved, machine overloaded) the input, delayed by the same
// latency, plays in its place for that frame and the miss is counted; it fades back to the network's output over 5 ms
// when answers return. Frames that could not even be queued (worker stalled for 160 ms) are counted as overruns.
//
// Not cutting the voice (measured): libDF's shortcuts by the network's local-SNR estimate are off (see
// kMinDb in DfnEngine.cpp), and a short speech protection, which NoiseRemoval switches on, keeps what the network takes
// for noise right after speech from being attenuated by more than a few dB (setProtection).
//
// Stereo: the second lane is only fed while the two channels differ. When it starts, its network has no history, so it
// plays the (aligned) input for 60 ms before it fades to the network's output.
class DfnEngine final : public Denoiser48
{
public:
    // Creates the network state(s) and starts the worker thread. nullptr (with `error`) if that is impossible.
    // Not for the audio thread.
    static std::unique_ptr<DfnEngine> create (std::shared_ptr<dfn::LoadedModel> model, juce::String& error);

    ~DfnEngine() override;

    int latency() const noexcept override;
    void process (const float* const* in, float* const* out, int numLanes, int n) noexcept override;
    float voice() const noexcept override;
    void setPostFilter (float beta) noexcept override;
    uint32_t misses() const noexcept override;
    float workerLoad() const noexcept override;
    float worstFrameMs() const noexcept override;

    uint32_t overruns() const noexcept;

    // libDF's local-SNR thresholds for every lane's network (see dfn::State::setThresholds; the defaults are in the .cpp). Any
    // thread; the worker applies a change before its next frame.
    struct Thresholds { float minDb, maxErbDb, maxDfDb; };
    void setThresholds (Thresholds thresholds) noexcept;

    // Speech protection: for `holdFrames` frames after the last run of `armFrames` or more consecutive frames the network found
    // speech in (its local SNR >= speechLsnrDb; a run, so that single noisy frames do not arm it), a frame it calls noise is not
    // attenuated by more than `limitDb` (the aligned input is mixed back in); afterwards that limit lifts by `releaseDbPerFrame`
    // per frame. Word endings and the weak parts of speech that the network takes for noise survive; a pause longer than the hold
    // is cleaned in full. limitDb <= 0 or holdFrames <= 0 = off, which is how an engine starts. Any thread.
    struct Protection { float speechLsnrDb; float limitDb; int holdFrames; float releaseDbPerFrame; int armFrames; };
    void setProtection (Protection protection) noexcept;

    // Tests: makes the worker sleep this long before every frame (0 = normal), to provoke missed frames.
    void setTestWorkerDelayMicroseconds (int microseconds) noexcept;

    // Tests: the network's attenuation limit in dB (>= 100 = none). About 0.02 dB makes the output the input, delayed by
    // exactly latency(), which is how the latency is measured.
    void setTestAttenuationLimitDb (float db) noexcept;

    // Tests, process-wide: the audio thread waits for the answer to every frame before it goes on, so that offline
    // processing (faster than real time) never misses a frame and gives the same result every time.
    static void setTestLockstep (bool on) noexcept;

    // Tests: the worker's clock for a frame (ms), for measuring; also how the thread ended up being scheduled.
    juce::String describeWorkerThread() const;

private:
    struct Impl;
    explicit DfnEngine (std::unique_ptr<Impl>);

    std::unique_ptr<Impl> impl;
};
}
