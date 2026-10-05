// DfnRuntime implementation: model lookup, the load-once cache and the C interface wrapper.
#include "engine/denoise/DfnRuntime.h"

#if HEADROOM_HAVE_DFN
 #include <hr_dfn.h>
#endif

#include <cmath>
#include <mutex>
#include <vector>

#if JUCE_LINUX
 #include <unistd.h>
#endif

namespace hr::dfn
{
namespace
{
    struct Cached
    {
        std::mutex loadMutex;                   // held while loading: one load per model, others wait
        std::mutex guard;                       // protects the fields below (never held while loading)
        std::shared_ptr<LoadedModel> model;
    };

    Cached& cacheFor (Model model)
    {
        static Cached entries[2];
        return entries[model == Model::fast ? 0 : 1];
    }
}

const char* fileName (Model model)
{
    return model == Model::fast ? "DeepFilterNet3_ll_onnx.tar.gz" : "DeepFilterNet3_onnx.tar.gz";
}

namespace
{
    // Where the models may be, best first. $HEADROOM_MODELS_DIR is exclusive (tests, unusual installs). Otherwise "models"
    // beside the executable, found through /proc/self/exe on Linux (the installed program is started through the symlink
    // ~/.local/bin/headroom, and JUCE reports the path it was started by), then through a link's target, then as started.
    std::vector<juce::File> candidateFolders()
    {
        const auto fromEnvironment = juce::SystemStats::getEnvironmentVariable ("HEADROOM_MODELS_DIR", {});

        if (fromEnvironment.isNotEmpty())
            return { juce::File (fromEnvironment) };

        std::vector<juce::File> folders;

       #if JUCE_LINUX
        char path[4096] {};
        const auto length = readlink ("/proc/self/exe", path, sizeof (path) - 1);

        if (length > 0)
            folders.push_back (juce::File (juce::String::fromUTF8 (path, (int) length)).getParentDirectory().getChildFile ("models"));
       #endif

        const auto executable = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        folders.push_back (executable.getLinkedTarget().getParentDirectory().getChildFile ("models"));
        folders.push_back (executable.getParentDirectory().getChildFile ("models"));
        return folders;
    }
}

juce::File getModelsFolder()
{
    const auto folders = candidateFolders();

    for (const auto& folder : folders)
        if (folder.isDirectory())
            return folder;

    return folders.front();
}

juce::File findModelFile (Model model)
{
    for (const auto& folder : candidateFolders())
        if (auto file = folder.getChildFile (fileName (model)); file.existsAsFile())
            return file;

    return {};
}

bool isAvailable (Model model)
{
    return isCompiledIn() && findModelFile (model) != juce::File();
}

// ---------------------------------------------------------------------------------------------------
// State / LoadedModel
// ---------------------------------------------------------------------------------------------------

#if HEADROOM_HAVE_DFN

State::~State()
{
    hr_df_state_free (static_cast<HrDfState*> (raw));
}

float State::process (const float* in, float* out) noexcept
{
    return hr_df_state_process (static_cast<HrDfState*> (raw), in, out);
}

void State::setPostFilter (float beta) noexcept
{
    hr_df_state_set_post_filter (static_cast<HrDfState*> (raw), beta);
}

void State::setAttenuationLimit (float db) noexcept
{
    hr_df_state_set_atten_limit (static_cast<HrDfState*> (raw), db);
}

void State::setThresholds (float minDb, float maxErbDb, float maxDfDb) noexcept
{
    hr_df_state_set_thresholds (static_cast<HrDfState*> (raw), minDb, maxErbDb, maxDfDb);
}

LoadedModel::LoadedModel (void* handle, int frameLength, int sampleRate, int latency)
    : raw (handle), frame (frameLength), rate (sampleRate), delay (latency)
{
}

LoadedModel::~LoadedModel()
{
    hr_df_model_free (static_cast<HrDfModel*> (raw));
}

std::unique_ptr<State> LoadedModel::createState() const
{
    const std::lock_guard<std::mutex> lock (createMutex);
    networkTaken.store (true);

    if (auto* s = hr_df_state_new (static_cast<const HrDfModel*> (raw)))
        return std::unique_ptr<State> (new State (s));

    return nullptr;
}

std::shared_ptr<LoadedModel> Models::get (Model model, juce::String& error)
{
    auto& entry = cacheFor (model);
    const std::lock_guard<std::mutex> loading (entry.loadMutex);

    {
        const std::lock_guard<std::mutex> lock (entry.guard);

        if (entry.model != nullptr)
            return entry.model;
    }

    const auto file = findModelFile (model);

    if (file == juce::File())
    {
        error = juce::String ("model file ") + fileName (model) + " not found in " + getModelsFolder().getFullPathName();
        return nullptr;
    }

    char message[256] {};
    auto* raw = hr_df_model_load (file.getFullPathName().toRawUTF8(), message, sizeof (message));

    if (raw == nullptr)
    {
        error = juce::String::fromUTF8 (message);
        return nullptr;
    }

    std::shared_ptr<LoadedModel> loaded (new LoadedModel (raw, (int) hr_df_model_frame_length (raw),
                                                          (int) hr_df_model_sample_rate (raw), (int) hr_df_model_latency (raw)));

    if (loaded->frameLength() <= 0 || loaded->sampleRate() != 48000)
    {
        error = "unexpected model format";
        return nullptr;
    }

    const std::lock_guard<std::mutex> lock (entry.guard);
    entry.model = loaded;
    return loaded;
}

#else

State::~State() = default;
float State::process (const float*, float*) noexcept    { return std::nanf (""); }
void State::setPostFilter (float) noexcept              {}
void State::setAttenuationLimit (float) noexcept        {}
void State::setThresholds (float, float, float) noexcept {}

LoadedModel::LoadedModel (void* handle, int frameLength, int sampleRate, int latency)
    : raw (handle), frame (frameLength), rate (sampleRate), delay (latency)
{
}

LoadedModel::~LoadedModel() = default;
std::unique_ptr<State> LoadedModel::createState() const { return nullptr; }

std::shared_ptr<LoadedModel> Models::get (Model, juce::String& error)
{
    error = "this copy of Headroom was built without DeepFilterNet";
    return nullptr;
}

#endif

std::shared_ptr<LoadedModel> Models::peek (Model model)
{
    auto& entry = cacheFor (model);
    const std::lock_guard<std::mutex> lock (entry.guard);
    return entry.model;
}
}
