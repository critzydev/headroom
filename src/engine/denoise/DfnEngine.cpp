// DfnEngine implementation: frame assembly on the audio thread, wait-free queues, and the network on a worker thread.
#include "engine/denoise/DfnEngine.h"

#include "app/Log.h"
#include "engine/denoise/CpuPlacement.h"
#include "engine/denoise/SpscQueue.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

#if JUCE_LINUX
 #include <pthread.h>
 #include <sched.h>
 #include <sys/resource.h>
 #include <sys/syscall.h>
 #include <unistd.h>
#endif

namespace hr::denoise
{
namespace
{
    constexpr uint32_t kQueueFrames = 16;                   // 160 ms of slack per direction
    constexpr uint32_t kHistory = 8192;                     // input history: must cover latency() + a frame
    constexpr int kWarmFrames = 6;                          // cold network: its first answers are not trusted (60 ms)
    constexpr int kStartFrames = 3;                         // no answer can exist yet for the first frames
    constexpr float kFadeStep = 1.0f / 240.0f;              // dry -> network over 5 ms
    constexpr float kLoadSmoothing = 0.02f;                 // per frame: about 0.5 s

    // libDF saves work by what the network thinks of a frame (its local SNR estimate, -15..35 dB): below -10 it mutes the frame
    // without running the decoders, above 20 it skips the deep-filtering stage and applies only the ERB gains, above 30 it
    // lets the frame through. Measured on real speech (tests/unit/SpeechCutTests.cpp), the ERB gains alone
    // are wrong for the standard network (Best): from 20 dB local SNR on, i.e. for speech over a quiet room, they take most of the
    // voice below ~5 kHz away (35 % of the speech frames at 20 dB signal-to-noise were more than 6 dB too quiet). And a mute is
    // final for words that fade into the noise. So nothing is skipped here except the frames that are clean already (> 30 dB);
    // the network's own gains decide for the rest. The cost is the decoders running in silence too (a few % of a core).
    constexpr float kMinDb = -16.0f, kMaxErbDb = 30.0f, kMaxDfDb = 35.0f;

    std::atomic<bool> lockstepForTests { false };
    std::atomic<ThreadPromoter> workerPromoter { nullptr };
    constexpr int kWorkerPriority = 40;

    // DeepFilterNet's local SNR (dB) says how speech-like a frame is: about -15..-5 in noise, 5..30 in speech.
    float voiceFromLsnr (float lsnr) noexcept
    {
        return juce::jlimit (0.0f, 1.0f, (lsnr + 5.0f) * (1.0f / 15.0f));
    }

    enum class Scheduling : int { normal = 0, nice, realtime, high };

    struct SchedulingResult
    {
        Scheduling kind = Scheduling::normal;
        int value = 0;
    };

    // Best effort: this thread must be woken within a few milliseconds even when the machine is busy.
    SchedulingResult raiseCurrentThreadPriority()
    {
        if (auto promote = workerPromoter.load (std::memory_order_acquire))
            if (promote (kWorkerPriority))
            {
               #if JUCE_LINUX
                // rtkit caps the request (20 on a stock desktop), so report what we actually got. Ask the kernel for this
                // thread (pid 0): glibc's pthread_getschedparam returns its cached creation value when rtkit changed it.
                sched_param granted {};

                if (sched_getparam (0, &granted) == 0 && granted.sched_priority > 0)
                    return { Scheduling::realtime, granted.sched_priority };
               #endif

                return { Scheduling::realtime, kWorkerPriority };
            }

       #if JUCE_LINUX
        rlimit limit {};

        if (getrlimit (RLIMIT_RTPRIO, &limit) == 0 && limit.rlim_cur > 0)
        {
            sched_param param {};
            param.sched_priority = (int) std::min<rlim_t> (limit.rlim_cur, kWorkerPriority);

            if (pthread_setschedparam (pthread_self(), SCHED_RR, &param) == 0)
                return { Scheduling::realtime, param.sched_priority };
        }

        // Not allowed to be realtime (normal for a desktop user): a raised nice value where the limits permit it.
        if (setpriority (PRIO_PROCESS, (id_t) syscall (SYS_gettid), -8) == 0)
            return { Scheduling::nice, -8 };

        return {};
       #else
        return { Scheduling::high, 0 };         // the thread was started with Priority::high
       #endif
    }
}

void setWorkerThreadPromoter (ThreadPromoter promoter) noexcept
{
    workerPromoter.store (promoter, std::memory_order_release);
}

// ---------------------------------------------------------------------------------------------------

struct DfnEngine::Impl : private juce::Thread
{
    struct Frame
    {
        float s[kFrame] {};
        uint32_t seq = 0;
        float lsnr = 0.0f;
    };

    struct Lane
    {
        // Worker side
        std::unique_ptr<dfn::State> state;
        SpscQueue<Frame, kQueueFrames> inQ, outQ;
        float appliedPostFilter = 0.0f, appliedLimit = 100.0f;
        uint32_t appliedThresholds = 0;             // the version of `thresholds` the network has
        std::atomic<int64_t> doneSeq { -1 };        // last frame the worker has finished with (answered or skipped)

        // Audio side
        std::vector<float> hist;                    // the input, kHistory samples back
        Frame* claimed = nullptr;                   // the input frame being filled
        Frame scratch;                              // where it goes when the queue is full
        float wet[kFrame] {}, dry[kFrame] {};       // what plays during the current frame
        float w = 0.0f, target = 0.0f;              // 0 = dry, 1 = network
        float lastLsnr = 0.0f;
        int warm = kWarmFrames;
        int sinceSpeech = 1 << 20;                  // frames since the network last found speech (in a run long enough)
        int speechRun = 0;                          // consecutive frames that look like speech
        float protectShare = 0.0f;                  // the share of the aligned input mixed into the last frame, 0..1
    };

    explicit Impl (std::shared_ptr<dfn::LoadedModel> m)
        : juce::Thread ("hr-denoise"), model (std::move (m)), modelLatency (model->latency())
    {
        for (auto& lane : lanes)
            lane.hist.assign (kHistory, 0.0f);
    }

    ~Impl() override
    {
        stop();
    }

    bool start (juce::String& error)
    {
        // Only the first lane's state now: a second one costs as much as loading the model again, and only a stereo
        // input needs it (the mic is one lane). It is made in the background when the audio thread first asks for it.
        lanes[0].state = model->createState();

        if (lanes[0].state == nullptr)
        {
            error = "could not create the network state";
            return false;
        }

        beginFrame (1);

        if (! startThread (juce::Thread::Priority::high))
        {
            error = "could not start the worker thread";
            return false;
        }

        started = true;
        return true;
    }

    void stop()
    {
        if (started)
        {
            exiting.store (true, std::memory_order_release);
            wake.fetch_add (1, std::memory_order_release);
            wake.notify_one();
            stopThread (10000);
            started = false;
        }

        if (lane1Thread.joinable())
            lane1Thread.join();
    }

    // -------------------------------------------------------------------------------------------
    // Worker thread
    // -------------------------------------------------------------------------------------------
    void run() override
    {
        // First the CPUs (the last L3 group, away from the game's cores; a no-op on one group, elsewhere than Linux, or with
        // HEADROOM_NO_PIN=1), then the priority: the thread is on its cores from the first time it runs as a realtime thread.
        // The stereo lane's short-lived thread below is started from here and so inherits them.
        cpu::pinCurrentThread();

        const auto scheduling = raiseCurrentThreadPriority();
        schedulingKind.store ((int) scheduling.kind, std::memory_order_relaxed);
        schedulingValue.store (scheduling.value, std::memory_order_relaxed);

        static std::atomic<bool> logged { false };

        if (! logged.exchange (true))
            Log::info ("Noise removal thread: " + describe (scheduling.kind, scheduling.value));

        while (true)
        {
            const auto seen = wake.load (std::memory_order_acquire);
            bool worked = false;

            if (lane1Wanted.load (std::memory_order_relaxed) && ! lane1Started)
            {
                lane1Started = true;
                lane1Thread = std::thread ([this]
                {
                    if (auto state = model->createState())
                    {
                        lanes[1].state = std::move (state);
                        lane1Ready.store (true, std::memory_order_release);
                    }
                });
            }

            for (auto& lane : lanes)
                worked = drain (lane) || worked;

            if (exiting.load (std::memory_order_acquire))
                break;

            if (! worked)
                wake.wait (seen);               // returns at once if the audio thread woke us since `seen`
        }
    }

    static juce::String describe (Scheduling kind, int value)
    {
        switch (kind)
        {
            case Scheduling::realtime:  return "realtime priority " + juce::String (value);
            case Scheduling::nice:      return "nice " + juce::String (value);
            case Scheduling::high:      return "high priority";
            case Scheduling::normal:    break;
        }

        return "normal priority";
    }

    bool drain (Lane& lane)
    {
        bool worked = false;
        const bool usable = &lane != &lanes[1] || lane1Ready.load (std::memory_order_acquire);

        while (auto* in = lane.inQ.peek())
        {
            if (exiting.load (std::memory_order_relaxed))
                return worked;

            auto* out = usable ? lane.outQ.claim() : nullptr;

            if (out == nullptr)             // nobody is collecting answers (or this lane has no network yet): skip, do not stall
            {
                lane.doneSeq.store ((int64_t) in->seq, std::memory_order_release);
                lane.inQ.pop();
                worked = true;
                continue;
            }

            if (const int extra = testDelayMicroseconds.load (std::memory_order_relaxed); extra > 0)
                std::this_thread::sleep_for (std::chrono::microseconds (extra));

            const float beta = postFilter.load (std::memory_order_relaxed);

            if (beta != lane.appliedPostFilter)
            {
                lane.state->setPostFilter (beta);
                lane.appliedPostFilter = beta;
            }

            const float limit = attenuationLimit.load (std::memory_order_relaxed);

            if (limit != lane.appliedLimit)
            {
                lane.state->setAttenuationLimit (limit);
                lane.appliedLimit = limit;
            }

            if (const auto version = thresholdsVersion.load (std::memory_order_acquire); version != lane.appliedThresholds)
            {
                lane.state->setThresholds (thresholdsNow[0].load (std::memory_order_relaxed), thresholdsNow[1].load (std::memory_order_relaxed),
                                           thresholdsNow[2].load (std::memory_order_relaxed));
                lane.appliedThresholds = version;
            }

            const auto t0 = juce::Time::getHighResolutionTicks();
            out->lsnr = lane.state->process (in->s, out->s);
            const auto seconds = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
            const auto ms = (float) (seconds * 1000.0);

            out->seq = in->seq;
            lane.outQ.publish();
            lane.doneSeq.store ((int64_t) in->seq, std::memory_order_release);
            lane.inQ.pop();
            worked = true;

            load += kLoadSmoothing * (ms * 0.1f - load);        // share of the 10 ms budget
            workerLoadShared.store (load, std::memory_order_relaxed);
            worst = std::max (worst * 0.9995f, ms);             // decays over a few seconds
            worstShared.store (worst, std::memory_order_relaxed);
        }

        return worked;
    }

    // -------------------------------------------------------------------------------------------
    // Audio thread
    // -------------------------------------------------------------------------------------------
    void process (const float* const* in, float* const* out, int numLanes, int n) noexcept
    {
        if (numLanes == 2 && ! stereo)
            activateSecondLane();

        stereo = numLanes == 2;

        int pos = 0;

        while (pos < n)
        {
            const int segment = std::min (n - pos, kFrame - fill);

            for (int c = 0; c < numLanes; ++c)
            {
                auto& lane = lanes[(size_t) c];
                float* frameIn = lane.claimed->s + fill;
                float* history = lane.hist.data();
                const float* src = in[c] + pos;
                float* dst = out[c] + pos;
                const float* wet = lane.wet + fill;
                const float* dry = lane.dry + fill;

                for (int i = 0; i < segment; ++i)
                {
                    const float v = src[i];
                    frameIn[i] = v;
                    history[(absolute + (uint32_t) i) & (kHistory - 1)] = v;

                    if (lane.w < lane.target)
                        lane.w = std::min (lane.target, lane.w + kFadeStep);

                    dst[i] = dry[i] + (wet[i] - dry[i]) * lane.w;
                }
            }

            fill += segment;
            pos += segment;
            absolute += (uint32_t) segment;

            if (fill == kFrame)
                endOfFrame (numLanes);
        }
    }

    // The right channel starts to differ from the left one: give it the left one's history (the input was identical
    // up to now). Its network is cold, so for a while it plays the aligned input.
    void activateSecondLane() noexcept
    {
        auto& a = lanes[0];
        auto& b = lanes[1];

        std::copy (a.hist.begin(), a.hist.end(), b.hist.begin());
        std::memcpy (b.dry, a.dry, sizeof (b.dry));
        std::memcpy (b.wet, a.dry, sizeof (b.wet));
        b.warm = kWarmFrames;
        b.w = b.target = 0.0f;
        lane1Wanted.store (true, std::memory_order_relaxed);
        wake.fetch_add (1, std::memory_order_release);          // the worker starts making the network at once
        wake.notify_one();

        // Offline tests: wait for the network instead of playing the input meanwhile.
        if (lockstepForTests.load (std::memory_order_relaxed))
        {
            const auto deadline = juce::Time::getMillisecondCounter() + 10000;

            while (! lane1Ready.load (std::memory_order_acquire) && juce::Time::getMillisecondCounter() < deadline)
                std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }

        b.claimed = b.inQ.claim();

        if (b.claimed == nullptr)
            b.claimed = &b.scratch;

        std::memcpy (b.claimed->s, a.claimed->s, sizeof (float) * (size_t) fill);   // the part of this frame collected so far
    }

    // Frame `frameIndex` has just been completed on every active lane.
    void endOfFrame (int numLanes) noexcept
    {
        bool queued = false;

        for (int c = 0; c < numLanes; ++c)
        {
            auto& lane = lanes[(size_t) c];

            if (lane.claimed != &lane.scratch)
            {
                lane.claimed->seq = frameIndex;
                lane.inQ.publish();
                queued = true;
            }
            else
            {
                overrunCount.fetch_add (1, std::memory_order_relaxed);
            }
        }

        if (queued)
        {
            wake.fetch_add (1, std::memory_order_release);
            wake.notify_one();

            if (lockstepForTests.load (std::memory_order_relaxed))
                waitForAnswers (numLanes);
        }

        ++frameIndex;
        ++framesSeen;
        beginFrame (numLanes);
    }

    // Tests only: until the worker has answered the frame just handed in (at most a second).
    void waitForAnswers (int numLanes) noexcept
    {
        const auto deadline = juce::Time::getMillisecondCounter() + 1000;

        for (int c = 0; c < numLanes; ++c)
            while ((c == 0 || lane1Ready.load (std::memory_order_acquire))
                   && lanes[(size_t) c].doneSeq.load (std::memory_order_acquire) < (int64_t) frameIndex
                   && juce::Time::getMillisecondCounter() < deadline)
                std::this_thread::yield();
    }

    // Chooses what plays during the next frame: the answer for the frame completed one boundary ago, or that frame's
    // input (delayed by the model latency, which is what the answer is a cleaned version of) if the answer is not there.
    void beginFrame (int numLanes) noexcept
    {
        const bool answerDue = frameIndex >= 2;
        float voiceNow = -1.0f;

        for (int c = 0; c < numLanes; ++c)
        {
            auto& lane = lanes[(size_t) c];

            const uint32_t base = absolute - 2 * (uint32_t) kFrame - (uint32_t) modelLatency;

            for (int i = 0; i < kFrame; ++i)
                lane.dry[i] = lane.hist[(base + (uint32_t) i) & (kHistory - 1)];

            const bool networkReady = c == 0 || lane1Ready.load (std::memory_order_acquire);

            if (! networkReady)
                lane.warm = kWarmFrames;            // no network yet: the aligned input plays, and the warm-up has not begun

            const bool got = networkReady && answerDue && collect (lane, frameIndex - 2);

            if (got)
                protect (lane);
            const bool trusted = got && lane.warm == 0;

            if (networkReady && answerDue && lane.warm > 0)
                --lane.warm;

            if (! got)
            {
                std::memcpy (lane.wet, lane.dry, sizeof (lane.wet));

                if (networkReady && answerDue && lane.warm == 0 && framesSeen > (uint32_t) kStartFrames)
                    missCount.fetch_add (1, std::memory_order_relaxed);
            }
            else
            {
                voiceNow = std::max (voiceNow, voiceFromLsnr (lane.lastLsnr));
            }

            lane.target = trusted ? 1.0f : 0.0f;

            if (! trusted)
                lane.w = 0.0f;                          // playing the input itself: nothing to fade from

            lane.claimed = lane.inQ.claim();

            if (lane.claimed == nullptr)
                lane.claimed = &lane.scratch;
        }

        if (voiceNow >= 0.0f)
            voiceShared.store (voiceNow, std::memory_order_relaxed);

        fill = 0;
    }

    // Speech protection (see DfnEngine::Protection): mixes a share of the aligned input into the answer that was just
    // collected, ramped over the frame so that the mix never steps.
    void protect (Lane& lane) noexcept
    {
        const float speechOn = protectionSpeechDb.load (std::memory_order_relaxed);
        const float limitDb = protectionLimitDb.load (std::memory_order_relaxed);
        const int hold = protectionHold.load (std::memory_order_relaxed);
        const float release = protectionRelease.load (std::memory_order_relaxed);
        const int arm = protectionArm.load (std::memory_order_relaxed);

        float target = 0.0f;

        if (limitDb > 0.0f && hold > 0)
        {
            const bool speechNow = lane.lastLsnr >= speechOn;
            lane.speechRun = speechNow ? std::min (lane.speechRun + 1, 1 << 20) : 0;
            lane.sinceSpeech = lane.speechRun >= arm ? 0 : std::min (lane.sinceSpeech + 1, 1 << 20);

            if (! speechNow)
            {
                if (lane.sinceSpeech <= hold)
                    target = std::pow (10.0f, -limitDb * (1.0f / 20.0f));
                else
                    target = lane.protectShare * std::pow (10.0f, -release * (1.0f / 20.0f));

                if (target < 0.003f)
                    target = 0.0f;
            }
        }

        if (target > 0.0f || lane.protectShare > 0.0f)
        {
            const float from = lane.protectShare, step = (target - from) * (1.0f / (float) kFrame);

            for (int i = 0; i < kFrame; ++i)
                lane.wet[i] += (lane.dry[i] - lane.wet[i]) * (from + step * (float) (i + 1));
        }

        lane.protectShare = target;
    }

    // The answer for frame `want` into lane.wet, dropping older answers that arrived too late. False if it is not
    // there (yet), or if the network failed.
    bool collect (Lane& lane, uint32_t want) noexcept
    {
        while (auto* r = lane.outQ.peek())
        {
            const auto age = (int32_t) (r->seq - want);

            if (age < 0)
            {
                lane.outQ.pop();                        // late: its moment has passed
                continue;
            }

            if (age > 0)
                return false;

            const bool usable = std::isfinite (r->lsnr);

            if (usable)
            {
                std::memcpy (lane.wet, r->s, sizeof (lane.wet));
                lane.lastLsnr = r->lsnr;
            }

            lane.outQ.pop();
            return usable;
        }

        return false;
    }

    // -------------------------------------------------------------------------------------------
    std::shared_ptr<dfn::LoadedModel> model;
    const int modelLatency;
    std::array<Lane, kMaxLanes> lanes;

    // audio thread only
    int fill = 0;
    uint32_t frameIndex = 0, absolute = 0, framesSeen = 0;
    bool stereo = false;

    // shared
    std::atomic<uint32_t> wake { 0 };
    std::atomic<bool> exiting { false };
    std::atomic<bool> lane1Wanted { false }, lane1Ready { false };
    std::thread lane1Thread;
    bool lane1Started = false;                          // worker thread only
    std::atomic<float> voiceShared { 0.0f };
    std::atomic<float> postFilter { 0.0f }, attenuationLimit { 100.0f };
    std::atomic<float> thresholdsNow[3] { kMinDb, kMaxErbDb, kMaxDfDb };
    std::atomic<uint32_t> thresholdsVersion { 1 };                        // a new lane has applied version 0: it takes these before its first frame
    std::atomic<float> protectionSpeechDb { 0.0f }, protectionLimitDb { 0.0f }, protectionRelease { 3.0f };      // off until setProtection
    std::atomic<int> protectionHold { 0 }, protectionArm { 1 };
    std::atomic<uint32_t> missCount { 0 }, overrunCount { 0 };
    std::atomic<float> workerLoadShared { 0.0f }, worstShared { 0.0f };
    std::atomic<int> testDelayMicroseconds { 0 };
    std::atomic<int> schedulingKind { 0 }, schedulingValue { 0 };
    float load = 0.0f, worst = 0.0f;                    // worker only
    bool started = false;
};

// ---------------------------------------------------------------------------------------------------

DfnEngine::DfnEngine (std::unique_ptr<Impl> i) : impl (std::move (i)) {}

DfnEngine::~DfnEngine() = default;

std::unique_ptr<DfnEngine> DfnEngine::create (std::shared_ptr<dfn::LoadedModel> model, juce::String& error)
{
    if (model == nullptr || model->frameLength() != kFrame)
    {
        error = "no usable model";
        return nullptr;
    }

    auto impl = std::make_unique<Impl> (std::move (model));

    if (! impl->start (error))
        return nullptr;

    return std::unique_ptr<DfnEngine> (new DfnEngine (std::move (impl)));
}

int DfnEngine::latency() const noexcept                 { return 2 * kFrame + impl->modelLatency; }

void DfnEngine::process (const float* const* in, float* const* out, int numLanes, int n) noexcept
{
    impl->process (in, out, juce::jlimit (1, kMaxLanes, numLanes), n);
}

void DfnEngine::setThresholds (Thresholds thresholds) noexcept
{
    impl->thresholdsNow[0].store (thresholds.minDb, std::memory_order_relaxed);
    impl->thresholdsNow[1].store (thresholds.maxErbDb, std::memory_order_relaxed);
    impl->thresholdsNow[2].store (thresholds.maxDfDb, std::memory_order_relaxed);
    impl->thresholdsVersion.fetch_add (1, std::memory_order_release);
}

void DfnEngine::setProtection (Protection protection) noexcept
{
    impl->protectionSpeechDb.store (protection.speechLsnrDb, std::memory_order_relaxed);
    impl->protectionLimitDb.store (protection.limitDb, std::memory_order_relaxed);
    impl->protectionRelease.store (std::max (0.1f, protection.releaseDbPerFrame), std::memory_order_relaxed);
    impl->protectionHold.store (protection.holdFrames, std::memory_order_relaxed);
    impl->protectionArm.store (std::max (1, protection.armFrames), std::memory_order_relaxed);
}

float DfnEngine::voice() const noexcept                 { return impl->voiceShared.load (std::memory_order_relaxed); }
void DfnEngine::setPostFilter (float beta) noexcept     { impl->postFilter.store (std::max (0.0f, beta), std::memory_order_relaxed); }
uint32_t DfnEngine::misses() const noexcept             { return impl->missCount.load (std::memory_order_relaxed); }
uint32_t DfnEngine::overruns() const noexcept           { return impl->overrunCount.load (std::memory_order_relaxed); }
float DfnEngine::workerLoad() const noexcept            { return impl->workerLoadShared.load (std::memory_order_relaxed); }
float DfnEngine::worstFrameMs() const noexcept          { return impl->worstShared.load (std::memory_order_relaxed); }

void DfnEngine::setTestWorkerDelayMicroseconds (int microseconds) noexcept
{
    impl->testDelayMicroseconds.store (microseconds, std::memory_order_relaxed);
}

void DfnEngine::setTestAttenuationLimitDb (float db) noexcept
{
    impl->attenuationLimit.store (db, std::memory_order_relaxed);
}

void DfnEngine::setTestLockstep (bool on) noexcept
{
    lockstepForTests.store (on, std::memory_order_relaxed);
}

juce::String DfnEngine::describeWorkerThread() const
{
    return Impl::describe ((Scheduling) impl->schedulingKind.load (std::memory_order_relaxed),
                           impl->schedulingValue.load (std::memory_order_relaxed));
}
}
