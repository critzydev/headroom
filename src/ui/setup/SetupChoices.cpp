// SetupChoices implementation.
#include "ui/setup/SetupChoices.h"

#include "engine/builtin/BuiltinProcessor.h"
#include "engine/builtin/Presets.h"

namespace hr
{
namespace
{
    // The mic's Noise Removal effect, if it has one: its slot uid.
    juce::String noiseSlotUid (AppController& controller)
    {
        for (auto slotTree : AppState::getFx (controller.getState().getMicChannel()))
        {
            const auto slot = AppState::describeSlot (slotTree);

            if (slot.isBuiltin() && slot.builtinId == "noise" && ! slot.disabled)
                return slot.uid;
        }

        return {};
    }

    juce::RangedAudioParameter* modeParameter (AppController& controller)
    {
        const auto uid = noiseSlotUid (controller);

        if (uid.isEmpty())
            return nullptr;

        if (auto* processor = dynamic_cast<BuiltinProcessor*> (controller.getFxProcessor (uid)))
            return processor->apvts.getParameter ("mode");

        return nullptr;
    }
}

int SetupChoices::defaultNoiseModeFor (const juce::String& presetId)
{
    if (const auto* preset = findMicPreset (presetId))
        for (const auto& slot : preset->slots)
            if (slot.builtinId == "noise")
                for (const auto& [parameter, value] : slot.params)
                    if (parameter == "mode")
                        return juce::roundToInt (value);

    return 1;       // Best
}

int SetupChoices::noiseModeOf (AppController& controller, int fallback)
{
    if (auto* parameter = modeParameter (controller))
        return juce::roundToInt (parameter->convertFrom0to1 (parameter->getValue()));

    return fallback;
}

bool SetupChoices::setNoiseMode (AppController& controller, int mode)
{
    if (auto* parameter = modeParameter (controller))
    {
        if (noiseModeOf (controller, -1) != mode)
            parameter->setValueNotifyingHost (parameter->convertTo0to1 ((float) mode));

        return true;
    }

    return false;
}

SetupChoices SetupChoices::startingPoint (AppController& controller)
{
    auto& state = controller.getState();
    SetupChoices c;

    c.inputDevice = state.getInputDevice().isNotEmpty() ? state.getInputDevice() : juce::String ("auto");
    c.outputDevice = state.getOutputDevice().isNotEmpty() ? state.getOutputDevice() : juce::String ("auto");

    for (const auto& info : state.getChannelInfos())
    {
        if (! info.isApp())
            continue;           // the mic, and input channels (the wizard does not touch those)

        Channel channel;
        channel.id = info.id;
        channel.name = info.name;
        channel.colour = info.colour;
        c.channels.push_back (std::move (channel));
    }

    const auto preset = state.getSetting (ids::micPreset).toString();
    c.preset = preset.isNotEmpty() ? preset : juce::String ("clean");
    const int presetMode = defaultNoiseModeFor (c.preset == "custom" ? juce::String ("clean") : c.preset);
    c.noiseMode = noiseModeOf (controller, presetMode);
    c.noiseModeChosen = c.noiseMode != presetMode;            // a mode that is not what the preset gives was somebody's choice

    c.startAtLogin = (int) state.getSetting (ids::startAtLogin) != 0;
    c.makeDefault = state.getMakeDefault();
    c.keepDefault = state.getKeepDefault();

    const int clipGuard = state.getSetting (ids::clipGuard).isVoid() ? -1 : (int) state.getSetting (ids::clipGuard);
    c.clipGuard = clipGuard != 0;                   // never chosen (-1) counts as on: they are in the step to switch it on
    c.safeGain = (int) state.getSetting (ids::safeGain) != 0;
    return c;
}

void SetupChoices::apply (AppController& controller) const
{
    auto& state = controller.getState();

    controller.setInputDevice (inputDevice);

    // The devices were already put into the model as they were picked. A person who had more than one output and did not touch
    // the list here keeps them all: only a different choice replaces the list with one device.
    if (state.getOutputDevice() != outputDevice)
        controller.setOutputDevice (outputDevice);

    // Channels: remove, rename, add.
    for (const auto& channel : channels)
        if (! channel.isNew && ! channel.keep)
            controller.removeChannel (channel.id);

    for (const auto& channel : channels)
    {
        const auto name = channel.name.trim();

        if (! channel.keep)
            continue;

        if (channel.isNew)
        {
            const auto id = controller.addChannel();

            if (id.isNotEmpty() && name.isNotEmpty())
                controller.renameChannel (id, name);
        }
        else if (name.isNotEmpty() && name != AppState::describe (state.findChannel (channel.id)).name)
        {
            controller.renameChannel (channel.id, name);
        }
    }

    // Voice
    if (preset != "custom" && preset != state.getSetting (ids::micPreset).toString())
        controller.applyMicPreset (preset);

    if (preset != "off")
        setNoiseMode (controller, noiseMode);

    // System
    controller.setMakeDefault (makeDefault);
    controller.setKeepDefault (keepDefault);
    state.setSetting (ids::startAtLogin, startAtLogin ? 1 : 0);

    // Wave:3: only with the permission (the controller refuses Safe gain without it, and ClipGuard cannot be set without it).
    const auto hardware = controller.getHardwareControls();

    if (hardware.usbAvailable && hardware.usbPermission)
    {
        controller.setClipGuard (clipGuard);

        if (safeGain || (int) state.getSetting (ids::safeGain) != 0)
            controller.setSafeGain (safeGain);
    }

    state.setSetting (ids::firstRunDone, 1);        // this replaces the "You're set up" sheet
}
}
