// Implementation of WaveGuard (capture-first state machine, profile/route fixes, silence watchdog).
#include "WaveGuard.h"

namespace hr::pw
{

namespace
{
    constexpr int64_t kSilenceMs = 1500;          // silent this long => something is wrong
    constexpr int64_t kConfirmMs = 500;           // max wait for mic buffers before opening playback
    constexpr int64_t kLinkGiveUpMs = 4000;       // capture can't be linked: don't hold the headphones hostage
    constexpr int64_t kNoDataMs = 3000;           // linked but no mic buffers at all
    constexpr int64_t kAttemptWindowMs = 60000;   // at most kMaxAutoAttempts per this window
    constexpr int kMaxAutoAttempts = 2;
    constexpr int64_t kProfileRetryMs = 10000;    // don't fight over the profile more often than this
}

void WaveGuard::reset()
{
    out = GuardResult();
    plan = {};
    wording = {};
    phase = Phase::Idle;
    sessionKey.clear();
    sessionInputName.clear();
    routeChecked = false;
    profileFixAtMs = 0;
    inRecovery = false;
    manualRequested = false;
    episodeAttempts = 0;
    suppressAutoUntilMs = 0;
    inputMissingSinceMs = 0;
    attemptTimes.clear();
    recoveryDeviceId = SPA_ID_INVALID;
    recoveryTargetProfile = -1;
    attemptOpen = false;
}

namespace
{
    const char* micStateName (hr::MicState state)
    {
        switch (state)
        {
            case hr::MicState::NotFound:        return "NotFound";
            case hr::MicState::Starting:        return "Starting";
            case hr::MicState::Ok:              return "Ok";
            case hr::MicState::Recovering:      return "Recovering";
            case hr::MicState::SilentFromStart: return "SilentFromStart";
            case hr::MicState::MutedOnDevice:   return "MutedOnDevice";
        }

        return "?";
    }
}

void WaveGuard::setMic (hr::MicState state)
{
    // The integration test reads these lines from the log.
    if (out.mic != state)
        pwLog ("mic state %s -> %s", micStateName (out.mic), micStateName (state));

    out.mic = state;
    out.message = hr::devices::micMessage (state, wording);
}

// -------------------------------------------------------------------------------------------------

void WaveGuard::update (const GuardInputs& in, int64_t now)
{
    plan = in.inputFound ? hr::devices::guardPlanFor (in.inputIdentity) : hr::devices::GuardPlan();
    wording = hr::devices::wordingFor (in.inputIdentity, in.inputLabel);

    // The profile must be right before anything else can work (and even when the input
    // is missing because the active profile has no source at all).
    if (! isRecoveryPhase (phase))
        ensureWaveProfile (in, now);

    if (isRecoveryPhase (phase))
    {
        runRecovery (in, now);
        return;
    }

    if (! in.engineReady || ! in.inputFound)
    {
        if (phase != Phase::Idle)
            pwLog ("no capture device selected any more");

        phase = Phase::Idle;
        sessionKey.clear();
        out.allowCapture = false;
        out.allowPlayback = true;   // headphone audio must work without a mic
        inRecovery = false;
        setMic (hr::MicState::NotFound);

        if (inputMissingSinceMs == 0)
            inputMissingSinceMs = now;

        // The device really went away (unplugged): the next appearance is a fresh start.
        if (now - inputMissingSinceMs > 3000)
            episodeAttempts = 0;

        // Nothing to recover, but the button should still get the profile checked right away.
        if (manualRequested)
        {
            manualRequested = false;
            profileFixAtMs = 0;
        }

        return;
    }

    inputMissingSinceMs = 0;

    const std::string key = in.inputName + "#" + std::to_string (in.inputNodeId);

    if (key != sessionKey)
    {
        sessionKey = key;
        beginSession (in, now);
    }

    if (manualRequested)
    {
        manualRequested = false;
        startRecovery (in, now, true);
        runRecovery (in, now);
        return;
    }

    switch (phase)
    {
        case Phase::Linking:
        {
            out.allowCapture = true;
            out.allowPlayback = false;

            ensureSourceUnmuted (in);

            if (in.captureLinked)
            {
                phase = Phase::Confirming;
                phaseStartMs = now;
            }
            else if (now - phaseStartMs >= kLinkGiveUpMs)
            {
                pwLog ("capture could not be linked; opening playback anyway");
                phase = Phase::Running;
                phaseStartMs = now;
            }

            setMic (inRecovery ? hr::MicState::Recovering : hr::MicState::Starting);
            break;
        }

        case Phase::Confirming:
        {
            out.allowCapture = true;
            out.allowPlayback = false;

            ensureSourceUnmuted (in);
            const auto reading = filter.readMicProbe (probeToken);

            if (! in.captureLinked)
            {
                phase = Phase::Linking;
                phaseStartMs = now;
            }
            else if ((reading.valid && reading.liveCycles >= 3) || now - phaseStartMs >= kConfirmMs)
            {
                pwLog ("capture is flowing (%s), opening playback", reading.valid ? "mic buffers seen" : "timeout");
                phase = Phase::Running;
                phaseStartMs = now;
            }

            setMic (inRecovery ? hr::MicState::Recovering : hr::MicState::Starting);
            break;
        }

        case Phase::Running:
            out.allowCapture = true;
            out.allowPlayback = true;
            watchdog (in, now);
            break;

        case Phase::Idle:
        case Phase::TearDown:
        case Phase::ProfileOff:
        case Phase::ProfileOn:
            break;
    }

    // Playback opens exactly when we reach Running (set above).
    out.allowPlayback = (phase == Phase::Running);
}

void WaveGuard::beginSession (const GuardInputs& in, int64_t now)
{
    // Only an Elgato Wave input needs its mic linked and flowing before the headphones open. Any other input and output
    // are linked together, and the watchdog below only reports.
    phase = plan.captureFirst ? Phase::Linking : Phase::Running;
    phaseStartMs = now;
    sessionStartMs = now;
    sessionInputName = in.inputName;
    routeChecked = false;
    probeToken = filter.resetMicProbe();
    pwLog ("new capture session on %s", in.inputName.c_str());
    logSession (in);
}

void WaveGuard::logSession (const GuardInputs& in) const
{
    const std::string label = in.inputLabel.empty() ? in.inputName : in.inputLabel;

    if (! plan.captureFirst)
    {
        pwLog ("WaveGuard: input is %s (not an Elgato Wave): no capture-first ordering, no recovery", label.c_str());
        return;
    }

    if (in.inputIdentity.model != nullptr)
        pwLog ("WaveGuard: input is %s (shared clock: %s): capture first, dead-mic recovery on", label.c_str(),
               elgato::describe (in.inputIdentity.model->clock));
    else
        pwLog ("WaveGuard: input is %s (an Elgato Wave we do not know yet): capture first, dead-mic recovery on", label.c_str());
}

// -------------------------------------------------------------------------------------------------
// Profile and route

void WaveGuard::ensureWaveProfile (const GuardInputs& in, int64_t now)
{
    if (! in.manageWaveProfile || in.waveDeviceId == SPA_ID_INVALID)
        return;

    const auto who = identityOfDevice (in.waveDeviceId);

    if (! hr::devices::mayFixProfile (who))
        return;   // profiles are only ever switched on an Elgato Wave

    devices.watch (in.waveDeviceId);
    const auto* state = devices.state (in.waveDeviceId);

    if (state == nullptr || ! state->profilesSeen || ! state->activeSeen)
        return;   // still waiting for the device to tell us

    if (profiles::activeProfileIsGood (*state, true))
        return;

    if (now - profileFixAtMs < kProfileRetryMs)
        return;

    const int best = profiles::bestProfile (*state, true, preferredProfile (who));

    if (best < 0 || best == state->activeProfile)
        return;

    profileFixAtMs = now;
    pwLog ("Wave profile '%s' is not usable (%s), switching to profile %d",
           state->activeProfileName.c_str(), profiles::whyActiveProfileIsNotGood (*state, true), best);
    devices.setProfile (in.waveDeviceId, best, true);
}

elgato::Identity WaveGuard::identityOfDevice (uint32_t deviceId) const
{
    const auto* device = registry.device (deviceId);
    return device != nullptr ? device->identity() : elgato::Identity();
}

// The profile that gets the head start when one has to be chosen: the model's own, or analog + mono-fallback for a Wave
// we do not know ("" for a model whose card is not that layout: the best one the card offers).
const char* WaveGuard::preferredProfile (const elgato::Identity& who)
{
    return who.model != nullptr ? who.model->goodProfile : profiles::kDefaultPreferred;
}

void WaveGuard::ensureSourceUnmuted (const GuardInputs& in)
{
    if (routeChecked || ! plan.unmute || in.inputDeviceId == SPA_ID_INVALID)
        return;

    devices.watch (in.inputDeviceId);
    const auto* state = devices.state (in.inputDeviceId);

    if (state == nullptr)
        return;

    const auto* route = DeviceControl::findRoute (*state, SPA_DIRECTION_INPUT, in.inputCardDevice);

    if (route == nullptr)
        return;   // routes not known yet: check again next pass

    routeChecked = true;

    if (route->mute || route->softMute)
    {
        pwLog ("the Wave source was muted in software, unmuting");
        devices.setRoute (in.inputDeviceId, *route, false, std::nullopt);
    }
}

// -------------------------------------------------------------------------------------------------
// Silence watchdog

bool WaveGuard::canAutoRecover (int64_t now)
{
    while (! attemptTimes.empty() && now - attemptTimes.front() > kAttemptWindowMs)
        attemptTimes.pop_front();

    return episodeAttempts < kMaxAutoAttempts
        && (int) attemptTimes.size() < kMaxAutoAttempts
        && now >= suppressAutoUntilMs;
}

void WaveGuard::watchdog (const GuardInputs& in, int64_t now)
{
    const auto reading = filter.readMicProbe (probeToken);
    const double silentMs = reading.valid ? reading.silentMs : 0.0;
    const bool seen = reading.valid && reading.seenSignal;
    const bool noMicData = now - sessionStartMs >= kNoDataMs && (! reading.valid || reading.liveCycles == 0);

    if (seen && silentMs < (double) kSilenceMs)
    {
        if (attemptOpen)
            recoveryFinished (true);

        inRecovery = false;
        episodeAttempts = 0;
        setMic (hr::MicState::Ok);
        return;
    }

    if (seen)
    {
        // We heard the mic, then it went quiet: muted on the device. Recovery can't help.
        if (attemptOpen)
            recoveryFinished (true);   // it did deliver signal after the recovery

        inRecovery = false;
        setMic (hr::MicState::MutedOnDevice);
        return;
    }

    if (silentMs >= (double) kSilenceMs || noMicData)
    {
        if (attemptOpen)
            recoveryFinished (false);

        if (plan.recoverSilentMic && canAutoRecover (now))
        {
            pwLog ("no signal since capture started (%.0f ms silent, %u live mic cycles): recovering",
                   silentMs, (unsigned) reading.liveCycles);
            startRecovery (in, now, false);
            return;
        }

        inRecovery = false;
        setMic (hr::MicState::SilentFromStart);
        return;
    }

    setMic (inRecovery ? hr::MicState::Recovering : hr::MicState::Starting);
}

// -------------------------------------------------------------------------------------------------
// Recovery

void WaveGuard::startRecovery (const GuardInputs& in, int64_t now, bool manual)
{
    inRecovery = true;

    // Only an Elgato Wave's own device is ever cycled; any other mic gets the link-level restart and nothing else.
    recoveryDeviceId = plan.recoverSilentMic ? in.inputDeviceId : SPA_ID_INVALID;

    if (manual)
    {
        // One deliberate attempt; don't chain automatic ones behind it.
        episodeAttempts = kMaxAutoAttempts;
        suppressAutoUntilMs = now + kAttemptWindowMs;
    }
    else
    {
        attemptTimes.push_back (now);
        ++episodeAttempts;
    }

    // Come back to the profile the user had if it was a good one, else to the best one.
    recoveryTargetProfile = -1;
    attemptOpen = true;

    if (const auto* state = devices.state (recoveryDeviceId))
    {
        const auto preferred = preferredProfile (identityOfDevice (recoveryDeviceId));
        recoveryTargetProfile = profiles::activeProfileIsGood (*state, true) ? state->activeProfile
                                                                             : profiles::bestProfile (*state, true, preferred);
    }

    phase = Phase::TearDown;
    phaseStartMs = now;
    out.allowCapture = false;
    out.allowPlayback = false;
    setMic (hr::MicState::Recovering);
}

void WaveGuard::runRecovery (const GuardInputs& in, int64_t now)
{
    out.allowCapture = false;
    out.allowPlayback = false;
    setMic (hr::MicState::Recovering);

    const auto elapsed = now - phaseStartMs;
    const auto* state = recoveryDeviceId != SPA_ID_INVALID ? devices.state (recoveryDeviceId) : nullptr;
    const bool canTouchProfile = state != nullptr && state->profilesSeen;

    switch (phase)
    {
        case Phase::TearDown:
        {
            // The headphone and mic links are destroyed by the next reconcile; give it a moment.
            if (elapsed < 400)
                break;

            const int off = canTouchProfile ? profiles::findProfile (*state, "off") : -1;

            // Never switch the device off unless we know which profile to bring it back to.
            if (off >= 0 && recoveryTargetProfile >= 0)
            {
                devices.setProfile (recoveryDeviceId, off, false);
                phase = Phase::ProfileOff;
                phaseStartMs = now;
            }
            else
            {
                finishRecovery (in, now);   // no device to cycle: the link-level restart is all we can do
            }

            break;
        }

        case Phase::ProfileOff:
        {
            const bool gone = registry.nodeByName (sessionInputName) == nullptr;

            if (gone || elapsed >= 2000)
            {
                devices.setProfile (recoveryDeviceId, recoveryTargetProfile, true);
                profileFixAtMs = now;   // the profile check must not race the change we just made
                phase = Phase::ProfileOn;
                phaseStartMs = now;
            }

            break;
        }

        case Phase::ProfileOn:
        {
            const bool back = in.inputFound && in.captureLinkable && registry.nodeByName (sessionInputName) != nullptr;

            if ((back && elapsed >= 300) || elapsed >= 6000)
                finishRecovery (in, now);

            break;
        }

        case Phase::Idle:
        case Phase::Linking:
        case Phase::Confirming:
        case Phase::Running:
            break;
    }
}

void WaveGuard::finishRecovery (const GuardInputs& in, int64_t now)
{
    if (! in.inputFound)
    {
        pwLog ("the mic did not come back after recovery");
        recoveryFinished (false);
        phase = Phase::Idle;
        sessionKey.clear();
        inRecovery = false;
        return;
    }

    // Same order as at start: capture first, then (once it flows) the headphones.
    sessionKey = in.inputName + "#" + std::to_string (in.inputNodeId);
    beginSession (in, now);
    out.allowCapture = true;
    out.allowPlayback = false;
}

void WaveGuard::recoveryFinished (bool micOk)
{
    attemptOpen = false;
    pwLog (micOk ? "WaveGuard: recovery finished (mic ok)" : "WaveGuard: recovery finished (still silent)");

    if (onRecoveryFinished)
        onRecoveryFinished (micOk);
}

} // namespace hr::pw
