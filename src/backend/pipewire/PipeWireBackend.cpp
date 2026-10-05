// PipeWireBackend: wires the PipeWire pieces together and implements hr::AudioBackend.
#include "PipeWireBackend.h"

#include "backend/DeviceRules.h"
#include "AppRouter.h"
#include "Defaults.h"
#include "DeviceControl.h"
#include "DeviceSelector.h"
#include "EngineFilter.h"
#include "LinkManager.h"
#include "PwLoop.h"
#include "PwRegistry.h"
#include "VirtualNodes.h"
#include "WaveGuard.h"

#include "app/Paths.h"
#include "backend/wave/WaveGain.h"
#include "backend/wave/WaveHardware.h"
#include "backend/wave/WavePermission.h"
#include "backend/wave/WaveXlrHardware.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <cmath>
#include <fcntl.h>
#include <set>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace hr
{

using namespace pw;

namespace
{
    static_assert (devices::kNoPort == SPA_ID_INVALID, "the pure port rules and PipeWire agree on what no port is");

    using devices::StereoPorts;

    /** The ports as the pure rules see them (backend/DeviceRules.h): id and channel, in ascending id order. */
    std::vector<devices::PortFacts> facts (const std::vector<PortRec>& ports)
    {
        std::vector<devices::PortFacts> result;
        result.reserve (ports.size());

        for (const auto& port : ports)
            result.push_back ({ port.id, port.channel });

        return result;
    }

    /** Left/right from a node's ports: FL/FR by channel, else MONO, else the first two by id. */
    StereoPorts pickStereo (const std::vector<PortRec>& ports)
    {
        return devices::pickStereoPorts (facts (ports));
    }

    void addLink (std::vector<LinkRequest>& list, uint32_t outPort, uint32_t inPort, LinkKind kind)
    {
        if (outPort == SPA_ID_INVALID || inPort == SPA_ID_INVALID)
            return;

        LinkRequest request;
        request.outPort = outPort;
        request.inPort = inPort;
        request.kind = kind;
        list.push_back (request);
    }

    bool allLinksExist (const std::vector<LinkRequest>& list, const PwRegistry& registry)
    {
        if (list.empty())
            return false;

        for (const auto& request : list)
            if (! registry.hasLink (request.outPort, request.inPort))
                return false;

        return true;
    }

    bool sameDevices (const std::vector<DeviceInfo>& a, const std::vector<DeviceInfo>& b)
    {
        if (a.size() != b.size())
            return false;

        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].id != b[i].id || a[i].label != b[i].label || a[i].isWave != b[i].isWave || a[i].channels != b[i].channels)
                return false;

        return true;
    }

    bool sameOutputStates (const std::vector<OutputState>& a, const std::vector<OutputState>& b)
    {
        if (a.size() != b.size())
            return false;

        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].id != b[i].id || a[i].label != b[i].label || a[i].automatic != b[i].automatic
                || a[i].present != b[i].present || a[i].playing != b[i].playing)
                return false;

        return true;
    }

    bool sameInputStates (const std::vector<InputState>& a, const std::vector<InputState>& b)
    {
        if (a.size() != b.size())
            return false;

        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].channelId != b[i].channelId || a[i].deviceId != b[i].deviceId || a[i].label != b[i].label
                || a[i].connected != b[i].connected)
                return false;

        return true;
    }

    /** Two nodes of one device: the same pw_device, else (a node without one) the same card by its udev-style name. */
    bool sameDevice (const NodeRec& a, const NodeRec& b)
    {
        if (a.deviceId != SPA_ID_INVALID && a.deviceId == b.deviceId)
            return true;

        const auto card = hr::devices::cardOfNodeName (a.name);
        return ! card.empty() && card == hr::devices::cardOfNodeName (b.name);
    }

    std::vector<std::string> toStdList (const juce::StringArray& list)
    {
        std::vector<std::string> result;

        for (const auto& item : list)
            result.push_back (toStd (item));

        return result;
    }

    bool sameApps (const std::vector<AppStreamInfo>& a, const std::vector<AppStreamInfo>& b)
    {
        if (a.size() != b.size())
            return false;

        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].nodeId != b[i].nodeId || a[i].appName != b[i].appName || a[i].binary != b[i].binary
                || a[i].mediaName != b[i].mediaName || a[i].iconName != b[i].iconName
                || a[i].channelId != b[i].channelId)
                return false;

        return true;
    }

    bool sameStatus (const BackendStatus& a, const BackendStatus& b)
    {
        return a.connected == b.connected && a.mic == b.mic && a.outputOk == b.outputOk
            && a.inputLabel == b.inputLabel && a.outputLabel == b.outputLabel
            && sameOutputStates (a.headphoneOutputs, b.headphoneOutputs) && sameOutputStates (a.streamOutputs, b.streamOutputs)
            && sameInputStates (a.inputChannels, b.inputChannels)
            && a.sampleRate == b.sampleRate && a.quantum == b.quantum && a.message == b.message
            && std::abs (a.hardwareMicGain - b.hardwareMicGain) < 0.001f
            && std::abs (a.hardwareHeadphoneVolume - b.hardwareHeadphoneVolume) < 0.001f;
    }

    /** After each mic recovery: run $XDG_CONFIG_HOME/headroom/hooks/on-mic-recovered if it is an executable
        file, with HEADROOM_MIC_STATE=ok|silent in its environment. Detached (double fork, not waited for),
        so a slow script can never stall the audio graph work. Only async-signal-safe calls run between
        fork and exec, everything else is prepared beforehand. */
    void runMicRecoveredHook (bool micOk)
    {
        const auto hook = hr::Paths::getConfigDir().getChildFile ("hooks").getChildFile ("on-mic-recovered");
        const std::string path = toStd (hook.getFullPathName());

        if (! hook.existsAsFile() || ::access (path.c_str(), X_OK) != 0)
            return;

        std::vector<std::string> environment;

        for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry)
            if (std::strncmp (*entry, "HEADROOM_MIC_STATE=", 19) != 0)
                environment.emplace_back (*entry);

        environment.emplace_back (std::string ("HEADROOM_MIC_STATE=") + (micOk ? "ok" : "silent"));

        std::vector<char*> envp;

        for (auto& entry : environment)
            envp.push_back (entry.data());

        envp.push_back (nullptr);

        char* argv[] = { const_cast<char*> (path.c_str()), nullptr };
        const int devNull = ::open ("/dev/null", O_RDWR | O_CLOEXEC);
        const pid_t child = ::fork();

        if (child == 0)
        {
            ::setsid();

            if (::fork() == 0)
            {
                if (devNull >= 0)
                {
                    ::dup2 (devNull, 0);
                    ::dup2 (devNull, 1);
                    ::dup2 (devNull, 2);
                }

                ::execve (path.c_str(), argv, envp.data());
                ::_exit (127);
            }

            ::_exit (0);
        }

        if (child > 0)
            ::waitpid (child, nullptr, 0);   // the intermediate child exits at once

        if (devNull >= 0)
            ::close (devNull);

        pwLog ("ran the on-mic-recovered hook (mic %s)", micOk ? "ok" : "silent");
    }
} // namespace

// =================================================================================================

class PipeWireBackend::Impl
{
public:
    explicit Impl (PipeWireBackend& ownerToUse)
        : owner (ownerToUse),
          devices (registry),
          defaults (registry),
          router (registry),
          notifyState (std::make_shared<NotifyState>())
    {
        // The Wave:3's USB side lives on a thread of its own (never the loop thread, never RT): it reads and sets
        // ClipGuard and runs the password box. It reports through the same coalesced notification as everything else.
        WaveHardware::Options waveOptions;
        waveOptions.onChanged = [this] { requestNotify(); };

        // The udev rule it writes also covers the experimental models when the user has opted in.
        waveOptions.installPermission = [this] (const std::function<bool()>& abort)
        {
            return wave::permission::installViaPkexec (abort, experimentalOn.load());
        };

        waveUsb = std::make_unique<WaveHardware> (std::move (waveOptions));

        // Models with an experimental USB protocol (the Wave XLR's mic gain): on its own thread too, and inert until the
        // user opts in (setExperimentalHardware).
        WaveXlrHardware::Options xlrOptions;
        xlrOptions.onChanged = [this] { requestNotify(); };
        xlrUsb = std::make_unique<WaveXlrHardware> (std::move (xlrOptions));
    }

    ~Impl()
    {
        stop();
        waveUsb->stop();            // its thread calls requestNotify(): it has to be gone before the notifier is
        xlrUsb->stop();
        notifyState->alive = false;
    }

    // ---- message thread -----------------------------------------------------------------------

    bool start (AudioEngine& engineToDrive)
    {
        if (loop.isRunning())
            return true;

        filter = std::make_unique<EngineFilter> (engineToDrive);
        guard = std::make_unique<WaveGuard> (*filter, devices, registry);

        loop.onConnected = [this] { handleConnected(); };
        loop.onDisconnecting = [this] { handleDisconnecting(); };
        loop.onSyncDone = [this] { handleInitialSync(); };

        registry.onChanged = [this] { schedulePump(); };
        registry.onDeviceParam = [this] (uint32_t deviceId, uint32_t paramId, uint32_t index, const spa_pod* param)
        {
            devices.handleParam (deviceId, paramId, index, param);
        };
        registry.onDeviceRemoved = [this] (uint32_t deviceId) { devices.handleDeviceRemoved (deviceId); };
        registry.onMetadataBound = [this]
        {
            defaults.metadataRebound();
            schedulePump();
        };

        devices.onChanged = [this] { schedulePump(); };
        vnodes.onEvent = [this] { schedulePump(); };
        links.onEvent = [this] { schedulePump(); };
        filter->onStateChanged = [this] { schedulePump(); };
        guard->onRecoveryFinished = [] (bool micOk) { runMicRecoveredHook (micOk); };

        publishDisconnected();
        waveUsb->start();
        xlrUsb->start();
        return loop.start();
    }

    void stop()
    {
        waveUsb->stop();            // joins the USB thread (and abandons a password box that is still open)
        xlrUsb->stop();

        if (! loop.isRunning())
            return;

        {
            LoopLock lock (loop.threadLoop());
            tickTimer.reset();
            pumpTimer.reset();
            forceRecoveryTimer.reset();
        }

        loop.stop();   // runs handleDisconnecting() under the lock if we were connected

        guard.reset();
        filter.reset();
        publishDisconnected();
    }

    void setChannels (const std::vector<ChannelEndpoint>& channels)
    {
        LoopLock lock (loop.threadLoop());
        vnodes.setChannels (channels);
        updateDefaultTargets();
        schedulePump();
    }

    void setDevices (const juce::String& inputId, const juce::StringArray& headphones, const juce::StringArray& stream)
    {
        LoopLock lock (loop.threadLoop());
        inputSetting = toStd (inputId);
        headphoneSettings = toStdList (headphones);
        streamSettings = toStdList (stream);

        if (headphoneSettings.empty())
            headphoneSettings.push_back ("auto");

        schedulePump();
    }

    void setInputChannels (const std::vector<InputEndpoint>& inputs)
    {
        LoopLock lock (loop.threadLoop());
        inputChannelList = inputs;
        schedulePump();
    }

    void setRoutingRules (const std::vector<RoutingRule>& rules, const juce::String& fallbackChannelId)
    {
        LoopLock lock (loop.threadLoop());
        fallbackChannel = fallbackChannelId;
        router.setRules (rules, fallbackChannelId);
        updateDefaultTargets();
        schedulePump();
    }

    void setMakeDefault (bool makeDefault)
    {
        LoopLock lock (loop.threadLoop());
        defaults.setEnabled (makeDefault);
        schedulePump();
    }

    void setKeepDefault (bool keepDefault)
    {
        LoopLock lock (loop.threadLoop());
        defaults.setKeep (keepDefault);
        schedulePump();
    }

    void setLatencyQuantum (int frames)
    {
        LoopLock lock (loop.threadLoop());
        quantum = juce::jlimit (32, 2048, frames);

        if (filter != nullptr && filter->exists())
            filter->setQuantum (quantum);
    }

    void routeApp (uint32_t nodeId, const juce::String& channelId)
    {
        LoopLock lock (loop.threadLoop());

        if (! initialSyncDone || otherInstance)
            return;

        router.routeNow (nodeId, channelId);
        schedulePump();
    }

    void runMicRecovery()
    {
        LoopLock lock (loop.threadLoop());

        if (guard != nullptr)
            guard->requestManualRecovery();

        schedulePump (1);
    }

    void setHardwareMicGain (float linear0to1)
    {
        LoopLock lock (loop.threadLoop());
        pendingMicGain = linear0to1;
        schedulePump (1);
    }

    void setHardwareHeadphoneVolume (float linear0to1)
    {
        LoopLock lock (loop.threadLoop());
        pendingHeadphoneVolume = linear0to1;
        schedulePump (1);
    }

    void setClipGuard (bool on)                 { waveUsb->setClipGuard (on); }

    void setExperimentalHardware (bool on)
    {
        experimentalOn = on;
        xlrUsb->setEnabled (on);

        LoopLock lock (loop.threadLoop());
        schedulePump (1);
    }

    /** The password box for the USB rule: for the experimental model when that is what the person is looking at, else the Wave:3's. */
    void requestHardwarePermission()
    {
        const elgato::Model* model = nullptr;

        {
            const juce::ScopedLock lock (snapshotLock);
            model = snapshot.inputModel;
        }

        if (experimentalOn.load() && model != nullptr && model->hardware == elgato::Hardware::Experimental)
            xlrUsb->requestPermission();
        else
            waveUsb->requestPermission();
    }

    /** The USB side, narrowed to what the person is using: only when the selected input is the Wave:3. */
    HardwareControls getHardwareControls() const
    {
        auto controls = waveUsb->get();
        const elgato::Model* model = nullptr;

        {
            const juce::ScopedLock lock (snapshotLock);

            if (! snapshot.inputIsWave3)
                controls = {};

            model = snapshot.inputModel;
        }

        // A model whose USB protocol is documented but untried: the opt-in row, and its state once the user has opted in.
        if (model != nullptr && model->hardware == elgato::Hardware::Experimental)
        {
            controls.experimental = xlrUsb->get();
            controls.experimental.model = juce::String (model->shortName);
            controls.experimental.on = experimentalOn.load();
        }

        return controls;
    }

    std::vector<DeviceInfo> getInputs() const
    {
        const juce::ScopedLock lock (snapshotLock);
        return snapshot.inputs;
    }

    std::vector<DeviceInfo> getOutputs() const
    {
        const juce::ScopedLock lock (snapshotLock);
        return snapshot.outputs;
    }

    std::vector<AppStreamInfo> getAppStreams() const
    {
        const juce::ScopedLock lock (snapshotLock);
        return snapshot.apps;
    }

    BackendStatus getStatus() const
    {
        const juce::ScopedLock lock (snapshotLock);
        return snapshot.status;
    }

private:
    // ---- what a pass decided --------------------------------------------------------------------

    struct InputSelection
    {
        InputEndpoint endpoint;
        const NodeRec* node = nullptr;      // the capture device, nullptr when it is not there
    };

    struct Selection
    {
        const NodeRec* input = nullptr;
        const NodeRec* output = nullptr;                    // the first device of the Headphones mix that is there: what the status line and WaveGuard talk about
        std::vector<ResolvedOutput> headphones, stream;     // everything the two mixes play to (Stream: the extras)
        std::vector<InputSelection> inputs;                 // the input channels
    };

    /** The links to one device a mix plays to. */
    struct OutputPlan
    {
        const NodeRec* node = nullptr;
        std::string requested;
        bool automatic = false;
        std::vector<LinkRequest> links;
    };

    /** The links from one input channel's device to its slot in the engine. */
    struct InputPlan
    {
        InputEndpoint endpoint;
        const NodeRec* node = nullptr;
        std::vector<LinkRequest> links;
    };

    struct GraphPlan
    {
        bool engineReady = false;
        std::vector<LinkRequest> internal, capture;     // capture: the mic
        std::vector<OutputPlan> headphones, stream;     // one per device each mix plays to
        std::vector<InputPlan> inputs;                  // one per input channel
    };

    struct Snapshot
    {
        std::vector<DeviceInfo> inputs, outputs;
        std::vector<AppStreamInfo> apps;
        BackendStatus status;
        bool inputIsWave3 = false;      // the selected input is the original Wave:3 (its USB controls apply)
        const elgato::Model* inputModel = nullptr;      // the selected input's model, when it is one we know (static table)
    };

    struct NotifyState
    {
        std::atomic<bool> alive { true };
        std::atomic<bool> pending { false };
        std::atomic<int64_t> lastMs { 0 };
    };

    // ---- loop thread: connection lifecycle ---------------------------------------------------------

    void handleConnected()
    {
        registry.attach (loop.core());
        vnodes.attach (loop.core());
        links.attach (loop.core());
        devices.reset();
        defaults.reset();
        router.reset();

        if (guard != nullptr)
            guard->reset();

        filterRetryAtMs = 0;
        initialSyncDone = false;
        otherInstance = false;
        syncDeadlineMs = nowMs() + 3000;

        if (tickTimer == nullptr)
            tickTimer = std::make_unique<LoopTimer> (loop.loop(), [this] { pump(); });

        if (pumpTimer == nullptr)
            pumpTimer = std::make_unique<LoopTimer> (loop.loop(), [this]
            {
                pumpArmed = false;
                pump();
            });

        tickTimer->startRepeating (100);
        armTestRecoveryHook();

        // Nothing of ours is created until the registry has told us what already exists (handleInitialSync).
        loop.requestSync();
    }

    /** The registry has delivered everything that existed when we connected. Before any node of ours exists, make sure
        another Headroom does not already own headroom.engine: two of them would fight over the same nodes and links
        (SingleInstance keeps that from happening; this is the safety net). If one does, create nothing and say so. */
    void handleInitialSync()
    {
        if (initialSyncDone)
            return;

        initialSyncDone = true;

        if (const auto* other = findOtherEngine())
        {
            otherInstance = true;
            pwLog ("Headroom is already running (%s is node %u of client %u); creating nothing", kEngineName, other->id, other->clientId);
            publishDisconnected ("Headroom is already running.");
            return;
        }

        if (filter != nullptr && ! filter->create (loop.core(), quantum))
            filterRetryAtMs = nowMs() + 3000;

        schedulePump (1);
    }

    /** A headroom.engine node that is not ours: it was created by another client. */
    const NodeRec* findOtherEngine() const
    {
        const uint32_t ownNode = filter != nullptr ? filter->nodeId() : SPA_ID_INVALID;

        for (const auto& entry : registry.nodes())
            if (entry.second.name == kEngineName && entry.first != ownNode)
                return &entry.second;

        return nullptr;
    }

    /** Test hook: HEADROOM_TEST_FORCE_RECOVERY_AFTER_MS=<ms> forces one manual mic recovery that long after the
        first connection (the integration test uses it to check that apps capturing "Headroom Mic" are unharmed). */
    void armTestRecoveryHook()
    {
        if (forceRecoveryArmed)
            return;

        forceRecoveryArmed = true;
        const int delayMs = juce::SystemStats::getEnvironmentVariable ("HEADROOM_TEST_FORCE_RECOVERY_AFTER_MS", {}).getIntValue();

        if (delayMs <= 0)
            return;

        forceRecoveryTimer = std::make_unique<LoopTimer> (loop.loop(), [this]
        {
            pwLog ("test hook: forcing a mic recovery");

            if (guard != nullptr)
                guard->requestManualRecovery();

            schedulePump (1);
        });
        forceRecoveryTimer->startOnce (delayMs);
    }

    void handleDisconnecting()
    {
        // Dependency order: things that hold proxies, then the filter (stops the RT thread's
        // use of the engine), then the mirror.
        router.reset();
        defaults.reset();
        links.detach();
        vnodes.detach();

        if (filter != nullptr)
            filter->destroy();

        devices.reset();
        registry.detach();

        if (guard != nullptr)
            guard->reset();

        if (tickTimer != nullptr)
            tickTimer->stop();

        pumpArmed = false;
        initialSyncDone = false;
        otherInstance = false;
        inputGates.clear();
        publishDisconnected();
    }

    void schedulePump (int delayMs = 15)
    {
        if (pumpTimer == nullptr || pumpArmed)
            return;

        pumpArmed = true;
        pumpTimer->startOnce (delayMs);
    }

    void updateDefaultTargets()
    {
        const auto& channels = vnodes.channels();
        juce::String channel = fallbackChannel;
        bool found = false;

        for (const auto& candidate : channels)
            found = found || candidate.channelId == channel;

        if (! found && ! channels.empty())
        {
            channel = channels.front().channelId;
            found = true;
        }

        defaults.setTargets (found ? VirtualNodes::channelNodeName (channel) : std::string(), kMicSourceName);
    }

    // ---- loop thread: the update pass ---------------------------------------------------------------

    void recreateFilterIfNeeded (int64_t now)
    {
        if (filter == nullptr || loop.core() == nullptr)
            return;

        if (filter->exists() && filter->hasFailed())
        {
            pwLog ("the engine filter failed, recreating it");
            filter->destroy();
            filterRetryAtMs = now + 1000;
        }

        if (! filter->exists() && now >= filterRetryAtMs && ! filter->create (loop.core(), quantum))
            filterRetryAtMs = now + 3000;
    }

    GraphPlan buildPlan (const Selection& selection) const
    {
        GraphPlan plan;

        const NodeRec* engineNode = registry.nodeByName (kEngineName);

        if (engineNode == nullptr)
            return plan;

        const auto enginePort = [&] (bool input, const std::string& name)
        {
            const auto* port = registry.findPortByName (engineNode->id, input, name);
            return port != nullptr ? port->id : SPA_ID_INVALID;
        };

        plan.engineReady = enginePort (true, "mic_L") != SPA_ID_INVALID && enginePort (false, "monitor_L") != SPA_ID_INVALID;

        if (! plan.engineReady)
            return plan;

        // Channel sinks -> engine app inputs.
        for (const auto& channel : vnodes.channels())
        {
            if (channel.slot < 0 || channel.slot >= hr::kMaxAppChannels)
                continue;

            const auto* sink = registry.nodeByName (VirtualNodes::channelNodeName (channel.channelId));

            if (sink == nullptr)
                continue;

            const auto monitor = pickStereo (registry.portsOf (sink->id, false, PwRegistry::PortKind::MonitorOnly));
            addLink (plan.internal, monitor.left, enginePort (true, EngineFilter::appPortName (channel.slot, false)), LinkKind::Internal);
            addLink (plan.internal, monitor.right, enginePort (true, EngineFilter::appPortName (channel.slot, true)), LinkKind::Internal);
        }

        // Engine outputs -> Headroom Mic and Headroom Stream Mix.
        const auto feedSource = [&] (const char* sourceName, const char* leftPort, const char* rightPort)
        {
            const auto* source = registry.nodeByName (sourceName);

            if (source == nullptr)
                return;

            const auto inputs = pickStereo (registry.portsOf (source->id, true, PwRegistry::PortKind::Any));
            addLink (plan.internal, enginePort (false, leftPort), inputs.left, LinkKind::Internal);
            addLink (plan.internal, enginePort (false, rightPort), inputs.right, LinkKind::Internal);
        };

        feedSource (kMicSourceName, "micout_L", "micout_R");
        feedSource (kStreamSourceName, "stream_L", "stream_R");

        // Mic device -> engine. An Elgato Wave gives its mic as channel 1 (an XLR interface's stream carries more than the mic),
        // any other device as it is: one or two ports, or just the first channel of an interface with many.
        if (selection.input != nullptr)
        {
            const auto who = registry.identityOf (*selection.input);
            const auto source = hr::devices::pickMicPorts (facts (registry.portsOf (selection.input->id, false, PwRegistry::PortKind::Any)),
                                                       who.elgato && who.wave);
            addLink (plan.capture, source.left, enginePort (true, "mic_L"), LinkKind::Capture);
            addLink (plan.capture, source.right, enginePort (true, "mic_R"), LinkKind::Capture);
        }

        // Input channels: each capture device to its slot's pair of engine inputs. A mono device (or a channel taken alone) feeds both
        // sides, so the engine always sees a pair; an interface with many channels gives its first.
        for (const auto& input : selection.inputs)
        {
            InputPlan inputPlan;
            inputPlan.endpoint = input.endpoint;
            inputPlan.node = input.node;

            if (input.node != nullptr)
            {
                const auto source = hr::devices::pickInputPorts (facts (registry.portsOf (input.node->id, false, PwRegistry::PortKind::Any)),
                                                                 input.endpoint.stereo);

                for (const auto& link : hr::devices::planInputSlot (source,
                                                                    enginePort (true, EngineFilter::inputSlotPortName (input.endpoint.slot, false)),
                                                                    enginePort (true, EngineFilter::inputSlotPortName (input.endpoint.slot, true))))
                    addLink (inputPlan.links, link.out, link.in, LinkKind::Capture);
            }

            plan.inputs.push_back (std::move (inputPlan));
        }

        // Engine -> every device a mix plays to: the first pair of playback channels, or the pair a model's headphones listen to.
        // One output port feeds any number of input ports; PipeWire rate-matches the devices that are not the driver.
        const auto playTo = [&] (const std::vector<ResolvedOutput>& outputs, const char* leftPort, const char* rightPort,
                                 std::vector<OutputPlan>& into)
        {
            for (const auto& output : outputs)
            {
                OutputPlan outputPlan;
                outputPlan.node = output.node;
                outputPlan.requested = output.requested;
                outputPlan.automatic = output.automatic;

                if (output.node != nullptr)
                {
                    const auto who = registry.identityOf (*output.node);
                    const int pair = who.model != nullptr ? who.model->monitorPair : 0;
                    const auto sink = hr::devices::pickPlaybackPorts (facts (registry.portsOf (output.node->id, true, PwRegistry::PortKind::Any)), pair);

                    for (const auto& link : hr::devices::planFanOut (enginePort (false, leftPort), enginePort (false, rightPort), { sink }))
                        addLink (outputPlan.links, link.out, link.in, LinkKind::Playback);
                }

                into.push_back (std::move (outputPlan));
            }
        };

        playTo (selection.headphones, "monitor_L", "monitor_R", plan.headphones);
        playTo (selection.stream, "stream_L", "stream_R", plan.stream);

        return plan;
    }

    /** Capture-first for the input channels that are an Elgato Wave which also plays a mix: one gate per input channel,
        open when there is nothing to wait for. The main mic has WaveGuard for this; an input channel gets only the ordering. */
    void updateInputGates (const GraphPlan& plan, int64_t now)
    {
        std::set<std::string> present;

        for (const auto& input : plan.inputs)
        {
            const auto key = toStd (input.endpoint.channelId);
            present.insert (key);

            bool needed = false;

            if (input.node != nullptr && hr::devices::inputNeedsCaptureFirst (registry.identityOf (*input.node)))
                for (const auto* outputs : { &plan.headphones, &plan.stream })
                    for (const auto& output : *outputs)
                        needed = needed || (output.node != nullptr && sameDevice (*output.node, *input.node));

            inputGates[key].update (needed, input.node != nullptr && ! input.links.empty(), allLinksExist (input.links, registry),
                                    filter != nullptr ? filter->inputLiveCycles (input.endpoint.slot) : 0u, now);
        }

        for (auto it = inputGates.begin(); it != inputGates.end();)
            it = present.count (it->first) > 0 ? std::next (it) : inputGates.erase (it);
    }

    /** A device waits while an input channel on the same Elgato device is still being brought up. */
    bool outputMayPlay (const GraphPlan& plan, const OutputPlan& output) const
    {
        if (output.node == nullptr)
            return true;

        for (const auto& input : plan.inputs)
        {
            if (input.node == nullptr)
                continue;

            const auto gate = inputGates.find (toStd (input.endpoint.channelId));

            if (gate != inputGates.end() && ! gate->second.isOpen() && sameDevice (*output.node, *input.node))
                return false;
        }

        return true;
    }

    /** The Wave's current hardware level for the node's route, as the same 0..1 slider position the setters take
        (cube root of PipeWire's linear channel volume, averaged over the channels). -1 = not a Wave with a route. */
    float readHardwareValue (const NodeRec* node, int direction)
    {
        if (node == nullptr || node->deviceId == SPA_ID_INVALID || ! registry.isWaveNode (*node))
            return -1.0f;

        devices.watch (node->deviceId);
        const auto* state = devices.state (node->deviceId);

        if (state == nullptr)
            return -1.0f;

        const auto* route = DeviceControl::findRoute (*state, direction, node->cardProfileDevice);

        if (route == nullptr || route->volumes.empty())
            return -1.0f;

        float sum = 0.0f;

        for (const float volume : route->volumes)
            sum += DeviceControl::volumeToSlider (volume);

        return sum / (float) route->volumes.size();
    }

    /** The model has no ALSA gain control and an experimental USB protocol for it, and the user has opted in. */
    bool usesUsbGain (const elgato::Model* model) const
    {
        return model != nullptr && model->hardware == elgato::Hardware::Experimental && model->usbGainMaxDb > 0.0
            && experimentalOn.load();
    }

    void applyHardwareControls (const Selection& selection)
    {
        // The mic gain of a model the kernel gives no gain control for goes over USB (experimental, opt-in). The ALSA route
        // comes first wherever it has a volume: each control has one path only.
        if (pendingMicGain.has_value() && selection.input != nullptr)
        {
            const auto who = registry.identityOf (*selection.input);

            if (usesUsbGain (who.model) && readHardwareValue (selection.input, SPA_DIRECTION_INPUT) < 0.0f)
            {
                const double db = wave::sliderToHardwareDb ((double) *pendingMicGain, who.model->usbGainMaxDb);
                pwLog ("asking the %s for %.1f dB of mic gain over USB (experimental)", who.model->name, db);
                xlrUsb->setGainDb (db);
                pendingMicGain.reset();
            }
        }

        const auto apply = [this] (const NodeRec* node, int direction, std::optional<float>& pending, const char* what)
        {
            if (! pending.has_value() || node == nullptr || node->deviceId == SPA_ID_INVALID)
                return;

            devices.watch (node->deviceId);
            const auto* state = devices.state (node->deviceId);

            if (state == nullptr)
                return;

            const auto* route = DeviceControl::findRoute (*state, direction, node->cardProfileDevice);

            if (route == nullptr)
                return;

            if (devices.setRoute (node->deviceId, *route, false, DeviceControl::sliderToVolume (*pending)))
            {
                pwLog ("set hardware %s to %.2f", what, (double) *pending);
                pending.reset();
            }
        };

        const bool micGainWaiting = pendingMicGain.has_value();

        apply (selection.input, SPA_DIRECTION_INPUT, pendingMicGain, "mic gain");
        apply (selection.output, SPA_DIRECTION_OUTPUT, pendingHeadphoneVolume, "headphone volume");

        // The kernel's copy of the mic's gain is not proof that the mic took it (gain lock, a turned dial): once the
        // change has been made, have the USB side read what the mic itself says (Safe gain compares the two).
        if (micGainWaiting && ! pendingMicGain.has_value())
            waveUsb->readSoon (600);
    }

    void pump()
    {
        if (! loop.isConnected() || filter == nullptr || guard == nullptr)
            return;

        const int64_t now = nowMs();

        if (! initialSyncDone)
        {
            if (now < syncDeadlineMs)
                return;

            pwLog ("the registry sync did not come back in time, going on without it");
            handleInitialSync();
        }

        if (otherInstance)
        {
            if (findOtherEngine() != nullptr)
                return;         // still there: leave the graph alone

            pwLog ("the other Headroom is gone, taking over");
            otherInstance = false;
        }

        vnodes.sync (now);
        recreateFilterIfNeeded (now);

        Selection selection;
        selection.input = pickDevice (registry, true, inputSetting);
        selection.headphones = pickOutputs (registry, headphoneSettings, true);
        selection.stream = pickOutputs (registry, streamSettings, false);

        for (const auto& output : selection.headphones)
            if (output.node != nullptr)
            {
                selection.output = output.node;
                break;
            }

        for (const auto& endpoint : inputChannelList)
            selection.inputs.push_back ({ endpoint, findInputNode (registry, toStd (endpoint.deviceId)) });

        const GraphPlan plan = buildPlan (selection);

        // WaveGuard decides when the capture and playback links may exist.
        GuardInputs guardInputs;
        guardInputs.engineReady = plan.engineReady;
        guardInputs.inputFound = selection.input != nullptr;

        elgato::Identity inputWho, outputWho;

        if (selection.input != nullptr)
        {
            inputWho = registry.identityOf (*selection.input);
            guardInputs.inputName = selection.input->name;
            guardInputs.inputNodeId = selection.input->id;
            guardInputs.inputDeviceId = selection.input->deviceId;
            guardInputs.inputCardDevice = selection.input->cardProfileDevice;
            guardInputs.inputIdentity = inputWho;
            guardInputs.inputLabel = toStd (deviceLabel (registry, *selection.input, true));
        }

        if (selection.output != nullptr)
            outputWho = registry.identityOf (*selection.output);

        guardInputs.captureLinkable = ! plan.capture.empty();
        guardInputs.captureLinked = allLinksExist (plan.capture, registry);

        // The Elgato Wave whose profile may be fixed: the selected input's device, else the selected output's, else any Wave on
        // the machine (an "auto" setting with its mic or headphones not selected yet). Never a device that is not a Wave.
        uint32_t waveDevice = SPA_ID_INVALID;

        if (inputWho.wave && selection.input != nullptr && selection.input->deviceId != SPA_ID_INVALID)
            waveDevice = selection.input->deviceId;
        else if (outputWho.wave && selection.output != nullptr && selection.output->deviceId != SPA_ID_INVALID)
            waveDevice = selection.output->deviceId;
        else if (const auto* wave = registry.findWaveDevice())
            waveDevice = wave->id;

        if (waveDevice != SPA_ID_INVALID)
        {
            guardInputs.waveDeviceId = waveDevice;
            const bool outputIsAuto = std::find (headphoneSettings.begin(), headphoneSettings.end(), "auto") != headphoneSettings.end();
            guardInputs.manageWaveProfile = inputSetting == "auto" || outputIsAuto || inputWho.wave || outputWho.wave;
            devices.watch (waveDevice);
        }

        guard->update (guardInputs, now);
        const auto& verdict = guard->result();

        updateInputGates (plan, now);

        std::vector<LinkRequest> desired = plan.internal;

        if (verdict.allowCapture)
            desired.insert (desired.end(), plan.capture.begin(), plan.capture.end());

        // The input channels' capture is not part of the mic's WaveGuard: it is linked at once, and (for an Elgato device that
        // also plays a mix) only holds back the playback on that device.
        for (const auto& input : plan.inputs)
            desired.insert (desired.end(), input.links.begin(), input.links.end());

        // Every device of both mixes waits for the main mic's capture-first ordering, as the single device always did; one that
        // is the Elgato device of an input channel waits for that capture as well.
        if (verdict.allowPlayback)
            for (const auto* outputs : { &plan.headphones, &plan.stream })
                for (const auto& output : *outputs)
                    if (outputMayPlay (plan, output))
                        desired.insert (desired.end(), output.links.begin(), output.links.end());

        links.reconcile (desired, registry, now);

        defaults.evaluate (now);
        router.evaluate();
        applyHardwareControls (selection);

        publish (selection, plan, verdict);
    }

    // ---- snapshots ------------------------------------------------------------------------------------

    /** The name of a device that is not there: what it was called when it was last seen, else the best guess from its id. */
    juce::String rememberedLabel (const std::string& nodeName) const
    {
        const auto found = labelMemory.find (nodeName);
        return found != labelMemory.end() ? found->second : toJuce (hr::devices::friendlyNameFromNodeName (nodeName));
    }

    void publishDisconnected (const juce::String& message = "Waiting for the audio system to start.")
    {
        Snapshot fresh;
        fresh.status.connected = false;
        fresh.status.mic = MicState::NotFound;
        fresh.status.message = message;
        commit (std::move (fresh));
    }

    void publish (const Selection& selection, const GraphPlan& plan, const GuardResult& verdict)
    {
        Snapshot fresh;
        fresh.inputs = listDevices (registry, true);
        fresh.outputs = listDevices (registry, false);

        // What a device was called while it was there is what it is called when it is unplugged (a name in a list of ticks).
        for (const auto& device : fresh.inputs)
            labelMemory[toStd (device.id)] = device.label;

        for (const auto& device : fresh.outputs)
            labelMemory[toStd (device.id)] = device.label;

        for (const auto& entry : registry.nodes())
        {
            const auto& stream = entry.second;

            if (! stream.isStreamOutput() || stream.isOurs())
                continue;

            AppStreamInfo info;
            info.nodeId = stream.id;
            info.appName = toJuce (stream.appName.empty() ? stream.binary : stream.appName);
            info.binary = toJuce (stream.binary);
            info.mediaName = toJuce (stream.mediaName);
            info.iconName = toJuce (stream.iconName.empty() ? stream.binary : stream.iconName);
            info.channelId = router.channelOf (stream.id);
            fresh.apps.push_back (std::move (info));
        }

        auto& status = fresh.status;
        status.connected = true;
        status.mic = verdict.mic;
        status.message = verdict.message;

        // A mic problem is the more urgent thing to tell the user; the default-device lock only speaks up otherwise.
        if (status.message.isEmpty() && defaults.gaveUp())
            status.message = Defaults::gaveUpMessage();

        for (const auto& output : plan.headphones)
            if (output.node != nullptr)
            {
                status.outputOk = allLinksExist (output.links, registry);       // the first device that is there: what a single device always was
                break;
            }

        const auto describeOutputs = [this] (const std::vector<OutputPlan>& outputs)
        {
            std::vector<OutputState> states;

            for (const auto& output : outputs)
            {
                OutputState state;
                state.automatic = output.automatic;
                state.present = output.node != nullptr;
                state.id = state.present ? toJuce (output.node->name) : toJuce (output.requested);
                state.label = state.present ? deviceLabel (registry, *output.node, false) : rememberedLabel (output.requested);
                state.playing = state.present && allLinksExist (output.links, registry);
                states.push_back (std::move (state));
            }

            return states;
        };

        status.headphoneOutputs = describeOutputs (plan.headphones);
        status.streamOutputs = describeOutputs (plan.stream);

        for (const auto& input : plan.inputs)
        {
            InputState state;
            state.channelId = input.endpoint.channelId;
            state.deviceId = input.endpoint.deviceId;
            state.connected = input.node != nullptr;
            state.label = state.connected ? deviceLabel (registry, *input.node, true) : rememberedLabel (toStd (input.endpoint.deviceId));
            status.inputChannels.push_back (std::move (state));
        }

        const auto inputWho = selection.input != nullptr ? registry.identityOf (*selection.input) : elgato::Identity();
        fresh.inputIsWave3 = inputWho.isWave3();
        fresh.inputModel = inputWho.model;
        status.hardwareMicGain = readHardwareValue (selection.input, SPA_DIRECTION_INPUT);

        // No ALSA gain to read: what the mic itself says over USB, if the user has opted in and it has been read.
        if (status.hardwareMicGain < 0.0f && usesUsbGain (inputWho.model))
            if (const auto gain = xlrUsb->get().gainDb)
                status.hardwareMicGain = (float) wave::hardwareDbToSlider ((double) *gain, inputWho.model->usbGainMaxDb);
        status.hardwareHeadphoneVolume = readHardwareValue (selection.output, SPA_DIRECTION_OUTPUT);

        if (selection.input != nullptr)
            status.inputLabel = deviceLabel (registry, *selection.input, true);

        if (selection.output != nullptr)
            status.outputLabel = deviceLabel (registry, *selection.output, false);

        status.sampleRate = filter != nullptr ? (double) filter->lastSampleRate() : 0.0;
        status.quantum = filter != nullptr ? (int) filter->lastQuantum() : 0;

        if (status.quantum == 0)
            status.quantum = quantum;

        commit (std::move (fresh));
    }

    void commit (Snapshot&& fresh)
    {
        bool changed = false;

        {
            const juce::ScopedLock lock (snapshotLock);

            changed = ! sameDevices (snapshot.inputs, fresh.inputs) || ! sameDevices (snapshot.outputs, fresh.outputs)
                   || ! sameApps (snapshot.apps, fresh.apps) || ! sameStatus (snapshot.status, fresh.status)
                   || snapshot.inputIsWave3 != fresh.inputIsWave3 || snapshot.inputModel != fresh.inputModel;

            if (changed)
                snapshot = std::move (fresh);
        }

        if (changed)
            requestNotify();
    }

    /** Posts onChanged to the message thread, coalesced to at most about 20 calls a second. */
    void requestNotify()
    {
        if (notifyState->pending.exchange (true))
            return;

        if (juce::MessageManager::getInstanceWithoutCreating() == nullptr)
        {
            notifyState->pending = false;
            return;
        }

        const auto state = notifyState;
        PipeWireBackend* target = &owner;

        juce::MessageManager::callAsync ([state, target]
        {
            if (! state->alive.load())
                return;

            const auto fire = [state, target]
            {
                if (! state->alive.load())
                    return;

                state->pending = false;
                state->lastMs = nowMs();

                if (target->onChanged)
                    target->onChanged();
            };

            const int wait = juce::jmax (0, 50 - (int) (nowMs() - state->lastMs.load()));

            if (wait == 0)
                fire();
            else
                juce::Timer::callAfterDelay (wait, fire);
        });
    }

    // ---- members ----------------------------------------------------------------------------------------

    PipeWireBackend& owner;

    PwLoop loop;
    PwRegistry registry;
    std::unique_ptr<EngineFilter> filter;
    VirtualNodes vnodes;
    LinkManager links;
    DeviceControl devices;
    std::unique_ptr<WaveGuard> guard;
    Defaults defaults;
    AppRouter router;

    std::unique_ptr<LoopTimer> tickTimer, pumpTimer, forceRecoveryTimer;
    bool pumpArmed = false;
    bool forceRecoveryArmed = false;
    int64_t filterRetryAtMs = 0;

    // The start-up check for another Headroom (handleInitialSync). Loop thread / under the loop lock.
    bool initialSyncDone = false;
    bool otherInstance = false;
    int64_t syncDeadlineMs = 0;

    // Desired state: read and written under the loop lock.
    std::string inputSetting = "auto";
    std::vector<std::string> headphoneSettings { "auto" }, streamSettings;      // what each mix plays to: "auto" or node.names
    std::vector<InputEndpoint> inputChannelList;                                // the input channels of the model
    std::map<std::string, hr::devices::CaptureGate> inputGates;                 // by channel id; loop thread
    std::map<std::string, juce::String> labelMemory;                            // node.name -> the label it had when it was last seen; loop thread
    juce::String fallbackChannel;
    int quantum = 256;
    std::optional<float> pendingMicGain, pendingHeadphoneVolume;

    std::atomic<bool> experimentalOn { false };     // the user's opt-in for the models with an experimental USB protocol
    std::unique_ptr<WaveHardware> waveUsb;          // the Wave:3's USB side (own thread)
    std::unique_ptr<WaveXlrHardware> xlrUsb;        // the experimental models' USB side (own thread, inert until opted in)

    // Snapshots for the message thread.
    mutable juce::CriticalSection snapshotLock;
    Snapshot snapshot;
    std::shared_ptr<NotifyState> notifyState;
};

// =================================================================================================

PipeWireBackend::PipeWireBackend() : impl (std::make_unique<Impl> (*this)) {}
PipeWireBackend::~PipeWireBackend() = default;

bool PipeWireBackend::start (AudioEngine& engine) { return impl->start (engine); }
void PipeWireBackend::stop() { impl->stop(); }

void PipeWireBackend::setChannels (const std::vector<ChannelEndpoint>& channels) { impl->setChannels (channels); }
void PipeWireBackend::setDevices (const juce::String& inputId, const juce::StringArray& headphones, const juce::StringArray& stream)
{
    impl->setDevices (inputId, headphones, stream);
}

void PipeWireBackend::setInputChannels (const std::vector<InputEndpoint>& inputs) { impl->setInputChannels (inputs); }

void PipeWireBackend::setRoutingRules (const std::vector<RoutingRule>& rules, const juce::String& fallbackChannelId)
{
    impl->setRoutingRules (rules, fallbackChannelId);
}

void PipeWireBackend::setMakeDefault (bool makeDefault) { impl->setMakeDefault (makeDefault); }
void PipeWireBackend::setKeepDefault (bool keepDefault) { impl->setKeepDefault (keepDefault); }
void PipeWireBackend::setLatencyQuantum (int frames) { impl->setLatencyQuantum (frames); }
void PipeWireBackend::routeApp (uint32_t nodeId, const juce::String& channelId) { impl->routeApp (nodeId, channelId); }
void PipeWireBackend::runMicRecovery() { impl->runMicRecovery(); }
void PipeWireBackend::setHardwareMicGain (float linear0to1) { impl->setHardwareMicGain (linear0to1); }
void PipeWireBackend::setHardwareHeadphoneVolume (float linear0to1) { impl->setHardwareHeadphoneVolume (linear0to1); }
void PipeWireBackend::setClipGuard (bool on) { impl->setClipGuard (on); }
void PipeWireBackend::requestHardwarePermission() { impl->requestHardwarePermission(); }
void PipeWireBackend::setExperimentalHardware (bool on) { impl->setExperimentalHardware (on); }
HardwareControls PipeWireBackend::getHardwareControls() const { return impl->getHardwareControls(); }

std::vector<DeviceInfo> PipeWireBackend::getInputs() const { return impl->getInputs(); }
std::vector<DeviceInfo> PipeWireBackend::getOutputs() const { return impl->getOutputs(); }
std::vector<AppStreamInfo> PipeWireBackend::getAppStreams() const { return impl->getAppStreams(); }
BackendStatus PipeWireBackend::getStatus() const { return impl->getStatus(); }

} // namespace hr
