// DfnRuntime: finds and loads the DeepFilterNet model files (once per process) and wraps dfn-shim's C interface.
#pragma once

#include "Juce.h"

#include <atomic>
#include <memory>
#include <mutex>

#ifndef HEADROOM_HAVE_DFN
 #define HEADROOM_HAVE_DFN 0
#endif

namespace hr::dfn
{
// The two networks Headroom ships (models/ next to the program): DeepFilterNet3 low latency (no look-ahead) for
// "Fast", DeepFilterNet3 (two frames of look-ahead) for "Best".
enum class Model { fast, best };

// False when the program was built without DeepFilterNet (-DHEADROOM_DFN=OFF): nothing below can load anything.
constexpr bool isCompiledIn() noexcept      { return HEADROOM_HAVE_DFN != 0; }

const char* fileName (Model model);

// $HEADROOM_MODELS_DIR if set, else "models" next to the executable (build folder, or ~/.local/opt/headroom/, also when the
// program was started through a symlink such as ~/.local/bin/headroom).
juce::File getModelsFolder();

// The model file, or a File() that does not exist when it is not there.
juce::File findModelFile (Model model);

// Compiled in and the file is there (says nothing about whether it loads).
bool isAvailable (Model model);

// One running stream (mono, 48 kHz): owns the network's recurrent state. One thread at a time.
class State
{
public:
    ~State();

    // One frame of frameLength() samples. Returns the local SNR in dB (-15 .. 35), NaN if the state has failed
    // (the input is then copied through). Allocates internally: never call it from the audio thread.
    float process (const float* in, float* out) noexcept;
    void setPostFilter (float beta) noexcept;
    void setAttenuationLimit (float db) noexcept;               // >= 100: none (the default); tests use it to get the input back

    // libDF's local-SNR thresholds (dB, of the network's own estimate per frame, -15 .. 35): below `minDb` the frame is muted,
    // above `maxErbDb` it is passed untouched, above `maxDfDb` the deep-filtering stage is skipped (see hr_dfn.h). A new
    // state has libDF's defaults: -10, 30, 20.
    void setThresholds (float minDb, float maxErbDb, float maxDfDb) noexcept;

private:
    friend class LoadedModel;
    explicit State (void* handle) : raw (handle) {}

    void* raw;
    JUCE_DECLARE_NON_COPYABLE (State)
};

// A model that is loaded: the first stream created from it takes the parsed network (instant), each further one parses
// the file again (0.15-0.5 s). Streams cannot share a network because tract counts references without atomics.
class LoadedModel
{
public:
    ~LoadedModel();

    int frameLength() const noexcept        { return frame; }       // 480
    int sampleRate() const noexcept         { return rate; }        // 48000
    // Samples between an input sample and its output sample, counted from the frame that holds it having been handed
    // in: 480 (Fast) or 1440 (Best).
    int latency() const noexcept            { return delay; }

    // True while the next createState() is instant (nobody has taken the parsed network yet).
    bool isInstant() const noexcept         { return ! networkTaken.load(); }

    // Instant the first time, then as slow as loading: a background thread only. nullptr on failure.
    std::unique_ptr<State> createState() const;

private:
    friend struct Models;
    LoadedModel (void* handle, int frameLength, int sampleRate, int latency);

    void* raw;
    int frame, rate, delay;
    mutable std::mutex createMutex;
    mutable std::atomic<bool> networkTaken { false };
    JUCE_DECLARE_NON_COPYABLE (LoadedModel)
};

// Process-wide cache. Loading parses the network (Fast ~0.5 s, Best ~0.15 s): only from a background thread.
struct Models
{
    // Loads (or returns the cached) model; concurrent callers for the same model wait for one load.
    // nullptr with `error` set if it is not available or cannot be loaded.
    static std::shared_ptr<LoadedModel> get (Model model, juce::String& error);

    // The cached model if it is loaded right now (never blocks), else nullptr.
    static std::shared_ptr<LoadedModel> peek (Model model);

};
}
