// AppState: schema defaults, repair of loaded data, model helpers and atomic autosave.
#include "app/AppState.h"

#include "app/Log.h"
#include "engine/builtin/BuiltinFx.h"

#include <algorithm>

namespace hr
{
namespace
{
    constexpr int kAutosaveDelayMs = 1000;

    float asFloat (const juce::var& v, float fallback = 0.0f)   { return v.isVoid() ? fallback : (float) (double) v; }
    int asInt (const juce::var& v, int fallback = 0)            { return v.isVoid() ? fallback : (int) v; }
    bool asBool (const juce::var& v, bool fallback = false)     { return v.isVoid() ? fallback : (int) v != 0; }

    // Sets a property only if it is missing (used when repairing a loaded model).
    void ensureProperty (juce::ValueTree& tree, const juce::Identifier& property, const juce::var& value)
    {
        if (! tree.hasProperty (property))
            tree.setProperty (property, value, nullptr);
    }

    juce::ValueTree makeSettingsDefaults()
    {
        juce::ValueTree s (ids::SETTINGS);
        s.setProperty (ids::inputDevice, "auto", nullptr);
        s.setProperty (ids::outputDevice, "auto", nullptr);
        s.setProperty (ids::latency, 256, nullptr);
        s.setProperty (ids::makeDefault, 1, nullptr);
        s.setProperty (ids::keepDefault, 1, nullptr);          // Wave Link style lock; a state.xml without it means on
        s.setProperty (ids::startAtLogin, 1, nullptr);
        s.setProperty (ids::uiScale, 0.0, nullptr);            // 0 = automatic (derived from the desktop scale)
        s.setProperty (ids::restoreMicGain, 0, nullptr);
        s.setProperty (ids::restoreMicGainChosen, 0, nullptr); // hidden: 1 once "Restore mic gain" was decided, by the user's switch or by their first use of the gain slider (which turns it on once)
        s.setProperty (ids::micGain, 0.5, nullptr);
        s.setProperty (ids::headphoneVolume, 0.5, nullptr);
        s.setProperty (ids::firstRunDone, 0, nullptr);
        s.setProperty (ids::fallbackChannel, "system", nullptr);
        s.setProperty (ids::pluginFolders, "", nullptr);
        s.setProperty (ids::micPreset, "clean", nullptr);      // the default mic chain is the "clean" preset; "custom" once edited
        s.setProperty (ids::clipGuard, -1, nullptr);           // Wave:3 ClipGuard as the user last set it: -1 = never (leave the mic alone), 0 = off, 1 = on
        s.setProperty (ids::restoreClipGuard, 1, nullptr);     // put it back at start-up (the mic forgets its settings when it loses power)
        s.setProperty (ids::safeGain, 0, nullptr);             // Safe gain: lower hardware gain, Headroom makes up the rest
        s.setProperty (ids::safeGainBase, 40.0, nullptr);      // the hardware gain (dB) before Safe gain was switched on, put back when it is switched off
        s.setProperty (ids::experimentalHardware, 0, nullptr); // opt-in for the USB controls of models whose protocol is documented but that Headroom has not tried (off)
        return s;
    }

    juce::ValueTree makeMix (const juce::String& mixId, bool withOutputDevice)
    {
        juce::ValueTree m (ids::MIX);
        m.setProperty (ids::id, mixId, nullptr);
        m.setProperty (ids::gainDb, 0.0, nullptr);
        m.setProperty (ids::muted, 0, nullptr);
        m.setProperty (ids::limiter, 1, nullptr);

        // The Headphones mix plays through the automatic device until told otherwise; the Stream mix feeds "Headroom Stream Mix" and
        // plays to no other device (its list names the extra ones). outputDevice is the first of the Headphones list, kept for old readers.
        if (withOutputDevice)
        {
            m.setProperty (ids::outputDevice, "auto", nullptr);
            m.setProperty (ids::outputs, "auto", nullptr);
        }
        else
        {
            m.setProperty (ids::outputs, "", nullptr);
        }

        return m;
    }

    juce::ValueTree makeRule (const juce::String& pattern, const juce::String& channelId)
    {
        juce::ValueTree r (ids::RULE);
        r.setProperty (ids::pattern, pattern, nullptr);
        r.setProperty (ids::channel, channelId, nullptr);
        return r;
    }

    juce::String slugify (const juce::String& text)
    {
        juce::String slug;
        bool lastWasDash = true;    // swallows leading separators

        for (auto c : text.toLowerCase())
        {
            if (juce::CharacterFunctions::isLetterOrDigit (c) && c < 128)
            {
                slug << juce::String::charToString (c);
                lastWasDash = false;
            }
            else if (! lastWasDash)
            {
                slug << "-";
                lastWasDash = true;
            }
        }

        return slug.trimCharactersAtEnd ("-");
    }
}

const std::vector<juce::String>& getChannelColourPalette()
{
    static const std::vector<juce::String> palette { "E9A23B", "3FB8AF", "5B9BE6", "9D7BE0",
                                                     "E0708F", "9BC94B", "C9A77C", "8C95A3" };
    return palette;
}

juce::String AppState::mixId (Mix mix)
{
    return mix == Mix::Monitor ? values::mixMonitor : values::mixStream;
}

// ---------------------------------------------------------------------------------------------------
// Construction / defaults
// ---------------------------------------------------------------------------------------------------

AppState::AppState (const juce::File& stateFile) : file (stateFile)
{
    root.copyPropertiesAndChildrenFrom (createDefaults(), nullptr);
    root.addListener (this);
}

AppState::~AppState()
{
    stopTimer();
    root.removeListener (this);

    if (dirty && file != juce::File())
        saveNow();
}

juce::ValueTree AppState::makeChannel (const juce::String& id, const juce::String& kindValue, const juce::String& channelName,
                                       const juce::String& colourHex, const juce::String& iconName, int slotIndex,
                                       float monitorDbValue, bool monitorMutedValue)
{
    juce::ValueTree c (ids::CHANNEL);
    c.setProperty (ids::id, id, nullptr);
    c.setProperty (ids::kind, kindValue, nullptr);
    c.setProperty (ids::name, channelName, nullptr);
    c.setProperty (ids::colour, colourHex, nullptr);
    c.setProperty (ids::icon, iconName, nullptr);
    c.setProperty (ids::slot, slotIndex, nullptr);
    c.setProperty (ids::monitorDb, monitorDbValue, nullptr);
    c.setProperty (ids::monitorMuted, monitorMutedValue ? 1 : 0, nullptr);
    c.setProperty (ids::streamDb, 0.0, nullptr);
    c.setProperty (ids::streamMuted, 0, nullptr);
    c.setProperty (ids::linked, 0, nullptr);
    c.addChild (juce::ValueTree (ids::FX), -1, nullptr);
    return c;
}

juce::ValueTree AppState::makeBuiltinSlot (const juce::String& builtinId)
{
    juce::ValueTree s (ids::SLOT);
    s.setProperty (ids::uid, makeUid(), nullptr);
    s.setProperty (ids::crashId, (int) nextCrashId(), nullptr);
    s.setProperty (ids::type, values::slotBuiltin, nullptr);
    s.setProperty (ids::builtinId, builtinId, nullptr);
    s.setProperty (ids::name, BuiltinFx::displayNameFor (builtinId), nullptr);
    s.setProperty (ids::bypassed, 0, nullptr);
    s.setProperty (ids::disabled, 0, nullptr);
    s.setProperty (ids::state, "", nullptr);     // empty = the processor's own defaults, which are the preset
    return s;
}

juce::ValueTree AppState::createDefaults()
{
    crashIdCounter = 0;

    juce::ValueTree tree (ids::HEADROOM);
    tree.setProperty (ids::version, kStateVersion, nullptr);
    tree.addChild (makeSettingsDefaults(), -1, nullptr);

    juce::ValueTree mixes (ids::MIXES);
    mixes.addChild (makeMix (values::mixMonitor, true), -1, nullptr);
    mixes.addChild (makeMix (values::mixStream, false), -1, nullptr);
    tree.addChild (mixes, -1, nullptr);

    juce::ValueTree channels (ids::CHANNELS);

    // Mic: the Wave:3 has zero-latency hardware monitoring, so the processed mic is not sent to the headphones.
    auto mic = makeChannel (values::micChannelId, values::kindMic, "Mic", "E9A23B", "mic", -1, kMinusInfDb, false);

    // Default mic chain. Slots have empty state: the built-ins' parameter defaults are this preset.
    auto micFx = mic.getChildWithName (ids::FX);

    // The integration test needs the mic to pass through untouched (HEADROOM_TEST_NO_FX=1 = no mic FX on first run).
    const bool testNoFx = juce::SystemStats::getEnvironmentVariable ("HEADROOM_TEST_NO_FX", {}) == "1";

    if (! testNoFx)
        for (const char* builtinId : { "noise", "eq", "comp", "limit" })
            micFx.addChild (makeBuiltinSlot (builtinId), -1, nullptr);

    channels.addChild (mic, -1, nullptr);
    channels.addChild (makeChannel ("system", values::kindApp, "System", "8C95A3", "monitor", 0, 0.0f, false), -1, nullptr);
    channels.addChild (makeChannel ("game",   values::kindApp, "Game",   "5B9BE6", "gamepad", 1, 0.0f, false), -1, nullptr);
    channels.addChild (makeChannel ("music",  values::kindApp, "Music",  "3FB8AF", "music",   2, 0.0f, false), -1, nullptr);
    channels.addChild (makeChannel ("chat",   values::kindApp, "Chat",   "E0708F", "message", 3, 0.0f, false), -1, nullptr);
    tree.addChild (channels, -1, nullptr);

    juce::ValueTree routing (ids::ROUTING);

    for (const char* pattern : { "discord", "vesktop", "webcord", "webrtc voiceengine" })
        routing.addChild (makeRule (pattern, "chat"), -1, nullptr);

    routing.addChild (makeRule ("spotify", "music"), -1, nullptr);

    for (const char* pattern : { "*.exe", "*wine*", "*proton*", "steam_app_*" })
        routing.addChild (makeRule (pattern, "game"), -1, nullptr);

    tree.addChild (routing, -1, nullptr);
    return tree;
}

// ---------------------------------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------------------------------

void AppState::markDirty()
{
    if (suppressDirty)
        return;

    dirty = true;

    if (file != juce::File() && juce::MessageManager::getInstanceWithoutCreating() != nullptr)
        startTimer (kAutosaveDelayMs);      // restarting the timer debounces bursts of changes
}

void AppState::timerCallback()
{
    stopTimer();
    saveNow();
}

void AppState::setBeforeSave (std::function<void()> callback)
{
    beforeSave = std::move (callback);
}

bool AppState::saveNow()
{
    JUCE_ASSERT_MESSAGE_THREAD

    stopTimer();

    if (file == juce::File())
    {
        dirty = false;
        return true;
    }

    if (beforeSave)
    {
        const juce::ScopedValueSetter<bool> guard (suppressDirty, true);
        beforeSave();
    }

    auto xml = root.createXml();

    if (xml == nullptr)
        return false;

    file.getParentDirectory().createDirectory();

    juce::TemporaryFile temp (file);

    if (! xml->writeTo (temp.getFile()) || ! temp.overwriteTargetFileWithTemporary())
    {
        Log::error ("Could not save " + file.getFullPathName());
        return false;
    }

    dirty = false;
    return true;
}

AppState::LoadResult AppState::load()
{
    JUCE_ASSERT_MESSAGE_THREAD

    auto apply = [this] (const juce::ValueTree& tree)
    {
        const juce::ScopedValueSetter<bool> guard (suppressDirty, true);
        root.copyPropertiesAndChildrenFrom (tree, nullptr);
    };

    auto backUp = [this]
    {
        const auto backup = file.withFileExtension (file.getFileExtension() + ".bak");
        file.copyFileTo (backup);
        Log::warn ("Saved settings could not be used; a copy was kept as " + backup.getFullPathName());
    };

    if (file == juce::File() || ! file.existsAsFile())
    {
        apply (createDefaults());
        markDirty();        // persist the defaults early so slot uids / crash ids survive a restart
        return LoadResult::CreatedDefaults;
    }

    std::unique_ptr<juce::XmlElement> xml = juce::XmlDocument::parse (file);
    juce::ValueTree loaded;

    if (xml != nullptr && xml->hasTagName (ids::HEADROOM.toString()))
        loaded = juce::ValueTree::fromXml (*xml);

    if (! loaded.isValid())
    {
        backUp();
        apply (createDefaults());
        markDirty();
        return LoadResult::RecoveredFromCorrupt;
    }

    if (asInt (loaded.getProperty (ids::version), 0) > kStateVersion)
    {
        backUp();
        apply (createDefaults());
        markDirty();
        return LoadResult::UnsupportedVersion;
    }

    repair (loaded);
    apply (loaded);
    refreshCrashCounter();

    dirty = false;
    return LoadResult::Loaded;
}

// Loaded data is user-editable: make sure everything the rest of the app relies on exists.
void AppState::repair (juce::ValueTree& tree)
{
    tree.setProperty (ids::version, kStateVersion, nullptr);

    auto ensureChild = [&tree] (const juce::Identifier& type)
    {
        auto child = tree.getChildWithName (type);

        if (! child.isValid())
        {
            child = juce::ValueTree (type);
            tree.addChild (child, -1, nullptr);
        }

        return child;
    };

    // Settings
    auto settings = ensureChild (ids::SETTINGS);
    auto defaults = makeSettingsDefaults();

    for (int i = 0; i < defaults.getNumProperties(); ++i)
    {
        const auto name = defaults.getPropertyName (i);
        ensureProperty (settings, name, defaults.getProperty (name));
    }

    // Mixes
    auto mixes = ensureChild (ids::MIXES);

    for (auto mixIdValue : { values::mixMonitor, values::mixStream })
    {
        auto mix = mixes.getChildWithProperty (ids::id, mixIdValue);

        if (! mix.isValid())
        {
            mix = makeMix (mixIdValue, mixIdValue == values::mixMonitor);
            mixes.addChild (mix, -1, nullptr);
        }

        ensureProperty (mix, ids::gainDb, 0.0);
        ensureProperty (mix, ids::muted, 0);
        ensureProperty (mix, ids::limiter, 1);

        const bool isMonitor = mixIdValue == values::mixMonitor;

        if (isMonitor)
            ensureProperty (mix, ids::outputDevice, "auto");

        // Outputs: a file from before the list has one device, which becomes a list of one (SETTINGS says which: it is what the
        // backend was driven from). The Headphones list is never empty; the Stream list never holds "auto". The old property
        // follows the first entry.
        juce::String legacy;

        if (! mix.hasProperty (ids::outputs) && isMonitor)
            legacy = settings[ids::outputDevice].toString().isNotEmpty() ? settings[ids::outputDevice].toString() : mix[ids::outputDevice].toString();

        auto list = splitOutputs (mix.hasProperty (ids::outputs) ? mix[ids::outputs].toString() : legacy);

        if (isMonitor && list.isEmpty())
            list.add (values::outputAuto);
        else if (! isMonitor)
            list.removeString (values::outputAuto);

        mix.setProperty (ids::outputs, joinOutputs (list), nullptr);

        if (isMonitor)
        {
            mix.setProperty (ids::outputDevice, list[0], nullptr);
            settings.setProperty (ids::outputDevice, list[0], nullptr);
        }
    }

    // Channels
    auto channels = ensureChild (ids::CHANNELS);

    auto mic = channels.getChildWithProperty (ids::id, values::micChannelId);

    if (! mic.isValid())
    {
        mic = makeChannel (values::micChannelId, values::kindMic, "Mic", "E9A23B", "mic", -1, kMinusInfDb, false);
        channels.addChild (mic, 0, nullptr);
    }
    else if (channels.indexOf (mic) != 0)
    {
        channels.moveChild (channels.indexOf (mic), 0, nullptr);
    }

    juce::StringArray seenChannelIds;
    uint32_t usedSlots = 0, usedInputSlots = 0;
    std::vector<juce::ValueTree> toDrop;

    for (auto channel : channels)
    {
        auto channelId = channel[ids::id].toString();

        if (channelId.isEmpty() || seenChannelIds.contains (channelId))
        {
            toDrop.push_back (channel);
            continue;
        }

        seenChannelIds.add (channelId);

        const bool isMic = channelId == values::micChannelId;
        const bool isInput = ! isMic && channel[ids::kind].toString() == values::kindInput;
        channel.setProperty (ids::kind, isMic ? values::kindMic : (isInput ? values::kindInput : values::kindApp), nullptr);

        ensureProperty (channel, ids::name, isMic ? "Mic" : channelId);
        ensureProperty (channel, ids::colour, isMic ? "E9A23B" : "8C95A3");
        ensureProperty (channel, ids::icon, isMic || isInput ? "mic" : "speaker");
        ensureProperty (channel, ids::monitorDb, isMic ? (double) kMinusInfDb : 0.0);
        ensureProperty (channel, ids::monitorMuted, 0);
        ensureProperty (channel, ids::streamDb, 0.0);
        ensureProperty (channel, ids::streamMuted, 0);
        ensureProperty (channel, ids::linked, 0);

        if (isMic)
        {
            channel.setProperty (ids::slot, -1, nullptr);
        }
        else
        {
            // App channels and input channels have a numbering of their own: 0-15 and 0-7.
            const int slotCount = isInput ? kMaxInputChannels : kMaxAppChannels;
            auto& used = isInput ? usedInputSlots : usedSlots;
            int slotIndex = asInt (channel.getProperty (ids::slot), -1);

            if (! juce::isPositiveAndBelow (slotIndex, slotCount) || (used & (1u << slotIndex)) != 0)
            {
                slotIndex = -1;

                for (int s = 0; s < slotCount; ++s)
                    if ((used & (1u << s)) == 0) { slotIndex = s; break; }
            }

            if (slotIndex < 0)
            {
                toDrop.push_back (channel);     // more channels than slots: only possible with a hand-edited file
                continue;
            }

            used |= 1u << slotIndex;
            channel.setProperty (ids::slot, slotIndex, nullptr);
        }

        if (isInput)
        {
            ensureProperty (channel, ids::device, "");      // an input channel without a device just says "not connected"
            channel.setProperty (ids::channels, channel[ids::channels].toString() == values::inputStereo ? values::inputStereo : values::inputMono, nullptr);
        }

        if (! channel.getChildWithName (ids::FX).isValid())
            channel.addChild (juce::ValueTree (ids::FX), -1, nullptr);
    }

    for (auto& channel : toDrop)
        channels.removeChild (channel, nullptr);

    // FX slots: every slot needs a unique uid and a unique non-zero crashId.
    juce::StringArray seenUids;
    std::vector<uint32_t> seenCrashIds;
    uint32_t maxCrashId = 0;

    for (auto channel : channels)
        for (auto slotTree : channel.getChildWithName (ids::FX))
            maxCrashId = juce::jmax (maxCrashId, (uint32_t) asInt (slotTree.getProperty (ids::crashId), 0));

    for (auto channel : channels)
    {
        auto fx = channel.getChildWithName (ids::FX);

        for (int i = fx.getNumChildren(); --i >= 0;)
        {
            auto slotTree = fx.getChild (i);
            auto slotUid = slotTree[ids::uid].toString();
            auto slotType = slotTree[ids::type].toString();
            const bool typeOk = slotType == values::slotBuiltin || slotType == values::slotPlugin;

            if (! typeOk)
            {
                fx.removeChild (i, nullptr);
                continue;
            }

            while (slotUid.isEmpty() || seenUids.contains (slotUid))
                slotUid = juce::Uuid().toString().substring (0, 4);

            seenUids.add (slotUid);
            slotTree.setProperty (ids::uid, slotUid, nullptr);

            auto crash = (uint32_t) asInt (slotTree.getProperty (ids::crashId), 0);

            if (crash == 0 || std::find (seenCrashIds.begin(), seenCrashIds.end(), crash) != seenCrashIds.end())
                crash = ++maxCrashId;

            seenCrashIds.push_back (crash);
            slotTree.setProperty (ids::crashId, (int) crash, nullptr);

            ensureProperty (slotTree, ids::name, slotTree[ids::builtinId].toString());
            ensureProperty (slotTree, ids::bypassed, 0);
            ensureProperty (slotTree, ids::disabled, 0);
            ensureProperty (slotTree, ids::state, "");
        }
    }

    ensureChild (ids::ROUTING);

    // The fallback channel must exist.
    if (! channels.getChildWithProperty (ids::id, settings[ids::fallbackChannel].toString()).isValid())
    {
        juce::String replacement;

        for (auto channel : channels)
            if (channel[ids::kind].toString() == values::kindApp) { replacement = channel[ids::id].toString(); break; }

        settings.setProperty (ids::fallbackChannel, replacement, nullptr);
    }
}

void AppState::refreshCrashCounter()
{
    uint32_t maxId = 0;

    for (auto channel : getChannels())
        for (auto slotTree : channel.getChildWithName (ids::FX))
            maxId = juce::jmax (maxId, (uint32_t) asInt (slotTree.getProperty (ids::crashId), 0));

    crashIdCounter = maxId;
}

// ---------------------------------------------------------------------------------------------------
// Lookup helpers
// ---------------------------------------------------------------------------------------------------

juce::ValueTree AppState::getMix (Mix mix) const
{
    return getMixes().getChildWithProperty (ids::id, mixId (mix));
}

juce::ValueTree AppState::findChannel (const juce::String& channelId) const
{
    return getChannels().getChildWithProperty (ids::id, channelId);
}

juce::ValueTree AppState::findSlot (const juce::String& slotUid) const
{
    for (auto channel : getChannels())
    {
        auto found = channel.getChildWithName (ids::FX).getChildWithProperty (ids::uid, slotUid);

        if (found.isValid())
            return found;
    }

    return {};
}

juce::ValueTree AppState::getFx (const juce::ValueTree& channel)
{
    return channel.getChildWithName (ids::FX);
}

juce::String AppState::getChannelIdOfSlot (const juce::ValueTree& slotTree)
{
    return slotTree.getParent().getParent()[ids::id].toString();
}

ChannelInfo AppState::describe (const juce::ValueTree& channel)
{
    ChannelInfo info;
    info.id = channel[ids::id].toString();
    info.kind = channel[ids::kind].toString();
    info.name = channel[ids::name].toString();
    info.colour = channel[ids::colour].toString();
    info.icon = channel[ids::icon].toString();
    info.slot = asInt (channel[ids::slot], -1);
    info.device = channel[ids::device].toString();
    info.stereo = channel[ids::channels].toString() == values::inputStereo;
    info.monitorDb = asFloat (channel[ids::monitorDb]);
    info.streamDb = asFloat (channel[ids::streamDb]);
    info.monitorMuted = asBool (channel[ids::monitorMuted]);
    info.streamMuted = asBool (channel[ids::streamMuted]);
    info.linked = asBool (channel[ids::linked]);
    return info;
}

SlotInfo AppState::describeSlot (const juce::ValueTree& slotTree)
{
    SlotInfo info;
    info.uid = slotTree[ids::uid].toString();
    info.type = slotTree[ids::type].toString();
    info.builtinId = slotTree[ids::builtinId].toString();
    info.name = slotTree[ids::name].toString();
    info.descXml = slotTree[ids::desc].toString();
    info.stateBase64 = slotTree[ids::state].toString();
    info.crashId = (uint32_t) asInt (slotTree[ids::crashId]);
    info.bypassed = asBool (slotTree[ids::bypassed]);
    info.disabled = asBool (slotTree[ids::disabled]);
    return info;
}

std::vector<ChannelInfo> AppState::getChannelInfos() const
{
    std::vector<ChannelInfo> result;

    for (auto channel : getChannels())
        result.push_back (describe (channel));

    return result;
}

std::vector<ChannelEndpoint> AppState::getChannelEndpoints() const
{
    std::vector<ChannelEndpoint> result;

    for (auto channel : getChannels())
    {
        const auto info = describe (channel);

        if (info.isApp())
            result.push_back ({ info.id, info.name, info.slot });
    }

    return result;
}

std::vector<InputEndpoint> AppState::getInputEndpoints() const
{
    std::vector<InputEndpoint> result;

    for (auto channel : getChannels())
    {
        const auto info = describe (channel);

        if (info.isInput())
            result.push_back ({ info.id, info.name, info.device, info.slot, info.stereo });
    }

    return result;
}

std::vector<RoutingRule> AppState::getRoutingRules() const
{
    std::vector<RoutingRule> result;

    for (auto rule : getRouting())
        result.push_back ({ rule[ids::pattern].toString(), rule[ids::channel].toString() });

    return result;
}

// ---------------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------------

juce::var AppState::getSetting (const juce::Identifier& property) const
{
    return getSettings().getProperty (property);
}

void AppState::setSetting (const juce::Identifier& property, const juce::var& value)
{
    JUCE_ASSERT_MESSAGE_THREAD
    auto settings = getSettings();
    settings.setProperty (property, value, nullptr);
}

juce::StringArray AppState::getPluginFolders() const
{
    juce::StringArray folders;
    folders.addTokens (getSetting (ids::pluginFolders).toString(), "\n", "");
    folders.trim();
    folders.removeEmptyStrings();
    return folders;
}

// ---------------------------------------------------------------------------------------------------
// Channels
// ---------------------------------------------------------------------------------------------------

int AppState::nextFreeSlot() const
{
    uint32_t used = 0;

    for (auto channel : getChannels())
    {
        if (channel[ids::kind].toString() == values::kindInput)
            continue;       // the input channels count their slots on their own

        const auto s = asInt (channel[ids::slot], -1);

        if (juce::isPositiveAndBelow (s, kMaxAppChannels))
            used |= 1u << s;
    }

    for (int s = 0; s < kMaxAppChannels; ++s)
        if ((used & (1u << s)) == 0)
            return s;

    return -1;
}

int AppState::nextFreeInputSlot() const
{
    uint32_t used = 0;

    for (auto channel : getChannels())
    {
        if (channel[ids::kind].toString() != values::kindInput)
            continue;

        const auto s = asInt (channel[ids::slot], -1);

        if (juce::isPositiveAndBelow (s, kMaxInputChannels))
            used |= 1u << s;
    }

    for (int s = 0; s < kMaxInputChannels; ++s)
        if ((used & (1u << s)) == 0)
            return s;

    return -1;
}

juce::String AppState::nextFreeColour() const
{
    const auto& palette = getChannelColourPalette();
    juce::StringArray used;

    for (auto channel : getChannels())
        used.add (channel[ids::colour].toString().toUpperCase());

    for (const auto& colour : palette)
        if (! used.contains (colour))
            return colour;

    return palette[(size_t) getChannels().getNumChildren() % palette.size()];
}

juce::String AppState::makeChannelId (const juce::String& channelName) const
{
    auto base = slugify (channelName);

    if (base.isEmpty())
        base = "channel";

    auto candidate = base;

    for (int n = 2; findChannel (candidate).isValid(); ++n)
        candidate = base + "-" + juce::String (n);

    return candidate;
}

juce::ValueTree AppState::addAppChannel (const juce::String& requestedName)
{
    JUCE_ASSERT_MESSAGE_THREAD

    const auto slotIndex = nextFreeSlot();

    if (slotIndex < 0)
        return {};

    auto channelName = requestedName.trim();

    if (channelName.isEmpty())
    {
        int appCount = 0;

        for (auto channel : getChannels())
            if (channel[ids::kind].toString() == values::kindApp)
                ++appCount;

        auto nameTaken = [this] (const juce::String& candidate)
        {
            for (auto channel : getChannels())
                if (channel[ids::name].toString() == candidate)
                    return true;

            return false;
        };

        int n = appCount + 1;

        while (nameTaken ("Channel " + juce::String (n)))
            ++n;

        channelName = "Channel " + juce::String (n);
    }

    auto channel = makeChannel (makeChannelId (channelName), values::kindApp, channelName, nextFreeColour(),
                                "speaker", slotIndex, 0.0f, false);

    auto channels = getChannels();
    channels.addChild (channel, -1, nullptr);
    return channel;
}

juce::ValueTree AppState::addInputChannel (const juce::String& deviceId, const juce::String& requestedName, bool stereo)
{
    JUCE_ASSERT_MESSAGE_THREAD

    const auto slotIndex = nextFreeInputSlot();

    if (slotIndex < 0 || deviceId.isEmpty())
        return {};

    auto channelName = requestedName.trim();

    if (channelName.isEmpty())
        channelName = "Input " + juce::String (slotIndex + 1);

    auto id = makeChannelId (channelName);

    if (id == values::micChannelId)
        id = makeChannelId ("input " + channelName);

    // Starts silent in the Headphones mix (not muted, like the main mic): a mic heard through speakers in the same room
    // would feed back. The Stream mix gets it at 0 dB; turning it up for a capture card is one drag.
    auto channel = makeChannel (id, values::kindInput, channelName, nextFreeColour(), "mic", slotIndex, kMinusInfDb, false);
    channel.setProperty (ids::device, deviceId, nullptr);
    channel.setProperty (ids::channels, stereo ? values::inputStereo : values::inputMono, nullptr);

    getChannels().addChild (channel, -1, nullptr);
    return channel;
}

bool AppState::removeChannel (const juce::String& channelId)
{
    JUCE_ASSERT_MESSAGE_THREAD

    auto channel = findChannel (channelId);

    if (! channel.isValid() || channel[ids::kind].toString() == values::kindMic)
        return false;

    auto routing = getRouting();

    for (int i = routing.getNumChildren(); --i >= 0;)
        if (routing.getChild (i)[ids::channel].toString() == channelId)
            routing.removeChild (i, nullptr);

    if (getFallbackChannel() == channelId)
    {
        juce::String replacement;

        for (auto other : getChannels())
            if (other != channel && other[ids::kind].toString() == values::kindApp)
            {
                replacement = other[ids::id].toString();
                break;
            }

        setSetting (ids::fallbackChannel, replacement);
    }

    getChannels().removeChild (channel, nullptr);
    return true;
}

// ---------------------------------------------------------------------------------------------------
// Outputs
// ---------------------------------------------------------------------------------------------------

juce::StringArray AppState::splitOutputs (const juce::String& joined)
{
    juce::StringArray list;
    list.addTokens (joined, "|", "");
    list.trim();
    list.removeEmptyStrings();
    list.removeDuplicates (false);
    return list;
}

juce::String AppState::joinOutputs (const juce::StringArray& devices)
{
    return devices.joinIntoString ("|");
}

juce::StringArray AppState::getOutputs (Mix mix) const
{
    const auto tree = getMix (mix);
    auto list = splitOutputs (tree[ids::outputs].toString());

    if (mix == Mix::Monitor)
    {
        if (list.isEmpty() && ! tree.hasProperty (ids::outputs))
            list = splitOutputs (getSetting (ids::outputDevice).toString());       // a model that never went through repair()

        if (list.isEmpty())
            list.add (values::outputAuto);
    }
    else
    {
        list.removeString (values::outputAuto);
    }

    return list;
}

void AppState::setOutputs (Mix mix, const juce::StringArray& devices)
{
    JUCE_ASSERT_MESSAGE_THREAD

    auto list = devices;
    list.trim();
    list.removeEmptyStrings();
    list.removeDuplicates (false);

    if (mix == Mix::Monitor && list.isEmpty())
        list.add (values::outputAuto);
    else if (mix == Mix::Stream)
        list.removeString (values::outputAuto);

    auto tree = getMix (mix);

    if (! tree.isValid())
        return;

    // The list first: whoever reacts to one of the other two changes already finds the new list.
    tree.setProperty (ids::outputs, joinOutputs (list), nullptr);

    if (mix == Mix::Monitor)
    {
        tree.setProperty (ids::outputDevice, list[0], nullptr);
        setSetting (ids::outputDevice, list[0]);
    }
}

// ---------------------------------------------------------------------------------------------------
// Effects
// ---------------------------------------------------------------------------------------------------

juce::String AppState::makeUid() const
{
    static const char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    auto& random = juce::Random::getSystemRandom();

    for (;;)
    {
        juce::String candidate;

        for (int i = 0; i < 4; ++i)
            candidate += juce::String::charToString ((juce::juce_wchar) alphabet[random.nextInt (36)]);

        if (! findSlot (candidate).isValid())
            return candidate;
    }
}

juce::ValueTree AppState::addBuiltinSlot (const juce::String& channelId, const juce::String& builtinId, int index)
{
    JUCE_ASSERT_MESSAGE_THREAD

    auto channel = findChannel (channelId);

    if (! channel.isValid())
        return {};

    auto fx = channel.getChildWithName (ids::FX);

    if (! fx.isValid())
    {
        fx = juce::ValueTree (ids::FX);
        channel.addChild (fx, -1, nullptr);
    }

    auto slotTree = makeBuiltinSlot (builtinId);
    fx.addChild (slotTree, index, nullptr);
    return slotTree;
}

juce::ValueTree AppState::addPluginSlot (const juce::String& channelId, const juce::PluginDescription& description, int index)
{
    JUCE_ASSERT_MESSAGE_THREAD

    auto channel = findChannel (channelId);

    if (! channel.isValid())
        return {};

    auto fx = channel.getChildWithName (ids::FX);

    if (! fx.isValid())
    {
        fx = juce::ValueTree (ids::FX);
        channel.addChild (fx, -1, nullptr);
    }

    juce::String descXml;

    if (auto xml = description.createXml())
        descXml = xml->toString (juce::XmlElement::TextFormat().singleLine().withoutHeader());

    juce::ValueTree slotTree (ids::SLOT);
    slotTree.setProperty (ids::uid, makeUid(), nullptr);
    slotTree.setProperty (ids::crashId, (int) nextCrashId(), nullptr);
    slotTree.setProperty (ids::type, values::slotPlugin, nullptr);
    slotTree.setProperty (ids::name, description.name, nullptr);
    slotTree.setProperty (ids::desc, descXml, nullptr);
    slotTree.setProperty (ids::bypassed, 0, nullptr);
    slotTree.setProperty (ids::disabled, 0, nullptr);
    slotTree.setProperty (ids::state, "", nullptr);

    fx.addChild (slotTree, index, nullptr);
    return slotTree;
}

bool AppState::removeSlot (const juce::String& slotUid)
{
    JUCE_ASSERT_MESSAGE_THREAD

    auto slotTree = findSlot (slotUid);

    if (! slotTree.isValid())
        return false;

    slotTree.getParent().removeChild (slotTree, nullptr);
    return true;
}

bool AppState::moveSlot (const juce::String& slotUid, int newIndex)
{
    JUCE_ASSERT_MESSAGE_THREAD

    auto slotTree = findSlot (slotUid);

    if (! slotTree.isValid())
        return false;

    auto fx = slotTree.getParent();
    fx.moveChild (fx.indexOf (slotTree), juce::jlimit (0, fx.getNumChildren() - 1, newIndex), nullptr);
    return true;
}

void AppState::setSlotState (const juce::String& slotUid, const juce::String& base64, bool markAsDirty)
{
    JUCE_ASSERT_MESSAGE_THREAD

    auto slotTree = findSlot (slotUid);

    if (! slotTree.isValid() || slotTree[ids::state].toString() == base64)
        return;

    const juce::ScopedValueSetter<bool> guard (suppressDirty, ! markAsDirty || suppressDirty);
    slotTree.setProperty (ids::state, base64, nullptr);
}

// ---------------------------------------------------------------------------------------------------
// Routing rules
// ---------------------------------------------------------------------------------------------------

void AppState::setRule (const juce::String& pattern, const juce::String& channelId)
{
    JUCE_ASSERT_MESSAGE_THREAD

    const auto key = pattern.trim().toLowerCase();

    if (key.isEmpty())
        return;

    auto routing = getRouting();
    auto existing = routing.getChildWithProperty (ids::pattern, key);

    if (existing.isValid())
        existing.setProperty (ids::channel, channelId, nullptr);
    else
        routing.addChild (makeRule (key, channelId), 0, nullptr);   // first match wins: a user's choice beats seeded wildcards
}

void AppState::removeRule (const juce::String& pattern)
{
    JUCE_ASSERT_MESSAGE_THREAD

    auto routing = getRouting();
    auto existing = routing.getChildWithProperty (ids::pattern, pattern.trim().toLowerCase());

    if (existing.isValid())
        routing.removeChild (existing, nullptr);
}
}
