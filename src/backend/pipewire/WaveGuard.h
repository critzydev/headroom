// WaveGuard: capture-first ordering and self-healing for the Elgato Wave family's shared capture/playback clock.
#pragma once

#include "PwCommon.h"
#include "PwRegistry.h"
#include "DeviceControl.h"
#include "EngineFilter.h"

#include "backend/AudioBackend.h"
#include "backend/DeviceRules.h"
#include "backend/ElgatoModels.h"

#include <deque>

namespace hr::pw
{

/** What the backend tells WaveGuard about the current selection. Updated every pass. */
struct GuardInputs
{
    bool engineReady = false;            // the engine node and its ports exist
    bool inputFound = false;             // a capture device is selected and present
    std::string inputName;               // its node.name
    uint32_t inputNodeId = SPA_ID_INVALID;
    uint32_t inputDeviceId = SPA_ID_INVALID;  // the pw_device that owns the input node, if any
    int inputCardDevice = -1;            // card.profile.device of the input node (route lookup)
    elgato::Identity inputIdentity;      // who the selected input is: only an Elgato Wave gets the treatment below
    std::string inputLabel;              // "Elgato Wave:3", for the words
    bool captureLinkable = false;        // the input's ports and the engine's mic ports are all there
    bool captureLinked = false;          // all capture links exist in the graph

    uint32_t waveDeviceId = SPA_ID_INVALID;   // the Elgato Wave pw_device whose profile may be fixed (the input's, else the output's)
    bool manageWaveProfile = false;           // settings involve the Wave, so its profile may be fixed
};

/** What WaveGuard wants the rest of the backend to do and show. */
struct GuardResult
{
    bool allowCapture = false;
    bool allowPlayback = true;
    hr::MicState mic = hr::MicState::NotFound;
    juce::String message;
};

/**
    The Elgato Wave firmware (every model, see ElgatoModels.h) shares one clock between capture and playback: if playback
    opens before capture, the mic is silent for the life of that capture stream. WaveGuard therefore
      1. makes sure the device profile has both a source and a sink (and is not Pro Audio, which has no hardware
         gain control: ProfileRules.h),
      2. unmutes the source route,
      3. lets capture links exist first and only allows the headphone links once mic buffers
         are flowing (or 500 ms passed),
      4. watches for silence: silent since capture started means the firmware bug (recover:
         profile off, good profile again, capture first again; at most 2 automatic attempts per
         60 s), silent after having heard signal means the mic was muted on the device (never
         auto-recovered).
    update() is called from the loop thread on every pass; it never blocks.

    All of this is for an Elgato Wave input (devices::guardPlanFor). Any other mic gets no capture-first ordering, no
    recovery, no unmute and no profile change from here: when it is silent it is reported, nothing more. A manual "Fix my
    mic" on such a mic only re-links it; no device is ever cycled.
*/
class WaveGuard
{
public:
    WaveGuard (EngineFilter& engineFilter, DeviceControl& deviceControl, PwRegistry& pwRegistry)
        : filter (engineFilter), devices (deviceControl), registry (pwRegistry) {}

    void reset();
    void update (const GuardInputs& inputs, int64_t nowMs);
    const GuardResult& result() const noexcept { return out; }

    /** The user pressed "Fix my mic" / "Try again". Handled on the next update(). */
    void requestManualRecovery() { manualRequested = true; }

    /** Called (loop thread) when a recovery attempt has been judged: true = the mic delivers signal again. */
    std::function<void (bool micOk)> onRecoveryFinished;

private:
    enum class Phase { Idle, Linking, Confirming, Running, TearDown, ProfileOff, ProfileOn };

    static bool isRecoveryPhase (Phase p) noexcept
    {
        return p == Phase::TearDown || p == Phase::ProfileOff || p == Phase::ProfileOn;
    }

    void beginSession (const GuardInputs& inputs, int64_t now);
    void ensureWaveProfile (const GuardInputs& inputs, int64_t now);
    elgato::Identity identityOfDevice (uint32_t deviceId) const;
    static const char* preferredProfile (const elgato::Identity& who);
    void logSession (const GuardInputs& inputs) const;
    void ensureSourceUnmuted (const GuardInputs& inputs);
    void watchdog (const GuardInputs& inputs, int64_t now);
    bool canAutoRecover (int64_t now);
    void startRecovery (const GuardInputs& inputs, int64_t now, bool manual);
    void runRecovery (const GuardInputs& inputs, int64_t now);
    void finishRecovery (const GuardInputs& inputs, int64_t now);
    void recoveryFinished (bool micOk);
    void setMic (hr::MicState state);


    EngineFilter& filter;
    DeviceControl& devices;
    PwRegistry& registry;

    GuardResult out;
    Phase phase = Phase::Idle;
    int64_t phaseStartMs = 0;
    int64_t sessionStartMs = 0;
    std::string sessionKey;
    std::string sessionInputName;
    uint32_t probeToken = 0;
    hr::devices::GuardPlan plan;              // what this input is allowed to get (set on every update)
    hr::devices::MicWording wording;          // and how to talk about it

    bool routeChecked = false;
    int64_t profileFixAtMs = 0;

    bool inRecovery = false;
    bool manualRequested = false;
    int episodeAttempts = 0;
    int64_t suppressAutoUntilMs = 0;
    int64_t inputMissingSinceMs = 0;
    std::deque<int64_t> attemptTimes;

    uint32_t recoveryDeviceId = SPA_ID_INVALID;
    int recoveryTargetProfile = -1;   // the profile to switch back to after "off"
    bool attemptOpen = false;         // a recovery attempt has started and has not been judged yet
};

} // namespace hr::pw
