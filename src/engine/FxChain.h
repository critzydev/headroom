// FxSlot (one hosted or built-in processor) and FxChain (immutable, ref-counted snapshot of a channel's slots).
#pragma once

#include "Juce.h"
#include "engine/EngineTypes.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace hr
{
// Wraps one juce::AudioProcessor for use from the RT thread. The processor is owned via shared_ptr so a slot
// can move between chain snapshots (reorder, add a neighbour) without being re-instantiated.
// Only the message thread ever drops the last reference: see AudioEngine::collectGarbage().
//
// RT guarantees of process(): no allocation, no locks, no frees. The slot
//  - publishes its crash-id in CrashGuard::currentRtSlot around processBlock,
//  - sanitises what the processor produced (non-finite -> 0, hard clip at +-4.0) so one misbehaving plugin
//    cannot poison the mix, and counts the blocks where that was necessary,
//  - turns bypass on and off with a short crossfade. Switching on first feeds the processor for a warm-up period
//    (its latency plus 40 ms) while the dry signal keeps playing, so latency effects and envelope followers are
//    warm before they become audible, then fades the result in over 10 ms. Fully bypassed slots cost nothing.
class FxSlot
{
public:
    FxSlot (std::shared_ptr<juce::AudioProcessor> processorToWrap, juce::String slotUid, uint32_t slotCrashId);

    // Message thread only. The first call negotiates a bus layout (stereo/stereo, else mono/stereo, else mono/mono,
    // else the processor's default) and prepares the processor. Later calls (sample-rate change) release and
    // prepare it again with the same layout and restart the warm-up. Never call while the RT thread may be inside
    // process(): the engine suspends itself around this.
    void configure (double sampleRate, int maxBlock);
    bool isConfigured() const noexcept              { return configured; }

    juce::AudioProcessor& getProcessor() const noexcept { return *processor; }
    const std::shared_ptr<juce::AudioProcessor>& getProcessorPtr() const noexcept { return processor; }
    const juce::String& getUid() const noexcept     { return uid; }
    uint32_t getCrashId() const noexcept            { return crashId; }
    int getNumInputChannels() const noexcept        { return numIn; }
    int getNumOutputChannels() const noexcept       { return numOut; }

    std::atomic<bool> bypassed { false };           // written by the message thread, read by RT

    // Any thread. Number of blocks in which the processor's output contained non-finite or > +-4.0 samples.
    uint32_t getSanitisedBlocks() const noexcept    { return sanitisedBlocks.load (std::memory_order_relaxed); }

    // Any thread. True once processBlock threw: the slot is then bypassed for good (until re-configured).
    bool hasFaulted() const noexcept                { return faulted.load (std::memory_order_relaxed); }

    // RT thread. Processes l/r in place. Mono-only processors get L and their output is copied to R.
    void process (float* l, float* r, int numFrames) noexcept;

private:
    void negotiateLayout();
    void processChunk (float* l, float* r, int n) noexcept;
    int computeWarmupSamples() const noexcept;

    std::shared_ptr<juce::AudioProcessor> processor;
    const juce::String uid;
    const uint32_t crashId;

    bool configured = false;
    int numIn = 2, numOut = 2;
    juce::AudioBuffer<float> work;      // max(ins, outs, 2) channels x maxBlock, preallocated in configure()
    juce::MidiBuffer midi;

    // RT-only state (the message thread touches it only in configure(), while the engine is suspended).
    float wetMix = 0.0f;                // 0 = dry, 1 = fully processed
    bool warm = false;                  // the processor has been fed since the slot last left bypass
    int warmupLeft = 0;
    double preparedRate = 48000.0;
    int warmupSamples = 0;
    float rampStep = 0.0f;              // per-sample change of wetMix

    std::atomic<uint32_t> sanitisedBlocks { 0 };
    std::atomic<bool> faulted { false };
};

using FxSlotPtr = std::shared_ptr<FxSlot>;

// Immutable list of slots. Built on the message thread, published to the RT thread through an atomic
// pointer in ChannelProcessor, retired again on the message thread (see AudioEngine::setChain).
class FxChain : public juce::ReferenceCountedObject
{
public:
    using Ptr = juce::ReferenceCountedObjectPtr<FxChain>;

    static Ptr create (std::vector<FxSlotPtr> slots)    { return Ptr (new FxChain (std::move (slots))); }

    const std::vector<FxSlotPtr>& getSlots() const noexcept    { return slots; }
    bool isEmpty() const noexcept                               { return slots.empty(); }

    // RT thread. Runs every slot in order, in place (bypassed slots pass the audio through).
    void process (float* l, float* r, int numFrames) noexcept;

    // Message thread, engine suspended: re-prepare every slot for a new sample rate.
    void reprepare (double sampleRate);

private:
    explicit FxChain (std::vector<FxSlotPtr> s) : slots (std::move (s)) {}

    const std::vector<FxSlotPtr> slots;
};
}
