// Noise Removal built-in effect (id "noise"): DeepFilterNet (Fast, Best) or RNNoise (Light) with a sample-exact wet/dry blend.
#pragma once

#include "engine/builtin/BuiltinProcessor.h"
#include "engine/denoise/DenoiseChain.h"
#include "engine/denoise/NoiseMeters.h"
#include "engine/denoise/SpectrumTap.h"
#include "engine/denoise/SpscQueue.h"

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

namespace hr
{
// One effect, three engines (parameter "mode"; the default is Best):
//
//   Fast   DeepFilterNet3 low latency: no look-ahead, 30 ms in total (see denoise::DfnEngine for where they go).
//   Best   DeepFilterNet3: two frames of look-ahead, 50 ms, the cleanest and (smaller network) the lightest on the CPU.
//   Light  RNNoise on the audio thread, 30 ms, a fraction of the CPU, with the voice gate (Voice threshold, Hold).
//
// "strength" (0-100 %) is how much of the noise may be removed, as an attenuation limit: 0 % = nothing (the input,
// delayed), 100 % = at most 30 dB, 80 % = 24 dB. The limit is applied as a sample-exact blend of the aligned input
// into the engine's output, which keeps some natural room tone and avoids the "watery" sound of full suppression.
//
// Neither DeepFilterNet mode cuts the voice: libDF's shortcuts by its own local-SNR estimate are off, and a short
// speech protection keeps what the network takes for noise right after speech from being attenuated by more than 9-12 dB.
//
// Mode changes are click-free: the new engine is built on a background thread (and DeepFilterNet's model is loaded
// there, ~0.5 s the first time), fed with the live input until it has settled, then cross-faded in over 20 ms.
// Until it is ready, and for good if the model files are missing or the program was built without DeepFilterNet,
// Light runs instead: getStatus() says which.
//
// The latency reported through setLatencySamples() is the one of the engine that is running (it changes after a
// cross-fade). Rates outside 8-192 kHz pass the audio through untouched (latency 0); getRateMode() tells the UI.
//
// Real-time rules: processBlock() never allocates, locks or waits. Chains (engine + buffers) are built off the audio
// thread, handed over through an atomic pointer and handed back through a wait-free queue to be destroyed.
class NoiseRemoval final : public BuiltinProcessor,
                           private juce::AudioProcessorValueTreeState::Listener
{
public:
    static constexpr const char* kId = "noise";
    static constexpr int kFrame = denoise::kFrame;              // 480 samples: one frame of every engine
    static constexpr int kNativeLatency = 3 * kFrame;           // Light at 48 kHz

    enum class Mode { fast = 0, best = 1, light = 2 };          // the order of the "mode" choice parameter
    enum class RateMode { native, resampled, unsupported };
    enum class Status { ready, loading, unavailable };
    enum class Problem { none, notCompiledIn, modelsMissing, loadFailed };

    NoiseRemoval();
    ~NoiseRemoval() override;

    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    // ---- The setting ------------------------------------------------------------------------------
    // Strength (0-100 %) -> attenuation limit in dB (0..30) and the share of the input left in the output.
    static float attenuationDbForStrength (float strengthPercent) noexcept;
    static float dryMixForStrength (float strengthPercent) noexcept;
    static constexpr float kMaxAttenuationDb = 30.0f;
    static constexpr float kMaxPostFilterBeta = 0.05f;          // Post-filter 100 %

    Mode getRequestedMode() const noexcept;
    static const char* getModeName (Mode mode) noexcept;
    static int getNominalLatencySamples (Mode mode) noexcept;   // at 48 kHz, for captions

    // ---- State of the engine ----------------------------------------------------------------------
    // Any thread. Which engine is running (Light while another one is still being prepared, or is unavailable).
    Mode getActiveMode() const noexcept;
    Status getStatus() const noexcept;
    Problem getProblem() const noexcept;                        // why the requested mode is unavailable
    juce::String getStatusNote() const;                         // one plain sentence for the panel, empty when all is well

    // Blocks until the requested engine is running or has failed (true = running). Only while the audio thread is
    // not inside processBlock() (tests, screenshots before audio starts): it adopts a finished engine directly,
    // without the cross-fade.
    bool waitUntilReady (int timeoutMs);

    // Any thread. Where the mode-switch is: an engine has been built and waits for, or is in, its cross-fade.
    bool isSwitching() const noexcept;

    // ---- Meters (any thread, cheap) ---------------------------------------------------------------
    float getMeterValue() const noexcept override               { return meters.getVoiceProbability(); }
    float getReductionDb() const noexcept                       { return meters.getReductionDb(); }
    float getNoiseFloorDb() const noexcept                      { return meters.getNoiseFloorDb(); }
    float getVoiceLevelDb() const noexcept                      { return meters.getVoiceLevelDb(); }
    bool isVoiceActive() const noexcept                         { return meters.isVoiceActive(); }
    float getGateGain() const noexcept                          { return gateLevel.load (std::memory_order_relaxed); }

    // Share of one core the effect uses (audio thread + the network's own thread), and how many frames the network
    // was late for (DeepFilterNet answers are played from a queue, see denoise::DfnEngine).
    float getCpuLoad() const noexcept                           { return cpuAudio.load (std::memory_order_relaxed) + cpuWorker.load (std::memory_order_relaxed); }
    float getWorstFrameMs() const noexcept                      { return worstFrame.load (std::memory_order_relaxed); }
    uint32_t getMissedFrames() const noexcept                   { return missed.load (std::memory_order_relaxed); }

    // ---- The panel's spectrum: the audio thread fills it only while `active` is set ----------------
    denoise::SpectrumTap& getSpectrumTap() noexcept             { return tap; }

    // ---- "Hear what's removed" (momentary) ----------------------------------------------------------
    // While on, the output is the input minus the cleaned signal, sample-aligned. The app must also route it to the
    // headphones and keep it away from Discord: requestHearRemoved() asks it to (onHearRemovedRequest, set by
    // AppController) and switches the effect. The panel calls it on press and release.
    void requestHearRemoved (bool on);
    void setHearRemoved (bool on) noexcept                      { hearRemoved.store (on, std::memory_order_relaxed); }
    bool isHearingRemoved() const noexcept                      { return hearRemoved.load (std::memory_order_relaxed); }
    std::function<void (bool)> onHearRemovedRequest;

    // ---- Rates ------------------------------------------------------------------------------------
    // native = 48 kHz; resampled = other rates in 8-192 kHz; unsupported = audio passes through.
    RateMode getRateMode() const noexcept                       { return rateMode.load (std::memory_order_relaxed); }
    bool isPassingThrough() const noexcept                      { return getRateMode() == RateMode::unsupported; }

    // ---- Testing hooks (any thread) ----------------------------------------------------------------
    void setForceStereo (bool force) noexcept                   { forceStereo.store (force, std::memory_order_relaxed); }
    uint32_t getUnderruns() const noexcept                      { return underruns.load (std::memory_order_relaxed); }
    // The running engine (only while the audio thread is idle).
    denoise::Denoiser48* getActiveEngineForTesting() noexcept   { return active != nullptr ? &active->getEngine() : nullptr; }

private:
    class Builder;
    enum class Phase { steady, priming, fading };
    using Chain = denoise::DenoiseChain;

    static Layout createLayout();
    void parameterChanged (const juce::String& parameterId, float newValue) override;

    static Chain::Kind kindFor (Mode mode) noexcept;
    static Mode modeFor (Chain::Kind kind) noexcept;

    // Any thread. Builds the chain for `kind` at `rate`; blocks while a model loads.
    static std::unique_ptr<Chain> makeChain (Chain::Kind kind, double rate, Problem& problem, juce::String& detail);

    // Message thread.
    void requestBuild();
    void discardChains();
    void adoptPendingWhileIdle();

    // Builder thread.
    void builderStep();
    void recordProblem (Chain::Kind kind, Problem problem) noexcept;

    // Audio thread.
    void processChunk (float* l, float* r, int n) noexcept;
    void adoptPending() noexcept;
    void finishTransition() noexcept;
    void retire (Chain* chain) noexcept;
    void publishActive() noexcept;

    static constexpr int kChunk = Chain::kMaxChunk;
    static constexpr int kGateLeadFrames = 2;                   // the network sees ~20 ms ahead of the audio it gates

    std::atomic<float> *pMode, *pStrength, *pThreshold, *pHold, *pPostFilter;

    // Chains. `active`, `incoming` and `stuck` belong to the audio thread while it runs; the message thread touches
    // them only in prepareToPlay / releaseResources / waitUntilReady (audio thread idle).
    Chain* active = nullptr;
    Chain* incoming = nullptr;
    Chain* stuck = nullptr;                                     // retired but the queue was full: try again
    std::atomic<Chain*> pending { nullptr };                    // builder -> audio thread
    denoise::SpscQueue<Chain*, 16> retired;                     // audio thread -> builder (destroys them)
    Phase phase = Phase::steady;
    int phaseLeft = 0, fadeDone = 0, fadeLength = 1, primeExtra = 0;

    std::atomic<double> sampleRate { 0.0 };
    std::atomic<RateMode> rateMode { RateMode::native };
    std::atomic<int> activeKind { (int) Chain::Kind::light };
    std::atomic<int> activeLatency { 0 };
    std::atomic<bool> latencyDirty { false };
    std::atomic<int> problemState { 0 };                        // kind * 8 + problem, 0 = none

    // Builder
    std::unique_ptr<Builder> builder;
    std::mutex requestMutex;
    bool buildWanted = false;
    Chain::Kind wantedKind = Chain::Kind::best;
    double wantedRate = 48000.0;
    uint64_t generation = 0;
    Chain::Kind lastPublishedKind = Chain::Kind::light;

    // Mixing
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> strength { 0.8f }, hearMix { 0.0f };
    bool needsSnap = true;
    std::atomic<bool> forceStereo { false };
    std::atomic<bool> hearRemoved { false };
    std::array<std::array<float, kChunk>, 2> outA {}, dryA {}, outB {}, dryB {};
    std::array<float, kChunk> mixRamp {}, hearRamp {};

    // Meters and taps
    denoise::NoiseMeters meters;
    denoise::SpectrumTap tap;
    std::atomic<float> gateLevel { 1.0f };
    std::atomic<float> cpuAudio { 0.0f }, cpuWorker { 0.0f }, worstFrame { 0.0f };
    std::atomic<uint32_t> underruns { 0 }, missed { 0 };
    float cpuSmoothed = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NoiseRemoval)
};
}
