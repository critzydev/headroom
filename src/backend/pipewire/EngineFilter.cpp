// Implementation of the engine pw_filter (ports, RT process, mic probe).
#include "EngineFilter.h"

namespace hr::pw
{

EngineFilter::EngineFilter (hr::AudioEngine& engineToDrive) : engine (engineToDrive) {}

EngineFilter::~EngineFilter()
{
    destroy();
}

std::string EngineFilter::appPortName (int slot, bool right)
{
    return "app" + std::to_string (slot) + (right ? "_R" : "_L");
}

std::string EngineFilter::inputSlotPortName (int slot, bool right)
{
    return "in" + std::to_string (slot) + (right ? "_R" : "_L");
}

std::string EngineFilter::inputPortName (int index)
{
    if (index < hr::kMaxAppChannels * 2)
        return appPortName (index / 2, (index % 2) == 1);

    if (index < hr::kMaxAppChannels * 2 + 2)
        return index == hr::kMaxAppChannels * 2 ? "mic_L" : "mic_R";

    const int inputIndex = index - (hr::kMaxAppChannels * 2 + 2);
    return inputSlotPortName (inputIndex / 2, (inputIndex % 2) == 1);
}

std::string EngineFilter::outputPortName (int index)
{
    static const char* const names[kOutputPorts] = { "monitor_L", "monitor_R", "stream_L", "stream_R", "micout_L", "micout_R" };
    return names[index];
}

bool EngineFilter::create (pw_core* core, int quantumFrames)
{
    if (filter != nullptr)
        return true;

    Props props;
    props.set (PW_KEY_NODE_NAME, kEngineName)
         .set (PW_KEY_NODE_DESCRIPTION, "Headroom Engine")
         .set (PW_KEY_MEDIA_TYPE, "Audio")
         .set (PW_KEY_MEDIA_CATEGORY, "Filter")
         .set (PW_KEY_MEDIA_ROLE, "DSP")
         .set (PW_KEY_OBJECT_LINGER, "false")
         .set (PW_KEY_NODE_LATENCY, std::to_string (quantumFrames) + "/48000");
    // No media.class on purpose: pavucontrol/KDE never see it and WirePlumber never auto-links it.

    filter = pw_filter_new (core, "headroom-engine", props.release());

    if (filter == nullptr)
    {
        pwLog ("could not create the engine filter");
        return false;
    }

    static const pw_filter_events events = []
    {
        pw_filter_events e {};
        e.version = PW_VERSION_FILTER_EVENTS;
        e.state_changed = &EngineFilter::onStateChangedCb;
        e.process = &EngineFilter::onProcess;
        return e;
    }();

    std::memset (&listener, 0, sizeof (listener));
    pw_filter_add_listener (filter, &listener, &events, this);
    listenerActive = true;

    for (int i = 0; i < kInputPorts; ++i)
    {
        Props portProps;
        portProps.set (PW_KEY_FORMAT_DSP, "32 bit float mono audio")
                 .set (PW_KEY_PORT_NAME, inputPortName (i));

        inPorts[i] = pw_filter_add_port (filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                                         sizeof (uint32_t), portProps.release(), nullptr, 0);
    }

    for (int i = 0; i < kOutputPorts; ++i)
    {
        Props portProps;
        portProps.set (PW_KEY_FORMAT_DSP, "32 bit float mono audio")
                 .set (PW_KEY_PORT_NAME, outputPortName (i));

        outPorts[i] = pw_filter_add_port (filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                                          sizeof (uint32_t), portProps.release(), nullptr, 0);
    }

    for (void* port : inPorts)
    {
        if (port == nullptr)
        {
            pwLog ("could not add an input port to the engine filter");
            destroy();
            return false;
        }
    }

    for (void* port : outPorts)
    {
        if (port == nullptr)
        {
            pwLog ("could not add an output port to the engine filter");
            destroy();
            return false;
        }
    }

    const int result = pw_filter_connect (filter, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0);

    if (result < 0)
    {
        pwLog ("could not connect the engine filter: %s", spa_strerror (result));
        destroy();
        return false;
    }

    pwLog ("engine filter created (%d frames)", quantumFrames);
    return true;
}

void EngineFilter::destroy()
{
    if (filter == nullptr)
        return;

    if (listenerActive)
    {
        spa_hook_remove (&listener);
        listenerActive = false;
    }

    auto* victim = filter;
    filter = nullptr;

    // Blocks until the RT thread has left process(), so the engine may be destroyed afterwards.
    pw_filter_destroy (victim);

    for (auto& port : inPorts)
        port = nullptr;

    for (auto& port : outPorts)
        port = nullptr;

    currentState = PW_FILTER_STATE_UNCONNECTED;
}

void EngineFilter::setQuantum (int frames)
{
    if (filter == nullptr)
        return;

    const std::string latency = std::to_string (frames) + "/48000";

    spa_dict_item item;
    item.key = PW_KEY_NODE_LATENCY;
    item.value = latency.c_str();

    spa_dict dict;
    dict.flags = 0;
    dict.n_items = 1;
    dict.items = &item;

    pw_filter_update_properties (filter, nullptr, &dict);
}

uint32_t EngineFilter::nodeId() const
{
    return filter != nullptr ? pw_filter_get_node_id (filter) : SPA_ID_INVALID;
}

bool EngineFilter::hasFailed() const
{
    return filter != nullptr && currentState == PW_FILTER_STATE_ERROR;
}

void EngineFilter::onStateChangedCb (void* data, pw_filter_state oldState, pw_filter_state newState, const char* error)
{
    auto* self = static_cast<EngineFilter*> (data);
    self->currentState = newState;

    pwLog ("engine filter %s -> %s%s%s", pw_filter_state_as_string (oldState), pw_filter_state_as_string (newState),
           error != nullptr ? ": " : "", error != nullptr ? error : "");

    if (self->onStateChanged)
        self->onStateChanged();
}

// -------------------------------------------------------------------------------------------------
// Mic probe (the watchdog itself is in AudioEngine)

uint32_t EngineFilter::resetMicProbe()
{
    engine.resetMicWatch();
    resetCycleMark = engine.getCycleCount();
    return ++probeToken;
}

MicProbeReading EngineFilter::readMicProbe (uint32_t token) const
{
    MicProbeReading reading;

    if (token != probeToken)
        return reading;

    // resetMicWatch() takes effect at the start of the RT thread's next cycle. Once two more cycles
    // have run, the counters belong to the new capture; before that they may still be stale.
    const auto cycles = engine.getCycleCount();

    if (cycles < resetCycleMark + 2)
        return reading;

    reading.valid = true;
    reading.cycles = cycles - resetCycleMark;
    reading.silentMs = (double) engine.micSilentMs.load (std::memory_order_relaxed);
    reading.liveCycles = engine.micLiveCycles.load (std::memory_order_relaxed);
    reading.seenSignal = engine.micSeenSignal.load (std::memory_order_relaxed) != 0;
    return reading;
}

// -------------------------------------------------------------------------------------------------
// RT thread

void EngineFilter::onProcess (void* data, spa_io_position* position)
{
    static_cast<EngineFilter*> (data)->process (position);
}

void EngineFilter::process (spa_io_position* position) noexcept
{
    if (position == nullptr)
        return;

    const auto frames = (uint32_t) position->clock.duration;

    if (frames == 0)
        return;

    uint32_t rate = position->clock.rate.denom;

    if (rate == 0)
        rate = 48000;

    hr::EngineIO io;
    io.numFrames = (int) frames;
    io.sampleRate = (double) rate;

    // nullptr = the port is not linked, which the engine treats as silence (inputs) or skips (outputs).
    for (int slot = 0; slot < hr::kMaxAppChannels; ++slot)
    {
        io.appIn[slot][0] = inputBuffer (slot * 2, frames);
        io.appIn[slot][1] = inputBuffer (slot * 2 + 1, frames);
    }

    io.micIn[0] = inputBuffer (hr::kMaxAppChannels * 2, frames);
    io.micIn[1] = inputBuffer (hr::kMaxAppChannels * 2 + 1, frames);

    for (int slot = 0; slot < hr::kMaxInputChannels; ++slot)
    {
        io.inputIn[slot][0] = inputBuffer (hr::kMaxAppChannels * 2 + 2 + slot * 2, frames);
        io.inputIn[slot][1] = inputBuffer (hr::kMaxAppChannels * 2 + 2 + slot * 2 + 1, frames);
    }

    io.monitorOut[0] = outputBuffer (0, frames);
    io.monitorOut[1] = outputBuffer (1, frames);
    io.streamOut[0] = outputBuffer (2, frames);
    io.streamOut[1] = outputBuffer (3, frames);
    io.micOut[0] = outputBuffer (4, frames);
    io.micOut[1] = outputBuffer (5, frames);

    engine.process (io);

    pubRate.store (rate, std::memory_order_relaxed);
    pubQuantum.store (frames, std::memory_order_relaxed);
}

} // namespace hr::pw
