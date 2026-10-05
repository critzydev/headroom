// NullBackend: demo devices, fake app streams routed by the real rules, and synthetic audio so meters move.
#include "backend/NullBackend.h"

#include "backend/DeviceRules.h"
#include "backend/RuleMatcher.h"
#include "backend/wave/WaveGain.h"
#include "engine/AudioEngine.h"

#include <cmath>

namespace hr
{
namespace
{
    constexpr double kRate = 48000.0;
    constexpr int kBlock = 256;                     // frames per engine.process() call
    constexpr float kTwoPi = 6.28318530717958647692f;

    const char* const kWaveInput   = "alsa_input.usb-Elgato_Systems_Elgato_Wave_3_DEMO-00.mono-fallback";
    const char* const kWebcamInput = "alsa_input.usb-Generic_USB_Webcam_DEMO-02.analog-stereo";
    const char* const kWaveOutput  = "alsa_output.usb-Elgato_Systems_Elgato_Wave_3_DEMO-00.analog-stereo";
    const char* const kHdmiOutput  = "alsa_output.pci-0000_01_00.1.hdmi-stereo";

    struct OnePole
    {
        float z = 0.0f;
        float lowpass (float x, float a) noexcept   { z += a * (x - z); return z; }
    };

    // Each source renders `n` stereo samples of a recognisable, non-repeating-looking signal.
    class Source
    {
    public:
        virtual ~Source() = default;
        virtual void render (float* l, float* r, int n) noexcept = 0;

    protected:
        float noise() noexcept  { return rng.nextFloat() * 2.0f - 1.0f; }
        juce::Random rng;
    };

    // Filtered noise under a slow swell, decorrelated left/right.
    class MusicSource final : public Source
    {
    public:
        MusicSource() { rng.setSeed (11); }

        void render (float* l, float* r, int n) noexcept override
        {
            for (int i = 0; i < n; ++i)
            {
                const float env = 0.55f + 0.45f * std::sin (kTwoPi * (float) t);
                t += 0.32 / kRate;
                if (t > 1.0) t -= 1.0;

                l[i] = lpL.lowpass (noise(), 0.07f) * 0.5f * env;     // peaks about -12 dBFS at the top of the swell
                r[i] = lpR.lowpass (noise(), 0.07f) * 0.5f * env;
            }
        }

    private:
        double t = 0.0;
        OnePole lpL, lpR;
    };

    // Bursts of band-limited noise with a fast decay (explosions, footsteps).
    class GameSource final : public Source
    {
    public:
        GameSource() { rng.setSeed (22); untilBurst = (int) (0.4 * kRate); }

        void render (float* l, float* r, int n) noexcept override
        {
            for (int i = 0; i < n; ++i)
            {
                if (--untilBurst <= 0)
                {
                    env = 0.6f + 0.4f * rng.nextFloat();
                    untilBurst = (int) ((0.5 + 2.0 * rng.nextFloat()) * kRate);
                }

                env *= 0.99985f;
                const float rumble = 0.03f * noise();
                l[i] = lpL.lowpass (noise(), 0.22f) * 0.65f * env + rumble;     // bursts -13 ... -9 dBFS
                r[i] = lpR.lowpass (noise(), 0.22f) * 0.65f * env + rumble;
            }
        }

    private:
        float env = 0.0f;
        int untilBurst = 0;
        OnePole lpL, lpR;
    };

    // Voiced sawtooth + breath noise under a syllable envelope with gaps: reads as "someone talking".
    class SpeechSource final : public Source
    {
    public:
        SpeechSource (int seed, float basePitchHz, float level) : pitch (basePitchHz), gain (level)
        {
            rng.setSeed (seed);
            startSegment();
        }

        void render (float* l, float* r, int n) noexcept override
        {
            for (int i = 0; i < n; ++i)
            {
                if (--remaining <= 0)
                    startSegment();

                env += (target - env) * 0.004f;

                vibrato += 5.5 / kRate;
                if (vibrato > 1.0) vibrato -= 1.0;

                const float f0 = pitch * (1.0f + 0.06f * std::sin (kTwoPi * (float) vibrato)) * (0.9f + 0.2f * segmentPitch);
                phase += f0 / kRate;
                if (phase > 1.0) phase -= 1.0;

                const float saw = (float) (2.0 * phase - 1.0);
                const float voiced = formant2.lowpass (formant1.lowpass (saw, 0.09f), 0.35f) * 3.2f;
                const float breath = breathLp.lowpass (noise(), 0.3f) * 0.5f;
                const float s = (voiced + breath) * env * gain;

                l[i] = s;
                r[i] = s;
            }
        }

    private:
        void startSegment() noexcept
        {
            if (inSyllable)
            {
                inSyllable = false;
                target = 0.0f;
                ++syllables;
                const float gap = (syllables % 5 == 0) ? 0.6f + 0.6f * rng.nextFloat() : 0.03f + 0.12f * rng.nextFloat();
                remaining = (int) (gap * kRate);
            }
            else
            {
                inSyllable = true;
                target = 0.4f + 0.6f * rng.nextFloat();
                segmentPitch = rng.nextFloat();
                remaining = (int) ((0.12f + 0.23f * rng.nextFloat()) * kRate);
            }
        }

        const float pitch, gain;
        bool inSyllable = false;
        int remaining = 0, syllables = 0;
        float env = 0.0f, target = 0.0f, segmentPitch = 0.5f;
        double phase = 0.0, vibrato = 0.0;
        OnePole formant1, formant2, breathLp;
    };

    // The occasional notification blip.
    class SystemSource final : public Source
    {
    public:
        SystemSource() { rng.setSeed (44); untilClick = (int) (0.8 * kRate); }

        void render (float* l, float* r, int n) noexcept override
        {
            for (int i = 0; i < n; ++i)
            {
                if (--untilClick <= 0)
                {
                    env = 1.0f;
                    phase = 0.0;
                    untilClick = (int) ((2.0 + 3.0 * rng.nextFloat()) * kRate);
                }

                phase += 1200.0 / kRate;
                if (phase > 1.0) phase -= 1.0;

                env *= 0.9993f;
                const float s = std::sin (kTwoPi * (float) phase) * env * 0.2f;      // a soft blip, about -14 dBFS
                l[i] = s;
                r[i] = s;
            }
        }

    private:
        float env = 0.0f;
        double phase = 0.0;
        int untilClick = 0;
    };

    struct FakeApp { uint32_t nodeId; const char* appName; const char* binary; const char* media; const char* icon; };
}

// ---------------------------------------------------------------------------------------------------
// Signal thread
// ---------------------------------------------------------------------------------------------------

class NullBackend::SignalThread final : public juce::Thread
{
public:
    SignalThread (NullBackend& o, AudioEngine& e)
        : juce::Thread ("Headroom demo audio"), owner (o), engine (e),
          speechMic (55, 125.0f, 0.09f)
    {
        sources[(int) Kind::Music]  = std::make_unique<MusicSource>();
        sources[(int) Kind::Game]   = std::make_unique<GameSource>();
        sources[(int) Kind::Chat]   = std::make_unique<SpeechSource> (33, 170.0f, 0.08f);
        sources[(int) Kind::System] = std::make_unique<SystemSource>();

        // A second voice for every input slot that has a device: each one a little different, so that two inputs are not one.
        for (int k = 0; k < kMaxInputChannels; ++k)
            inputVoices[(size_t) k] = std::make_unique<SpeechSource> (60 + k * 7, 105.0f + 22.0f * (float) k, 0.08f);
    }

    void run() override
    {
        const double blockMs = 1000.0 * kBlock / kRate;
        double next = juce::Time::getMillisecondCounterHiRes();

        while (! threadShouldExit())
        {
            const double now = juce::Time::getMillisecondCounterHiRes();

            if (now < next)
            {
                juce::Thread::sleep (1);
                continue;
            }

            if (now - next > 200.0)     // stalled (debugger, suspend): resync rather than racing to catch up
                next = now;

            next += blockMs;
            renderBlock();
        }
    }

private:
    void renderBlock() noexcept
    {
        for (int k = 0; k < 4; ++k)
            sources[(size_t) k]->render (kindBuf[(size_t) k][0].data(), kindBuf[(size_t) k][1].data(), kBlock);

        if (owner.micMuted)
        {
            micBuf.fill (0.0f);     // a muted Wave:3 sends exact digital silence
        }
        else
        {
            speechMic.render (micBuf.data(), scratch.data(), kBlock);

            // A real room is never silent between words: a fan (low-passed noise and its hum) and a little hiss, about
            // -47 dBFS. Gives the noise removal something to remove in the demo and in the screenshots.
            for (auto& v : micBuf)
            {
                humPhase += 120.0 / kRate;
                if (humPhase > 1.0) humPhase -= 1.0;

                const float white = roomRng.nextFloat() * 2.0f - 1.0f;
                v += fanLp.lowpass (roomRng.nextFloat() * 2.0f - 1.0f, 0.03f) * 0.05f + 0.0014f * std::sin (kTwoPi * (float) humPhase) + 0.0012f * white;
            }
        }

        EngineIO io;
        io.numFrames = kBlock;
        io.sampleRate = kRate;

        for (int slot = 0; slot < kMaxAppChannels; ++slot)
        {
            const auto mask = owner.slotKinds[(size_t) slot].load (std::memory_order_relaxed);

            if (mask == 0)
                continue;       // nothing playing into this channel: port stays "unlinked"

            auto& l = slotBuf[(size_t) slot][0];
            auto& r = slotBuf[(size_t) slot][1];
            l.fill (0.0f);
            r.fill (0.0f);

            for (int k = 0; k < 4; ++k)
            {
                if ((mask & (1u << k)) == 0)
                    continue;

                for (int i = 0; i < kBlock; ++i)
                {
                    l[(size_t) i] += kindBuf[(size_t) k][0][(size_t) i];
                    r[(size_t) i] += kindBuf[(size_t) k][1][(size_t) i];
                }
            }

            io.appIn[slot][0] = l.data();
            io.appIn[slot][1] = r.data();
        }

        for (int k = 0; k < kMaxInputChannels; ++k)
        {
            if (owner.inputLive[(size_t) k].load (std::memory_order_relaxed) == 0)
                continue;       // no device behind this input channel: the slot stays "unlinked"

            auto& v = inputBuf[(size_t) k];
            inputVoices[(size_t) k]->render (v[0].data(), v[1].data(), kBlock);
            io.inputIn[k][0] = v[0].data();
            io.inputIn[k][1] = v[1].data();
        }

        io.micIn[0] = micBuf.data();        // mono like the real Wave:3
        io.monitorOut[0] = outBuf[0].data();  io.monitorOut[1] = outBuf[1].data();
        io.streamOut[0]  = outBuf[2].data();  io.streamOut[1]  = outBuf[3].data();
        io.micOut[0]     = outBuf[4].data();  io.micOut[1]     = outBuf[5].data();

        engine.process (io);
    }

    NullBackend& owner;
    AudioEngine& engine;

    std::array<std::unique_ptr<Source>, 4> sources;
    std::array<std::unique_ptr<Source>, kMaxInputChannels> inputVoices;
    SpeechSource speechMic;
    OnePole fanLp;
    juce::Random roomRng { 77 };
    double humPhase = 0.0;

    std::array<std::array<std::array<float, kBlock>, 2>, 4> kindBuf {};
    std::array<std::array<std::array<float, kBlock>, 2>, kMaxAppChannels> slotBuf {};
    std::array<std::array<std::array<float, kBlock>, 2>, kMaxInputChannels> inputBuf {};
    std::array<float, kBlock> micBuf {}, scratch {};
    std::array<std::array<float, kBlock>, 6> outBuf {};     // outputs go nowhere, but the engine must fill them
};

// Coalesces onChanged notifications and delivers them on the message thread, never re-entrantly.
struct NullBackend::Notifier final : public juce::AsyncUpdater
{
    explicit Notifier (NullBackend& o) : owner (o) {}
    ~Notifier() override                { cancelPendingUpdate(); }

    void handleAsyncUpdate() override   { if (owner.onChanged) owner.onChanged(); }

    NullBackend& owner;
};

// ---------------------------------------------------------------------------------------------------
// NullBackend
// ---------------------------------------------------------------------------------------------------

NullBackend::NullBackend (bool micMutedOnDevice)
    : micMuted (micMutedOnDevice), notifier (std::make_unique<Notifier> (*this))
{
    for (auto& mask : slotKinds)
        mask.store (0);

    for (auto& live : inputLive)
        live.store (0);

    static const FakeApp apps[]
    {
        { 101, "Discord",         "discord",           "Voice chat",  "discord" },
        { 102, "Spotify",         "spotify",           "Now playing", "spotify" },
        { 103, "Firefox",         "firefox",           "Video",       "firefox" },
        { 104, "Cyberpunk 2077",  "cyberpunk2077.exe", "Game audio",  "steam_app_1091500" },
    };

    const Kind kinds[] { Kind::Chat, Kind::Music, Kind::System, Kind::Game };

    for (size_t i = 0; i < std::size (apps); ++i)
    {
        FakeStream s;
        s.info.nodeId = apps[i].nodeId;
        s.info.appName = apps[i].appName;
        s.info.binary = apps[i].binary;
        s.info.mediaName = apps[i].media;
        s.info.iconName = apps[i].icon;
        s.kind = kinds[i];
        streams.push_back (std::move (s));
    }
}

NullBackend::~NullBackend()
{
    stop();
}

bool NullBackend::start (AudioEngine& engine)
{
    if (running)
        return true;

    thread = std::make_unique<SignalThread> (*this, engine);
    thread->startThread (juce::Thread::Priority::high);
    running = true;

    reroute();
    return true;
}

void NullBackend::stop()
{
    if (thread != nullptr)
    {
        thread->stopThread (2000);
        thread.reset();
    }

    running = false;
}

void NullBackend::setChannels (const std::vector<ChannelEndpoint>& newEndpoints)
{
    endpoints = newEndpoints;
    reroute();
}

void NullBackend::setDevices (const juce::String& newInputId, const juce::StringArray& headphones, const juce::StringArray& stream)
{
    inputId = newInputId;
    headphoneIds = headphones;
    streamIds = stream;
    notifyChanged();
}

void NullBackend::setInputChannels (const std::vector<InputEndpoint>& endpointsNow)
{
    inputEndpoints = endpointsNow;
    refreshInputSlots();
    notifyChanged();
}

// An input channel whose device is one of the demo's has a signal; one whose device is not (an unplugged mic) stays silent
// and says "not connected".
void NullBackend::refreshInputSlots()
{
    std::array<uint32_t, kMaxInputChannels> live {};
    const auto devices = getInputs();

    for (const auto& endpoint : inputEndpoints)
        for (const auto& device : devices)
            if (device.id == endpoint.deviceId && juce::isPositiveAndBelow (endpoint.slot, kMaxInputChannels))
                live[(size_t) endpoint.slot] = 1;

    for (size_t i = 0; i < live.size(); ++i)
        inputLive[i].store (live[i]);
}

void NullBackend::setRoutingRules (const std::vector<RoutingRule>& newRules, const juce::String& fallbackChannelId)
{
    rules = newRules;
    fallbackChannel = fallbackChannelId;
    reroute();
}

void NullBackend::setMakeDefault (bool shouldMakeDefault)
{
    // Nothing to change: there is no system default here. Only remembered.
    makeDefault = shouldMakeDefault;
}

void NullBackend::setKeepDefault (bool shouldKeepDefault)
{
    keepDefault = shouldKeepDefault;
}

void NullBackend::setLatencyQuantum (int frames)
{
    quantum = frames;
    notifyChanged();
}

void NullBackend::routeApp (uint32_t nodeId, const juce::String& channelId)
{
    for (auto& s : streams)
        if (s.info.nodeId == nodeId)
            s.info.channelId = channelId;

    refreshSlotMasks();
    notifyChanged();
}

void NullBackend::runMicRecovery()
{
    juce::Logger::writeToLog ("NullBackend: runMicRecovery() (nothing to recover)");
}

void NullBackend::setHardwareMicGain (float linear0to1)
{
    hardwareMicGain = linear0to1;
    notifyChanged();        // like the real one: the new value comes back to the model as a status change
}

void NullBackend::setHardwareHeadphoneVolume (float linear0to1)  { hardwareHeadphoneVolume = linear0to1; }

void NullBackend::setClipGuard (bool on)
{
    // Like the real backend: the state to keep the mic in. Without permission it waits for it.
    desiredClipGuard = on;

    if (usbPermission)
        clipGuardOn = on;

    notifyChanged();
}

void NullBackend::requestHardwarePermission()
{
    usbPermission = true;

    if (desiredClipGuard.has_value())
        clipGuardOn = *desiredClipGuard;

    notifyChanged();
}

void NullBackend::setDemoUsbGain (std::optional<float> db, std::optional<bool> locked)
{
    demoUsbGainDb = db;
    demoUsbGainLocked = locked;
    notifyChanged();
}

void NullBackend::revokeHardwarePermission()
{
    usbPermission = false;
    notifyChanged();
}

void NullBackend::setDemoMicPresent (bool present, float gainOnReturn)
{
    if (present && ! demoMicPresent)
        hardwareMicGain = gainOnReturn;

    demoMicPresent = present;
    notifyChanged();
}

HardwareControls NullBackend::getHardwareControls() const
{
    HardwareControls controls;
    controls.usbAvailable = resolveDeviceLabel (getInputs(), inputId) == "Elgato Wave:3";       // the fake Wave:3 is the input
    controls.usbPermission = usbPermission;

    if (usbPermission)
    {
        controls.clipGuard = clipGuardOn;
        controls.gainLocked = demoUsbGainLocked;

        if (demoUsbGainDb.has_value())
            controls.gainDb = demoUsbGainDb;
        else if (demoMicPresent)
            controls.gainDb = (float) wave::sliderToHardwareDb ((double) hardwareMicGain);      // a mic that took what it was asked
    }

    return controls;
}

std::vector<DeviceInfo> NullBackend::getInputs() const
{
    return { { kWaveInput, "Elgato Wave:3", true, 1 }, { kWebcamInput, "USB Webcam Mic", false, 2 } };
}

std::vector<DeviceInfo> NullBackend::getOutputs() const
{
    return { { kWaveOutput, "Elgato Wave:3 Headphones", true, 2 }, { kHdmiOutput, "HDMI Audio", false, 2 } };
}

// The same rules as the real backend's (backend/DeviceRules): "auto" is a Wave, a device twice is one, and a list in which
// nothing is there falls back to "auto" for the Headphones mix.
std::vector<OutputState> NullBackend::resolveOutputStates (const juce::StringArray& requested, bool fallBackToAuto) const
{
    const auto outputs = getOutputs();
    std::vector<devices::Candidate> candidates;

    for (const auto& device : outputs)
    {
        devices::Candidate candidate;
        candidate.name = device.id.toStdString();
        candidate.real = true;
        candidate.priority = device.isWave ? 2000 : 1000;
        candidate.wave = device.isWave;
        candidates.push_back (std::move (candidate));
    }

    std::vector<std::string> ids;

    for (const auto& id : requested)
        ids.push_back (id.toStdString());

    std::vector<OutputState> states;

    for (const auto& choice : devices::resolveOutputs (candidates, ids, fallBackToAuto))
    {
        OutputState state;
        state.automatic = choice.automatic;
        state.present = choice.index >= 0;
        state.playing = state.present && running;
        state.id = state.present ? outputs[(size_t) choice.index].id : juce::String (choice.requested);
        state.label = state.present ? outputs[(size_t) choice.index].label : juce::String (choice.requested);
        states.push_back (std::move (state));
    }

    return states;
}

std::vector<AppStreamInfo> NullBackend::getAppStreams() const
{
    std::vector<AppStreamInfo> result;

    for (const auto& s : streams)
        result.push_back (s.info);

    return result;
}

juce::String NullBackend::resolveDeviceLabel (const std::vector<DeviceInfo>& devices, const juce::String& id) const
{
    if (id != "auto")
        for (const auto& d : devices)
            if (d.id == id)
                return d.label;

    for (const auto& d : devices)       // "auto" (or a device that has gone away) prefers the Wave
        if (d.isWave)
            return d.label;

    return devices.empty() ? juce::String() : devices.front().label;
}

BackendStatus NullBackend::getStatus() const
{
    BackendStatus s;
    s.connected = running;
    s.mic = ! running ? MicState::Starting : (micMuted ? MicState::MutedOnDevice : MicState::Ok);
    s.outputOk = running;
    s.inputLabel = resolveDeviceLabel (getInputs(), inputId);
    s.headphoneOutputs = resolveOutputStates (headphoneIds, true);
    s.streamOutputs = resolveOutputStates (streamIds, false);

    for (const auto& output : s.headphoneOutputs)
        if (output.present)
        {
            s.outputLabel = output.label;
            break;
        }

    for (const auto& endpoint : inputEndpoints)
    {
        InputState input;
        input.channelId = endpoint.channelId;
        input.deviceId = endpoint.deviceId;

        for (const auto& device : getInputs())
            if (device.id == endpoint.deviceId)
            {
                input.label = device.label;
                input.connected = true;
            }

        s.inputChannels.push_back (std::move (input));
    }
    s.sampleRate = kRate;
    s.quantum = quantum;
    s.hardwareMicGain = demoMicPresent ? hardwareMicGain : -1.0f;
    s.hardwareHeadphoneVolume = hardwareHeadphoneVolume;

    if (micMuted)
        s.message = juce::String (juce::CharPointer_UTF8 ("Mic muted on the Wave:3 \xe2\x80\x94 tap the top to unmute"));

    return s;
}

void NullBackend::reroute()
{
    auto exists = [this] (const juce::String& id)
    {
        for (const auto& e : endpoints)
            if (e.channelId == id)
                return true;

        return false;
    };

    for (auto& s : streams)
    {
        auto target = matchRoutingRule (rules, s.info.binary, s.info.appName);

        if (target.isEmpty() || ! exists (target))
            target = exists (fallbackChannel) ? fallbackChannel : juce::String();

        s.info.channelId = target;
    }

    refreshSlotMasks();
    notifyChanged();
}

void NullBackend::refreshSlotMasks()
{
    std::array<uint32_t, kMaxAppChannels> masks {};

    for (const auto& s : streams)
        for (const auto& e : endpoints)
            if (e.channelId == s.info.channelId && juce::isPositiveAndBelow (e.slot, kMaxAppChannels))
                masks[(size_t) e.slot] |= 1u << (int) s.kind;

    for (size_t i = 0; i < masks.size(); ++i)
        slotKinds[i].store (masks[i]);
}

void NullBackend::notifyChanged()
{
    if (notifier != nullptr && juce::MessageManager::getInstanceWithoutCreating() != nullptr)
        notifier->triggerAsyncUpdate();
}
}
