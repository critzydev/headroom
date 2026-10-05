// AppController: applies model changes to the engine, backend and plugins, and exposes the UI-facing operations.
#include "app/AppController.h"
#include "engine/builtin/Presets.h"

#include "app/Log.h"
#include "app/Paths.h"
#include "backend/NullBackend.h"
#include "backend/wave/WaveGain.h"
#include "engine/builtin/BuiltinFx.h"
#include "engine/builtin/NoiseRemoval.h"
#include "plugins/CrashGuard.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace hr
{
namespace
{
    constexpr double kDefaultSampleRate = 48000.0;
    constexpr float kMaxFaderDb = 6.0f;
    constexpr int kTimerMs = 50;                    // rate-change poll + FX garbage collection
    constexpr int kStateCaptureEveryTicks = 100;    // 5 s
    constexpr double kHearRemovedMaxMs = 30000.0;   // "Hear what's removed" lets go by itself after this long

    bool asBool (const juce::var& v)   { return ! v.isVoid() && (int) v != 0; }
    float asFloat (const juce::var& v) { return v.isVoid() ? 0.0f : (float) (double) v; }
    double asDouble (const juce::var& v, double fallback) { return v.isVoid() ? fallback : (double) v; }

    constexpr int kMaxSafeGainWrites = 3;               // asks the mic this often for the lower gain before saying it will not
    // ... and waits options.safeGainRetryMs between asks, so that the answer can arrive

    // "24" or "24.5": whole numbers stay tidy.
    juce::String plainDb (double db)
    {
        return std::abs (db - std::round (db)) < 0.05 ? juce::String (juce::roundToInt (db)) : juce::String (db, 1);
    }

    juce::AudioProcessor* rawProcessor (const FxSlotPtr& slot)
    {
        return slot != nullptr ? &slot->getProcessor() : nullptr;
    }

    // Where a model channel lives in the engine: the mic has its own, app channels their slot, input channels the block after the mic.
    int engineIndexOf (const ChannelInfo& info)
    {
        if (info.isMic())
            return kMicChannel;

        if (info.isInput())
            return juce::isPositiveAndBelow (info.slot, kMaxInputChannels) ? kInputChannel0 + info.slot : -1;

        return juce::isPositiveAndBelow (info.slot, kMaxAppChannels) ? info.slot : -1;
    }
}

// ---------------------------------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------------------------------

AppController::AppController (const Options& o)
    : options (o),
      engine (std::make_unique<AudioEngine>()),
      plugins (o.persist ? Paths::getPluginsFile() : juce::File(), o.persist ? Paths::getScanDeadMansFile() : juce::File()),
      state (o.persist ? (o.stateFile != juce::File() ? o.stateFile : Paths::getStateFile()) : juce::File())
{
    JUCE_ASSERT_MESSAGE_THREAD

    holdingDefaults = options.holdDefaults;

    const auto loadResult = state.load();

    switch (loadResult)
    {
        case AppState::LoadResult::Loaded:               Log::info ("Loaded settings"); break;
        case AppState::LoadResult::CreatedDefaults:      Log::info ("No saved settings: starting with defaults"); break;
        case AppState::LoadResult::RecoveredFromCorrupt: Log::warn ("Saved settings were unreadable: starting with defaults"); break;
        case AppState::LoadResult::UnsupportedVersion:   Log::warn ("Saved settings are from a newer Headroom: starting with defaults"); break;
    }

    // A plugin that took the app down last time is switched off and the banner offers to turn it back on.
    if (options.persist)
    {
        for (auto crashId : CrashGuard::readAndClearLastCrash (Paths::getCrashFile()))
        {
            for (auto channel : state.getChannels())
                for (auto slotTree : AppState::getFx (channel))
                    if ((uint32_t) (int) slotTree[ids::crashId] == crashId)
                    {
                        const auto uid = slotTree[ids::uid].toString();
                        const auto name = slotTree[ids::name].toString();

                        Log::warn ("Turning off " + name + " because it crashed");
                        slotTree.setProperty (ids::disabled, 1, nullptr);

                        Notice notice;
                        notice.id = "crash:" + uid;
                        notice.severity = Notice::Severity::warning;
                        notice.text = name + " crashed and was turned off.";
                        notice.actionLabel = "Turn back on";
                        notice.action = [this, uid] { reenableFx (uid); };
                        postNotice (std::move (notice));
                    }
        }
    }

    engine->prepare (kDefaultSampleRate);
    plugins.setExtraFolders (state.getPluginFolders());

    // A plugin that hung or crashed the scanner is skipped from now on: say so once, so a missing plugin is explained.
    plugins.onScanFinished = [this] (const PluginManager::ScanStats& stats)
    {
        if (stats.skipped.isEmpty())
            return;

        const auto nameOf = [] (const juce::String& identifier)
        {
            return juce::File::isAbsolutePath (identifier) ? juce::File (identifier).getFileNameWithoutExtension() : identifier;
        };

        Notice notice;
        notice.id = "plugin-scan-skipped";
        notice.severity = Notice::Severity::info;
        notice.text = stats.skipped.size() == 1 ? nameOf (stats.skipped[0]) + " could not be checked and was skipped."
                                                : juce::String (stats.skipped.size()) + " plugins could not be checked and were skipped.";
        postNotice (std::move (notice));
    };

    if (options.demo || options.demoMicMuted)
        backend = std::make_unique<NullBackend> (options.demoMicMuted);
    else
        backend = createPlatformBackend();

    // Safe gain follows what the mic says, so it looks at every change first; the UI is told after.
    backend->onChanged = [this]
    {
        maybeRestoreMicGain();
        updateSafeGain();

        if (onBackendChanged)
            onBackendChanged();
    };

    state.setBeforeSave ([this] { captureFxStates (false); });
    state.addListener (this);

    startTimer (kTimerMs);
}

AppController::~AppController()
{
    stopTimer();
    setHearingRemoved (false);

    for (auto& [uid, live] : liveSlots)
        if (auto* noise = dynamic_cast<NoiseRemoval*> (&live.slot->getProcessor()))
            noise->onHearRemovedRequest = nullptr;

    state.removeListener (this);

    if (options.persist)
    {
        captureFxStates (false);
        state.saveNow();
    }

    state.setBeforeSave ({});
    backend->onChanged = nullptr;
    backend->stop();            // no more RT calls into the engine after this
    plugins.closeAllEditors();
}

void AppController::start()
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (started)
        return;

    // Whole model -> engine
    for (auto mix : { Mix::Monitor, Mix::Stream })
        applyMixToEngine (state.getMix (mix));

    for (auto channel : state.getChannels())
        applyChannelToEngine (channel);

    for (auto channel : state.getChannels())
        rebuildChain (channel[ids::id].toString());

    // Whole model -> backend (the interface requires implementations to accept this before start())
    pushChannelsToBackend();
    pushDevicesToBackend();
    pushRoutingRules();
    backend->setMakeDefault (state.getMakeDefault() && ! holdingDefaults);
    backend->setKeepDefault (state.getKeepDefault() && ! holdingDefaults);
    backend->setLatencyQuantum (state.getLatency());

    // ClipGuard: put it back the way the user had it, if they want that (the backend does it as soon as it may talk to
    // the mic: it reads first and only writes if the mic is not already that way).
    if (asBool (state.getSetting (ids::restoreClipGuard)))
        pushClipGuard();

    backend->setExperimentalHardware (getExperimentalHardware());

    if (options.startBackend)
    {
        if (! backend->start (*engine))
            Log::error ("The audio backend could not be started");
    }

    maybeRestoreMicGain();
    updateSafeGain();

    // Look for new plugins in the background (a child process, never this thread). Only a real run does it: the
    // screenshot mode and the tests neither persist nor start the backend.
    if (options.persist && options.startBackend)
        plugins.startBackgroundScan (false);

    started = true;
}

int AppController::engineIndexFor (const juce::String& channelId) const
{
    auto channel = state.findChannel (channelId);

    return channel.isValid() ? engineIndexOf (AppState::describe (channel)) : -1;
}

void AppController::timerCallback()
{
    // The RT thread saw a rate we haven't prepared for: re-prepare everything at that rate.
    const auto requested = engine->requestedRate.exchange (0.0);

    if (requested > 0.0 && std::abs (requested - engine->getPreparedRate()) > 0.5)
    {
        Log::info ("Sample rate changed to " + juce::String (requested) + " Hz");

        // False only if the audio thread is stuck inside a plugin; it asks for the rate again once it is back.
        if (! engine->prepare (requested))
            Log::warn ("The audio thread did not respond to the sample-rate change; will retry");
    }

    engine->collectGarbage();

    if (hearingRemoved && juce::Time::getMillisecondCounterHiRes() - hearingRemovedSince > kHearRemovedMaxMs)
        setHearingRemoved (false);

    if (++tickCount % kStateCaptureEveryTicks == 0)
        captureFxStates (true);

    // Once a second: Safe gain waits for the mic to answer, and asks again if it has not taken the lower gain.
    if (tickCount % 20 == 0 && (safeGainInfo.on || safeGainRestorePending))
        updateSafeGain();
}

// ---------------------------------------------------------------------------------------------------
// Model -> engine / backend
// ---------------------------------------------------------------------------------------------------

void AppController::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    const auto type = tree.getType();

    if (type == ids::CHANNEL)           channelPropertyChanged (tree, property);
    else if (type == ids::MIX)          { applyMixToEngine (tree); if (property == ids::outputs) pushDevicesToBackend(); }
    else if (type == ids::SETTINGS)     settingChanged (property);
    else if (type == ids::SLOT)         slotPropertyChanged (tree, property);
    else if (type == ids::RULE)         pushRoutingRules();
}

void AppController::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    if (child.getType() == ids::CHANNEL && parent.getType() == ids::CHANNELS)
    {
        applyChannelToEngine (child);
        rebuildChain (child[ids::id].toString());
        pushChannelsToBackend();
    }
    else if (child.getType() == ids::SLOT && parent.getType() == ids::FX)
    {
        rebuildChain (parent.getParent()[ids::id].toString());
        markMicPresetCustom (parent);
    }
    else if (child.getType() == ids::RULE)
    {
        pushRoutingRules();
    }
}

void AppController::markMicPresetCustom (const juce::ValueTree& fxNode)
{
    if (applyingPreset || fxNode.getParent()[ids::id].toString() != values::micChannelId)
        return;

    if (state.getSetting (ids::micPreset).toString() != "custom")
        state.setSetting (ids::micPreset, "custom");
}

void AppController::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int)
{
    if (child.getType() == ids::CHANNEL && parent.getType() == ids::CHANNELS)
    {
        deactivateChannel (child);
        pushChannelsToBackend();
        pushRoutingRules();
    }
    else if (child.getType() == ids::SLOT && parent.getType() == ids::FX)
    {
        if (parent.getParent().isValid())
        {
            rebuildChain (parent.getParent()[ids::id].toString());
            markMicPresetCustom (parent);
        }
    }
    else if (child.getType() == ids::RULE)
    {
        pushRoutingRules();
    }
}

void AppController::valueTreeChildOrderChanged (juce::ValueTree& parent, int, int)
{
    if (parent.getType() == ids::FX)
    {
        rebuildChain (parent.getParent()[ids::id].toString());
        markMicPresetCustom (parent);
    }
}

void AppController::channelPropertyChanged (const juce::ValueTree& channel, const juce::Identifier& property)
{
    if (property == ids::monitorDb || property == ids::monitorMuted || property == ids::streamDb || property == ids::streamMuted)
        applyChannelToEngine (channel);
    else if (property == ids::name || property == ids::device || property == ids::channels)
        pushChannelsToBackend();
}

void AppController::slotPropertyChanged (const juce::ValueTree& slotTree, const juce::Identifier& property)
{
    if (property == ids::bypassed)
    {
        if (auto it = liveSlots.find (slotTree[ids::uid].toString()); it != liveSlots.end())
            it->second.slot->bypassed.store (asBool (slotTree[ids::bypassed]), std::memory_order_relaxed);
    }
    else if (property == ids::disabled)
    {
        rebuildChain (AppState::getChannelIdOfSlot (slotTree));
    }
    // Everything else (state, name, ...) doesn't change what runs.
}

void AppController::settingChanged (const juce::Identifier& property)
{
    // outputDevice is only the first of the Headphones list (AppState::setOutputs): the list's own change tells the backend.
    if (property == ids::inputDevice)
    {
        pushDevicesToBackend();
    }
    else if (property == ids::latency)
    {
        backend->setLatencyQuantum (state.getLatency());
    }
    else if (property == ids::makeDefault)
    {
        backend->setMakeDefault (state.getMakeDefault() && ! holdingDefaults);
    }
    else if (property == ids::keepDefault)
    {
        backend->setKeepDefault (state.getKeepDefault() && ! holdingDefaults);
    }
    else if (property == ids::fallbackChannel)
    {
        pushRoutingRules();
    }
    else if (property == ids::micGain)
    {
        // While Safe gain owns the mic's gain the stored value is only a memory: it is put back when Safe gain goes.
        if (! asBool (state.getSetting (ids::safeGain)))
            backend->setHardwareMicGain (asFloat (state.getSetting (ids::micGain)));
    }
    else if (property == ids::restoreMicGain)
    {
        // Switching "restore" on means: keep the mic the way it is now. Otherwise the next start would put back a value
        // from long ago (the slider's first position) and change the level without anyone touching anything.
        const auto current = backend->getStatus().hardwareMicGain;

        if (asBool (state.getSetting (ids::restoreMicGain)) && ! asBool (state.getSetting (ids::safeGain)) && current >= 0.0f
            && std::abs (current - asFloat (state.getSetting (ids::micGain))) > 0.005f)
            state.setSetting (ids::micGain, juce::jlimit (0.0f, 1.0f, current));
    }
    else if (property == ids::headphoneVolume)
    {
        backend->setHardwareHeadphoneVolume (asFloat (state.getSetting (ids::headphoneVolume)));
    }
    else if (property == ids::clipGuard)
    {
        pushClipGuard();
    }
    else if (property == ids::restoreClipGuard)
    {
        if (asBool (state.getSetting (ids::restoreClipGuard)))
            pushClipGuard();
    }
    else if (property == ids::experimentalHardware)
    {
        backend->setExperimentalHardware (getExperimentalHardware());
    }
    else if (property == ids::safeGain)
    {
        safeGainSwitched (asBool (state.getSetting (ids::safeGain)));
    }
    else if (property == ids::safeGainBase)
    {
        updateSafeGain();
    }
    else if (property == ids::pluginFolders)
    {
        plugins.setExtraFolders (state.getPluginFolders());

        if (started && options.persist && options.startBackend)
            plugins.startBackgroundScan (false);        // look in the new folder now (only files not seen before)
    }
}

void AppController::applyChannelToEngine (const juce::ValueTree& channel)
{
    const auto info = AppState::describe (channel);
    const auto index = engineIndexFor (info.id);

    if (index < 0)
        return;

    engine->setChannelActive (index, true);
    engine->setChannelGainDb (index, Mix::Monitor, info.monitorDb);
    engine->setChannelGainDb (index, Mix::Stream, info.streamDb);
    engine->setChannelMuted (index, Mix::Monitor, info.monitorMuted);
    engine->setChannelMuted (index, Mix::Stream, info.streamMuted);

    // The processed mic that apps (Discord) get follows the mic's stream mute: muting the mic in the Stream mix
    // mutes it everywhere apps can hear it. There is no separate "mute mic for apps" field in the schema.
    if (info.isMic())
    {
        engine->setMicOutMuted (info.streamMuted);

        // "Hear what's removed" takes over the mic's routing until it ends (see setHearingRemoved).
        if (hearingRemoved)
        {
            engine->setChannelGainDb (index, Mix::Monitor, 0.0f);
            engine->setChannelMuted (index, Mix::Monitor, false);
            engine->setChannelMuted (index, Mix::Stream, true);
            engine->setMicOutMuted (true);
        }
    }
}

void AppController::setHearingRemoved (bool on)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (on != hearingRemoved)
    {
        hearingRemoved = on;
        hearingRemovedSince = juce::Time::getMillisecondCounterHiRes();

        if (auto mic = state.getMicChannel(); mic.isValid())
            applyChannelToEngine (mic);         // from the model: everything is back as it was when this ends
    }

    // The effects' own switch follows (it may be this call that ends it: timeout, drawer closed, quit).
    for (auto& [uid, live] : liveSlots)
        if (auto* noise = dynamic_cast<NoiseRemoval*> (&live.slot->getProcessor()))
            noise->setHearRemoved (on);
}

void AppController::applyMixToEngine (const juce::ValueTree& mix)
{
    if (! mix.isValid())
        return;

    const auto which = mix[ids::id].toString() == values::mixStream ? Mix::Stream : Mix::Monitor;

    engine->setMasterGainDb (which, asFloat (mix[ids::gainDb]));
    engine->setMasterMuted (which, asBool (mix[ids::muted]));
    engine->setLimiterEnabled (which, mix[ids::limiter].isVoid() || asBool (mix[ids::limiter]));
}

void AppController::deactivateChannel (const juce::ValueTree& channel)
{
    const auto info = AppState::describe (channel);
    const auto index = engineIndexOf (info);

    for (auto slotTree : AppState::getFx (channel))
    {
        const auto uid = slotTree[ids::uid].toString();
        plugins.closeEditor (uid);
        liveSlots.erase (uid);
        slotErrors.erase (uid);
    }

    if (juce::isPositiveAndBelow (index, kNumChannels))
    {
        engine->setChain (index, nullptr);
        engine->setChannelActive (index, info.isMic());     // the mic never goes inactive
    }
}

void AppController::pushChannelsToBackend()
{
    backend->setChannels (state.getChannelEndpoints());
    backend->setInputChannels (state.getInputEndpoints());
}

void AppController::pushDevicesToBackend()
{
    backend->setDevices (state.getInputDevice(), state.getOutputs (Mix::Monitor), state.getOutputs (Mix::Stream));
}

void AppController::pushRoutingRules()
{
    backend->setRoutingRules (state.getRoutingRules(), state.getFallbackChannel());
}

// ---------------------------------------------------------------------------------------------------
// FX chains
// ---------------------------------------------------------------------------------------------------

FxSlotPtr AppController::createSlot (const SlotInfo& info, juce::String& error)
{
    const CrashGuard::ScopedUiSlot crashScope (info.crashId);

    std::shared_ptr<juce::AudioProcessor> processor;

    if (info.isBuiltin())
    {
        if (auto builtin = BuiltinFx::create (info.builtinId))
            processor = std::move (builtin);
        else
            error = "Unknown built-in effect '" + info.builtinId + "'";
    }
    else
    {
        juce::PluginDescription description;

        if (auto xml = juce::XmlDocument::parse (info.descXml); xml != nullptr && description.loadFromXml (*xml))
        {
            if (auto instance = plugins.createInstance (description, engine->getPreparedRate(), kMaxBlock, error))
                processor = std::move (instance);
        }
        else
        {
            error = "The saved plugin description could not be read";
        }
    }

    if (processor == nullptr)
        return nullptr;

    if (auto* noise = dynamic_cast<NoiseRemoval*> (processor.get()))
        noise->onHearRemovedRequest = [this] (bool on) { setHearingRemoved (on); };

    auto slot = std::make_shared<FxSlot> (processor, info.uid, info.crashId);
    slot->configure (engine->getPreparedRate(), kMaxBlock);

    // Restore after prepare. Empty state means "defaults", which is what a fresh instance has.
    if (info.stateBase64.isNotEmpty())
    {
        juce::MemoryOutputStream decoded;

        if (juce::Base64::convertFromBase64 (decoded, info.stateBase64))
            processor->setStateInformation (decoded.getData(), (int) decoded.getDataSize());
    }

    return slot;
}

void AppController::rebuildChain (const juce::String& channelId)
{
    JUCE_ASSERT_MESSAGE_THREAD

    auto channel = state.findChannel (channelId);
    const auto index = engineIndexFor (channelId);

    if (! channel.isValid() || index < 0)
        return;

    std::vector<FxSlotPtr> chainSlots;
    std::set<juce::String> inChain;

    for (auto slotTree : AppState::getFx (channel))
    {
        const auto info = AppState::describeSlot (slotTree);

        if (info.disabled)
            continue;

        FxSlotPtr slot;

        if (auto it = liveSlots.find (info.uid); it != liveSlots.end())
        {
            slot = it->second.slot;
        }
        else
        {
            juce::String error;
            slot = createSlot (info, error);

            if (slot == nullptr)
            {
                Log::warn ("Could not load " + info.name + ": " + error);
                slotErrors[info.uid] = error;
                continue;
            }

            slotErrors.erase (info.uid);
            liveSlots[info.uid] = { channelId, slot };
        }

        slot->bypassed.store (info.bypassed, std::memory_order_relaxed);
        chainSlots.push_back (slot);
        inChain.insert (info.uid);
    }

    // Slots of this channel that left the chain (removed or disabled): close their editors and let go.
    for (auto it = liveSlots.begin(); it != liveSlots.end();)
    {
        if (it->second.channelId == channelId && inChain.count (it->first) == 0)
        {
            plugins.closeEditor (it->first);
            it = liveSlots.erase (it);
        }
        else
        {
            ++it;
        }
    }

    engine->setChain (index, chainSlots.empty() ? nullptr : FxChain::create (std::move (chainSlots)));
}

void AppController::captureFxStates (bool markDirty)
{
    JUCE_ASSERT_MESSAGE_THREAD

    for (const auto& [uid, live] : liveSlots)
    {
        const CrashGuard::ScopedUiSlot crashScope (live.slot->getCrashId());     // a plugin can crash while saving, too

        juce::MemoryBlock block;
        live.slot->getProcessor().getStateInformation (block);
        state.setSlotState (uid, juce::Base64::toBase64 (block.getData(), block.getSize()), markDirty);
    }
}

juce::AudioProcessor* AppController::getFxProcessor (const juce::String& slotUid) const
{
    if (auto it = liveSlots.find (slotUid); it != liveSlots.end())
        return rawProcessor (it->second.slot);

    return nullptr;
}

std::shared_ptr<juce::AudioProcessor> AppController::getFxProcessorShared (const juce::String& slotUid) const
{
    if (auto it = liveSlots.find (slotUid); it != liveSlots.end())
        return it->second.slot->getProcessorPtr();

    return nullptr;
}

juce::String AppController::getFxError (const juce::String& slotUid) const
{
    if (auto it = slotErrors.find (slotUid); it != slotErrors.end())
        return it->second;

    return {};
}

juce::String AppController::addBuiltinFx (const juce::String& channelId, const juce::String& builtinId, int index)
{
    if (BuiltinFx::find (builtinId) == nullptr)
        return {};

    auto slotTree = state.addBuiltinSlot (channelId, builtinId, index);
    return slotTree.isValid() ? slotTree[ids::uid].toString() : juce::String();
}

juce::String AppController::addPluginFx (const juce::String& channelId, const juce::PluginDescription& description, int index)
{
    auto slotTree = state.addPluginSlot (channelId, description, index);
    return slotTree.isValid() ? slotTree[ids::uid].toString() : juce::String();
}

void AppController::removeFx (const juce::String& channelId, const juce::String& slotUid)
{
    juce::ignoreUnused (channelId);     // uids are unique across the model; kept for symmetry with the other FX calls
    plugins.closeEditor (slotUid);
    state.removeSlot (slotUid);
}

void AppController::moveFx (const juce::String& channelId, const juce::String& slotUid, int newIndex)
{
    juce::ignoreUnused (channelId);
    state.moveSlot (slotUid, newIndex);
}

void AppController::setFxBypassed (const juce::String& channelId, const juce::String& slotUid, bool bypassed)
{
    juce::ignoreUnused (channelId);

    auto slotTree = state.findSlot (slotUid);

    if (slotTree.isValid())
        slotTree.setProperty (ids::bypassed, bypassed ? 1 : 0, nullptr);
}

void AppController::openFxEditor (const juce::String& channelId, const juce::String& slotUid)
{
    if (auto it = liveSlots.find (slotUid); it != liveSlots.end())
    {
        // "‹Plugin› — ‹Channel›": tells two windows of the same plugin on different channels apart.
        const auto slotName = state.findSlot (slotUid)[ids::name].toString();
        const auto channelName = state.findChannel (channelId)[ids::name].toString();
        auto title = slotName.isNotEmpty() ? slotName : it->second.slot->getProcessor().getName();

        if (channelName.isNotEmpty())
            title += juce::String (juce::CharPointer_UTF8 (" \xe2\x80\x94 ")) + channelName;

        const auto crashId = it->second.slot->getCrashId();
        const CrashGuard::ScopedUiSlot crashScope (crashId);
        plugins.openEditor (slotUid, it->second.slot->getProcessor(), title, crashId);
    }
}

void AppController::reenableFx (const juce::String& slotUid)
{
    auto slotTree = state.findSlot (slotUid);

    if (slotTree.isValid())
        slotTree.setProperty (ids::disabled, 0, nullptr);
}

// ---------------------------------------------------------------------------------------------------
// UI operations
// ---------------------------------------------------------------------------------------------------

void AppController::setChannelLevel (const juce::String& channelId, Mix mix, float db)
{
    auto channel = state.findChannel (channelId);

    if (! channel.isValid())
        return;

    db = db <= kMinusInfDb ? kMinusInfDb : juce::jmin (db, kMaxFaderDb);

    channel.setProperty (mix == Mix::Monitor ? ids::monitorDb : ids::streamDb, db, nullptr);

    if (asBool (channel[ids::linked]))
        channel.setProperty (mix == Mix::Monitor ? ids::streamDb : ids::monitorDb, db, nullptr);
}

void AppController::setChannelMuted (const juce::String& channelId, Mix mix, bool muted)
{
    auto channel = state.findChannel (channelId);

    if (channel.isValid())
        channel.setProperty (mix == Mix::Monitor ? ids::monitorMuted : ids::streamMuted, muted ? 1 : 0, nullptr);
}

void AppController::toggleMute (const juce::String& channelId, Mix mix)
{
    auto channel = state.findChannel (channelId);

    if (channel.isValid())
        setChannelMuted (channelId, mix, ! asBool (channel[mix == Mix::Monitor ? ids::monitorMuted : ids::streamMuted]));
}

void AppController::setChannelLinked (const juce::String& channelId, bool linked)
{
    auto channel = state.findChannel (channelId);

    if (channel.isValid())
        channel.setProperty (ids::linked, linked ? 1 : 0, nullptr);
}

void AppController::renameChannel (const juce::String& channelId, const juce::String& newName)
{
    auto channel = state.findChannel (channelId);
    const auto trimmed = newName.trim();

    if (channel.isValid() && trimmed.isNotEmpty())
        channel.setProperty (ids::name, trimmed, nullptr);
}

void AppController::setChannelColour (const juce::String& channelId, const juce::String& rrggbb)
{
    auto channel = state.findChannel (channelId);

    if (channel.isValid())
        channel.setProperty (ids::colour, rrggbb.toUpperCase(), nullptr);
}

void AppController::setChannelIcon (const juce::String& channelId, const juce::String& iconName)
{
    auto channel = state.findChannel (channelId);

    if (channel.isValid())
        channel.setProperty (ids::icon, iconName, nullptr);
}

juce::String AppController::addChannel()
{
    auto channel = state.addAppChannel();
    return channel.isValid() ? channel[ids::id].toString() : juce::String();
}

juce::String AppController::addInputChannel (const juce::String& deviceId, const juce::String& name)
{
    juce::String label;
    bool stereo = false;

    for (const auto& device : backend->getInputs())
        if (device.id == deviceId)
        {
            label = device.label;
            stereo = device.channels == 2;      // a left and a right; more than that is an interface, whose first channel is the mic
        }

    auto channel = state.addInputChannel (deviceId, name.trim().isNotEmpty() ? name : label, stereo);
    return channel.isValid() ? channel[ids::id].toString() : juce::String();
}

void AppController::setChannelDevice (const juce::String& channelId, const juce::String& deviceId)
{
    auto channel = state.findChannel (channelId);

    if (channel.isValid() && channel[ids::kind].toString() == values::kindInput)
    {
        channel.setProperty (ids::device, deviceId, nullptr);

        for (const auto& device : backend->getInputs())
            if (device.id == deviceId)
                channel.setProperty (ids::channels, device.channels == 2 ? values::inputStereo : values::inputMono, nullptr);
    }
}

void AppController::setChannelStereo (const juce::String& channelId, bool stereo)
{
    auto channel = state.findChannel (channelId);

    if (channel.isValid() && channel[ids::kind].toString() == values::kindInput)
        channel.setProperty (ids::channels, stereo ? values::inputStereo : values::inputMono, nullptr);
}

void AppController::removeChannel (const juce::String& channelId)
{
    state.removeChannel (channelId);
}

void AppController::setMasterLevel (Mix mix, float db)
{
    auto m = state.getMix (mix);

    if (m.isValid())
        m.setProperty (ids::gainDb, db <= kMinusInfDb ? kMinusInfDb : juce::jmin (db, kMaxFaderDb), nullptr);
}

void AppController::setMasterMuted (Mix mix, bool muted)
{
    auto m = state.getMix (mix);

    if (m.isValid())
        m.setProperty (ids::muted, muted ? 1 : 0, nullptr);
}

void AppController::setLimiterEnabled (Mix mix, bool enabled)
{
    auto m = state.getMix (mix);

    if (m.isValid())
        m.setProperty (ids::limiter, enabled ? 1 : 0, nullptr);
}

void AppController::setInputDevice (const juce::String& deviceId)
{
    state.setSetting (ids::inputDevice, deviceId);
}

void AppController::setOutputDevice (const juce::String& deviceId)
{
    // One device: the list has just this one in it (and the old single-device properties follow it, AppState::setOutputs).
    state.setOutputs (Mix::Monitor, juce::StringArray (deviceId.isNotEmpty() ? deviceId : values::outputAuto));
}

void AppController::setMixOutputs (Mix mix, const juce::StringArray& deviceIds)
{
    state.setOutputs (mix, deviceIds);
}

void AppController::setOutputSelected (Mix mix, const juce::String& deviceId, bool selected)
{
    auto list = state.getOutputs (mix);

    if (selected)
    {
        if (deviceId == values::outputAuto)
        {
            if (mix == Mix::Monitor)
                list = juce::StringArray (values::outputAuto);
        }
        else
        {
            if (list.contains (values::outputAuto))
            {
                // "Automatic" is one choice: a device ticked next to it is added to what it plays through now, not instead of it.
                const auto status = backend->getStatus();
                juce::String resolved;

                for (const auto& output : mix == Mix::Monitor ? status.headphoneOutputs : status.streamOutputs)
                    if (output.automatic && output.present)
                        resolved = output.id;

                list.removeString (values::outputAuto);

                if (resolved.isNotEmpty())
                    list.addIfNotAlreadyThere (resolved);
            }

            list.addIfNotAlreadyThere (deviceId);
        }
    }
    else
    {
        list.removeString (deviceId);

        if (mix == Mix::Monitor && list.isEmpty())
            return;         // the Headphones mix always plays somewhere
    }

    state.setOutputs (mix, list);
}

void AppController::setLatency (int frames)                     { state.setSetting (ids::latency, frames); }
void AppController::setMakeDefault (bool makeDefault)           { state.setSetting (ids::makeDefault, makeDefault ? 1 : 0); }
void AppController::setKeepDefault (bool keepDefault)           { state.setSetting (ids::keepDefault, keepDefault ? 1 : 0); }

// While the setup wizard is open nothing is made the default yet (and what was made the default is let go of): the person has
// not chosen. Only the backend is told; the model keeps what they chose.
void AppController::setHoldDefaults (bool hold)
{
    holdingDefaults = hold;

    if (started)
    {
        backend->setMakeDefault (state.getMakeDefault() && ! holdingDefaults);
        backend->setKeepDefault (state.getKeepDefault() && ! holdingDefaults);
    }
}

void AppController::setFallbackChannel (const juce::String& id) { state.setSetting (ids::fallbackChannel, id); }

void AppController::setHardwareMicGain (float linear0to1)
{
    state.setSetting (ids::micGain, juce::jlimit (0.0f, 1.0f, linear0to1));
}

void AppController::userSetHardwareMicGain (float linear0to1)
{
    // Before the value is stored: switching "restore" on stores the mic's current level first (settingChanged), which the
    // new value then replaces. Afterwards, the order would put the old level back.
    if (! asBool (state.getSetting (ids::restoreMicGainChosen)) && ! asBool (state.getSetting (ids::safeGain)))
    {
        Log::info ("Mic gain was set by the user: \"Restore mic gain when Headroom starts\" is switched on");
        state.setSetting (ids::restoreMicGainChosen, 1);
        state.setSetting (ids::restoreMicGain, 1);
    }

    setHardwareMicGain (linear0to1);
}

void AppController::setHardwareHeadphoneVolume (float linear0to1)
{
    state.setSetting (ids::headphoneVolume, juce::jlimit (0.0f, 1.0f, linear0to1));
}

void AppController::fixMic()
{
    backend->runMicRecovery();
}

// ---- Wave:3 hardware controls -----------------------------------------------------------------------------------

void AppController::setClipGuard (bool on)
{
    const auto before = state.getSetting (ids::clipGuard);
    state.setSetting (ids::clipGuard, on ? 1 : 0);      // the listener tells the backend when the setting changed

    // Unchanged setting: the mic may still not be in that state (it was unplugged, or someone else changed it).
    if (! before.isVoid() && (int) before == (on ? 1 : 0))
        backend->setClipGuard (on);
}

void AppController::setRestoreClipGuard (bool restore)
{
    state.setSetting (ids::restoreClipGuard, restore ? 1 : 0);
}

void AppController::setRestoreMicGain (bool restore)
{
    state.setSetting (ids::restoreMicGainChosen, 1);        // decided: the slider no longer switches it on by itself
    state.setSetting (ids::restoreMicGain, restore ? 1 : 0);
}

// "Restore mic gain when Headroom starts": the Wave:3 comes up at its own gain (its maximum) whenever it loses power, so
// once each time its route appears (Headroom starting, a replug) the stored gain is put back through the ordinary path.
// Safe gain wins: it owns the mic's gain, and puts back the level from before it was switched on when it goes.
void AppController::maybeRestoreMicGain()
{
    const auto status = backend->getStatus();

    if (status.hardwareMicGain < 0.0f)
    {
        micGainApplied = false;             // no mic route (not started yet, or unplugged): again when it is back
        return;
    }

    if (micGainApplied)
        return;

    micGainApplied = true;

    if (! asBool (state.getSetting (ids::restoreMicGain)) || asBool (state.getSetting (ids::safeGain)))
        return;

    const float wanted = juce::jlimit (0.0f, 1.0f, asFloat (state.getSetting (ids::micGain)));

    if (std::abs (wanted - status.hardwareMicGain) < 0.005f)
        return;

    Log::info ("Restoring the mic gain to " + juce::String (juce::roundToInt (wanted * 100.0f)) + " % (it was "
               + juce::String (juce::roundToInt (status.hardwareMicGain * 100.0f)) + " %)");
    backend->setHardwareMicGain (wanted);
}

void AppController::requestHardwarePermission()
{
    backend->requestHardwarePermission();
}

void AppController::setExperimentalHardware (bool on)
{
    state.setSetting (ids::experimentalHardware, on ? 1 : 0);       // the listener tells the backend
}

bool AppController::getExperimentalHardware() const
{
    return asBool (state.getSetting (ids::experimentalHardware));
}

// Tells the backend which ClipGuard state to keep the mic in. -1 (never chosen) says nothing: the mic is left alone.
void AppController::pushClipGuard()
{
    const auto saved = state.getSetting (ids::clipGuard);
    const int value = saved.isVoid() ? -1 : (int) saved;

    if (value >= 0)
        backend->setClipGuard (value != 0);
}

// ---- Safe gain ----------------------------------------------------------------------------------------------------

void AppController::setSafeGain (bool on)
{
    // Without the permission Headroom cannot tell what the mic really did with the request, so it does not start.
    if (on && ! backend->getHardwareControls().usbPermission)
    {
        Log::warn ("Safe gain was asked for, but Headroom may not control the Wave:3 yet");
        return;
    }

    state.setSetting (ids::safeGain, on ? 1 : 0);       // the listener does the rest (safeGainSwitched)
}

void AppController::safeGainSwitched (bool on)
{
    if (on)
    {
        // Remember where the mic was, so that switching off puts it back exactly there: what it says over USB, else the
        // kernel's copy, else its maximum.
        const auto hardware = backend->getHardwareControls();
        const auto status = backend->getStatus();
        std::optional<double> measured;

        if (hardware.gainDb.has_value())
            measured = (double) *hardware.gainDb;
        else if (status.hardwareMicGain >= 0.0f)
            measured = wave::sliderToHardwareDb ((double) status.hardwareMicGain);

        const double baseline = std::clamp (std::round (measured.value_or (wave::kHardwareMaxDb) * 2.0) / 2.0, 0.0, wave::kHardwareMaxDb);

        safeGainWrites = 0;
        safeGainRestorePending = false;
        permissionMissing = false;
        Log::info ("Safe gain on: the mic was at " + plainDb (baseline) + " dB"
                   + (measured.has_value() ? juce::String() : juce::String (" (assumed, it has not been read yet)")));

        state.setSetting (ids::safeGainBase, baseline);
    }
    else if (safeGainAutoOff)
    {
        // The permission is gone: makeup 0, and the mic's own gain stays where it is (nothing can be confirmed any more).
        safeGainAutoOff = false;
        safeGainRestorePending = false;
        Log::warn ("Safe gain switched itself off: Headroom may no longer control the Wave:3");
    }
    else
    {
        safeGainRestorePending = true;      // updateSafeGain() puts the gain back as soon as the Wave:3 is the input
        Log::info ("Safe gain off");
    }

    updateSafeGain();
}

void AppController::applyMakeup (float db)
{
    if (std::abs (db - appliedMakeupDb) < 0.05f)
        return;

    appliedMakeupDb = db;
    engine->setChannelInputTrimDb (kMicChannel, db);
    Log::info ("Mic makeup gain: " + plainDb ((double) db) + " dB");
}

// Runs whenever the backend says something changed, once a second while Safe gain is waiting, and when the switch or its
// baseline changes. Decides three things: what the engine adds to the mic (makeup), whether the mic has to be told to go
// lower (or back), and what the settings page says.
//
// The makeup follows only what the mic itself reports over USB (which needs the permission): a mic that keeps its high gain,
// or has not been read, gets none, and so does one that is unplugged or is not the Wave:3. The kernel's copy of the gain is
// never enough, because a mic that ignores the request (gain lock) would still show the request there.
void AppController::updateSafeGain()
{
    JUCE_ASSERT_MESSAGE_THREAD

    const bool on = asBool (state.getSetting (ids::safeGain));
    const auto hardware = backend->getHardwareControls();
    const auto status = backend->getStatus();
    const std::optional<double> confirmed = hardware.gainDb.has_value() ? std::optional<double> ((double) *hardware.gainDb) : std::nullopt;
    const double baseline = std::clamp (asDouble (state.getSetting (ids::safeGainBase), wave::kHardwareMaxDb), 0.0, wave::kHardwareMaxDb);
    const juce::uint32 now = juce::Time::getMillisecondCounter();

    SafeGainInfo info;
    info.available = hardware.usbAvailable;
    info.on = on;
    float makeup = 0.0f;

    if (! confirmed.has_value())
        safeGainWrites = 0;                 // nothing to talk to: a later mic gets a fresh start

    // The permission is gone (the rule was removed): Safe gain cannot check anything any more. Give a replugged mic a moment
    // to get its access back, then switch off, once, and say so.
    if (on && hardware.usbAvailable && ! hardware.usbPermission)
    {
        if (! permissionMissing)
        {
            permissionMissing = true;
            permissionMissingSinceMs = now;
        }

        if (now - permissionMissingSinceMs >= (juce::uint32) options.safeGainPermissionGraceMs)
        {
            permissionMissing = false;
            safeGainAutoOff = true;
            state.setSetting (ids::safeGain, 0);        // the listener runs safeGainSwitched (which calls this again) and we are done here

            Notice notice;
            notice.id = "safe-gain-permission";
            notice.severity = Notice::Severity::warning;
            notice.text = "Safe gain was switched off because Headroom can no longer control the Wave:3. The mic's own gain was left as it is.";
            postNotice (std::move (notice));
            return;
        }
    }
    else
    {
        permissionMissing = false;
    }

    if (on && hardware.usbAvailable && hardware.usbPermission)
    {
        const auto plan = wave::planSafeGain (baseline, confirmed);
        makeup = (float) plan.makeupDb;

        info.hardwareKnown = confirmed.has_value();
        info.hardwareDb = (float) confirmed.value_or (plan.targetDb);
        info.makeupDb = makeup;

        if (! plan.hardwareTooHigh)
        {
            safeGainWrites = 0;
        }
        else if (safeGainWrites >= kMaxSafeGainWrites)
        {
            info.problem = hardware.gainLocked == std::optional<bool> (true)
                ? "The Wave:3 is set to ignore gain changes from the computer, so Safe gain cannot lower it."
                : "The Wave:3 did not take the lower gain, so Safe gain is not adding anything.";
        }
        else if (safeGainWrites == 0 || now - lastSafeGainWriteMs >= (juce::uint32) options.safeGainRetryMs)
        {
            Log::info ("Safe gain: the mic is at " + plainDb (*confirmed) + " dB, asking for " + plainDb (plan.targetDb)
                       + " dB (slider " + juce::String (wave::hardwareDbToSlider (plan.targetDb), 3) + ")");

            backend->setHardwareMicGain ((float) wave::hardwareDbToSlider (plan.targetDb));
            ++safeGainWrites;
            lastSafeGainWriteMs = now;

            if (safeGainWrites == kMaxSafeGainWrites)
                Log::warn ("Safe gain: this was the last try. The mic says " + plainDb (*confirmed) + " dB over USB, the kernel says "
                           + (status.hardwareMicGain >= 0.0f ? plainDb (wave::sliderToHardwareDb ((double) status.hardwareMicGain)) + " dB" : juce::String ("nothing")));
        }
    }
    else if (! on && safeGainRestorePending && info.available)
    {
        // Switched off: the mic's own gain goes back to where it was, unless it is there already.
        if (! confirmed.has_value() || *confirmed < baseline - wave::kGainToleranceDb)
        {
            Log::info ("Safe gain off: putting the mic back to " + plainDb (baseline) + " dB");
            backend->setHardwareMicGain ((float) wave::hardwareDbToSlider (baseline));
        }

        safeGainRestorePending = false;
    }

    applyMakeup (makeup);
    safeGainInfo = info;
}

void AppController::routeApp (uint32_t nodeId, const juce::String& channelId)
{
    backend->routeApp (nodeId, channelId);

    for (const auto& stream : backend->getAppStreams())
    {
        if (stream.nodeId != nodeId)
            continue;

        const auto pattern = (stream.binary.isNotEmpty() ? stream.binary : stream.appName).toLowerCase();
        state.setRule (pattern, channelId);     // listener pushes the updated rules to the backend
        break;
    }
}

void AppController::removeRule (const juce::String& pattern)
{
    state.removeRule (pattern);
}

// ---- Presets --------------------------------------------------------------------------------------------------

void AppController::applyMicPreset (const juce::String& presetId)
{
    const auto* preset = findMicPreset (presetId);
    auto mic = state.getMicChannel();

    if (preset == nullptr || ! mic.isValid())
        return;

    const juce::ScopedValueSetter<bool> presetGuard (applyingPreset, true);     // our own edits don't make it "custom"
    juce::StringArray oldUids;
    auto fx = AppState::getFx (mic);

    for (int i = 0; i < fx.getNumChildren(); ++i)
        oldUids.add (fx.getChild (i)[ids::uid].toString());

    for (auto& uid : oldUids)
        removeFx (values::micChannelId, uid);

    for (auto& presetSlot : preset->slots)
    {
        const auto uid = addBuiltinFx (values::micChannelId, presetSlot.builtinId);

        if (auto* builtin = dynamic_cast<BuiltinProcessor*> (getFxProcessor (uid)))
        {
            for (auto& [paramId, value] : presetSlot.params)
                if (auto* param = builtin->apvts.getParameter (paramId))
                    param->setValueNotifyingHost (param->convertTo0to1 (value));
        }
    }

    state.setSetting (ids::micPreset, presetId);
    captureFxStates (true);
}

// ---- Notices --------------------------------------------------------------------------------------------------

void AppController::postNotice (Notice notice)
{
    auto existing = std::find_if (notices.begin(), notices.end(), [&] (const Notice& n) { return n.id == notice.id; });

    if (existing != notices.end())
        *existing = std::move (notice);
    else
        notices.push_back (std::move (notice));

    if (onNoticesChanged)
        onNoticesChanged();
}

void AppController::dismissNotice (const juce::String& noticeId)
{
    const auto sizeBefore = notices.size();
    notices.erase (std::remove_if (notices.begin(), notices.end(), [&] (const Notice& n) { return n.id == noticeId; }),
                   notices.end());

    if (notices.size() != sizeBefore && onNoticesChanged)
        onNoticesChanged();
}

void AppController::runNoticeAction (const juce::String& noticeId)
{
    auto it = std::find_if (notices.begin(), notices.end(), [&] (const Notice& n) { return n.id == noticeId; });

    if (it == notices.end())
        return;

    auto action = it->action;
    dismissNotice (noticeId);

    if (action)
        action();
}
}
