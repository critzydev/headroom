// SettingsView implementation.
#include "ui/views/SettingsView.h"

#include "app/Paths.h"
#include "ui/HeadroomLookAndFeel.h"
#include "ui/Icons.h"
#include "ui/MicStatus.h"
#include "ui/Theme.h"
#include "ui/UiScale.h"
#include "ui/widgets/PlayThrough.h"

namespace hr
{
namespace
{
    constexpr int kMaxColumn = 640, kLabelWidth = 180, kRowHeight = 40;

    const juce::Identifier kUiScale = ids::uiScale;

    // "24" or "24.5": whole numbers stay whole.
    juce::String plainDb (float db)
    {
        return std::abs (db - std::round (db)) < 0.05f ? juce::String (juce::roundToInt (db)) : juce::String (db, 1);
    }

    struct LatencyChoice { int frames; const char* label; };
    constexpr LatencyChoice kLatencies[] = { { 128, "Low (2.7 ms)" }, { 256, "Balanced (5.3 ms)" }, { 512, "Safe (10.7 ms)" } };
    constexpr double kScales[] = { 1.0, 1.25, 1.5, 2.0 };       // the fixed choices after "Automatic"
}

class SettingsView::Content final : public juce::Component,
                                    private MeterClock::Listener
{
public:
    Content (AppController& c, MeterClock& meterClock, InstallManager* installer) : controller (c), clock (meterClock), installManager (installer)
    {
        auto setUpSlider = [this] (juce::Slider& s)
        {
            s.setSliderStyle (juce::Slider::LinearHorizontal);
            s.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
            s.setRange (0.0, 1.0, 0.01);
            s.setMouseClickGrabsKeyboardFocus (false);
            s.setScrollWheelEnabled (false);        // the wheel scrolls the page, it must not move a slider under the pointer
            s.setDoubleClickReturnValue (true, 0.5);
            addAndMakeVisible (s);
        };

        inputDropdown.setTooltip ("The microphone Headroom listens to");
        inputDropdown.onChange = [this] (const juce::String& key) { controller.setInputDevice (key); };
        addAndMakeVisible (inputDropdown);

        // The same "Play through" lists as the mixer's: every device a mix plays to, ticked in a small panel.
        outputDropdown.setTooltip ("Where your headphones mix plays. Click to pick more than one.");
        playthrough::attach (outputDropdown, controller, Mix::Monitor, true);
        addAndMakeVisible (outputDropdown);

        streamOutputDropdown.setPrefix ("Also plays to: ");
        streamOutputDropdown.setTooltip ("Other devices the Stream mix plays to, besides Headroom Stream Mix");
        playthrough::attach (streamOutputDropdown, controller, Mix::Stream);
        addAndMakeVisible (streamOutputDropdown);

        fixMic.getProperties().set (style::button, "primary");
        fixMic.setTooltip ("Restarts your mic if it went silent");
        fixMic.onClick = [this] { controller.fixMic(); };
        addAndMakeVisible (fixMic);

        setUpSlider (micGain);
        micGain.onValueChange = [this]
        {
            if (! syncing)
            {
                lastSliderUseMs = juce::Time::getMillisecondCounterHiRes();
                controller.userSetHardwareMicGain ((float) micGain.getValue());
            }

            repaint();
        };
        setUpSlider (headphoneVolume);
        headphoneVolume.onValueChange = [this]
        {
            if (! syncing)
            {
                lastSliderUseMs = juce::Time::getMillisecondCounterHiRes();
                controller.setHardwareHeadphoneVolume ((float) headphoneVolume.getValue());
            }

            repaint();
        };

        restoreGain.onClick = [this] { controller.setRestoreMicGain (restoreGain.getToggleState()); };
        addAndMakeVisible (restoreGain);

        latency.setLabels ({ kLatencies[0].label, kLatencies[1].label, kLatencies[2].label });
        latency.onChange = [this] (int i) { controller.setLatency (kLatencies[i].frames); };
        addAndMakeVisible (latency);

        makeDefault.onClick = [this] { controller.setMakeDefault (makeDefault.getToggleState()); };
        addAndMakeVisible (makeDefault);

        keepDefault.onClick = [this] { controller.setKeepDefault (keepDefault.getToggleState()); };
        addAndMakeVisible (keepDefault);

        startAtLogin.onClick = [this]
        {
            controller.getState().setSetting (ids::startAtLogin, startAtLogin.getToggleState() ? 1 : 0);

            if (installManager != nullptr)
                installManager->setStartAtLogin (startAtLogin.getToggleState());      // the service is enabled or disabled to match
        };
        addAndMakeVisible (startAtLogin);

        // Wave:3 hardware: shown only while the selected input is a Wave:3.
        allowHardware.getProperties().set (style::button, "primary");
        allowHardware.setTooltip ("ClipGuard stops distortion when you are loud. Headroom needs your password once to be allowed to control the microphone.");
        allowHardware.onClick = [this] { controller.requestHardwarePermission(); };
        addAndMakeVisible (allowHardware);

        clipGuardToggle.setTooltip ("The mic switches to a quieter input just before it would distort");
        clipGuardToggle.onClick = [this] { controller.setClipGuard (clipGuardToggle.getToggleState()); };
        addAndMakeVisible (clipGuardToggle);

        restoreClipGuardToggle.setTooltip ("The mic forgets ClipGuard when it loses power. Headroom puts it back when it starts.");
        restoreClipGuardToggle.onClick = [this] { controller.setRestoreClipGuard (restoreClipGuardToggle.getToggleState()); };
        addAndMakeVisible (restoreClipGuardToggle);

        safeGainToggle.setTooltip ("Keeps the mic's own gain at a level that does not distort, and adds the rest in Headroom so it sounds the same");
        safeGainToggle.onClick = [this] { controller.setSafeGain (safeGainToggle.getToggleState()); };
        addAndMakeVisible (safeGainToggle);

        // Experimental: another model's USB controls, from a published description that Headroom could not try.
        experimentalToggle.setTooltip ("Headroom has no way to try this on the real thing. It follows a published description of the mic's USB protocol, with the same safeguards as for the Wave:3.");
        experimentalToggle.onClick = [this] { controller.setExperimentalHardware (experimentalToggle.getToggleState()); };
        addAndMakeVisible (experimentalToggle);

        uiSize.onChange = [this] (int i) { controller.getState().setSetting (kUiScale, i == 0 ? 0.0 : kScales[i - 1]); };
        addAndMakeVisible (uiSize);

        addFolder.getProperties().set (style::icon, "plus");
        addFolder.setMouseClickGrabsKeyboardFocus (false);
        addFolder.onClick = [this] { chooseFolder(); };
        addAndMakeVisible (addFolder);

        rescan.getProperties().set (style::icon, "refresh-cw");
        rescan.setMouseClickGrabsKeyboardFocus (false);
        rescan.onClick = [this]
        {
            auto& plugins = controller.getPlugins();

            if (plugins.isScanning())
                plugins.stopScan();
            else
                plugins.startBackgroundScan (true);

            updateRescanButton();
            layoutFor (getWidth());
        };
        addAndMakeVisible (rescan);

        openLog.setMouseClickGrabsKeyboardFocus (false);
        openLog.onClick = [] { auto dir = Paths::getStateDir(); dir.createDirectory(); dir.startAsProcess(); };
        addAndMakeVisible (openLog);

        runSetup.setMouseClickGrabsKeyboardFocus (false);
        runSetup.onClick = [this] { if (settingsView != nullptr && settingsView->onRunSetup) settingsView->onRunSetup(); };
        addChildComponent (runSetup);

        uninstall.getProperties().set (style::button, "danger");
        uninstall.setMouseClickGrabsKeyboardFocus (false);
        uninstall.onClick = [this] { if (settingsView != nullptr && settingsView->onUninstall) settingsView->onUninstall(); };
        addChildComponent (uninstall);

        clock.addListener (this);
    }

    ~Content() override
    {
        clock.removeListener (this);
    }

    int getMicrophoneY() const noexcept     { return micSectionY; }

    // ---- Model -> controls -------------------------------------------------------------------------
    void sync()
    {
        const juce::ScopedValueSetter<bool> guard (syncing, true);
        auto& state = controller.getState();
        auto& backend = controller.getBackend();
        status = controller.getBackendStatus();

        // Devices: "Automatic" first, resolved to the device it currently means.
        auto devices = [] (const std::vector<DeviceInfo>& list, const juce::String& resolved)
        {
            std::vector<Dropdown::Item> items;
            Dropdown::Item automatic;
            automatic.key = "auto";
            automatic.text = resolved.isNotEmpty() ? "Automatic (" + resolved + ")" : juce::String ("Automatic");
            items.push_back (std::move (automatic));

            for (const auto& d : list)
            {
                Dropdown::Item item;
                item.key = d.id;
                item.text = d.label;
                items.push_back (std::move (item));
            }

            return items;
        };

        const auto inputs = backend.getInputs();
        inputDropdown.setItems (devices (inputs, status.inputLabel));
        inputDropdown.setSelectedKeyOr (state.getInputDevice(), "auto");
        playthrough::refresh (outputDropdown, controller, Mix::Monitor, true);
        playthrough::refresh (streamOutputDropdown, controller, Mix::Stream);

        // The hardware sliders show what the device says it is set to (-1 = this device has no hardware controls).
        micGainKnown = status.hardwareMicGain >= 0.0f;
        volumeKnown = status.hardwareHeadphoneVolume >= 0.0f;

        const bool leaveSliders = juce::Time::getMillisecondCounterHiRes() - lastSliderUseMs < 700.0;

        if (micGainKnown && ! micGain.isMouseButtonDown() && ! leaveSliders)
            micGain.setValue ((double) status.hardwareMicGain, juce::dontSendNotification);

        if (volumeKnown && ! headphoneVolume.isMouseButtonDown() && ! leaveSliders)
            headphoneVolume.setValue ((double) status.hardwareHeadphoneVolume, juce::dontSendNotification);

        restoreGain.setToggleState ((int) state.getSetting (ids::restoreMicGain) != 0, juce::dontSendNotification);

        hardware = controller.getHardwareControls();
        safeInfo = controller.getSafeGainInfo();

        clipGuardToggle.setToggleState (hardware.clipGuard.value_or (false), juce::dontSendNotification);
        clipGuardToggle.setEnabled (hardware.clipGuard.has_value() && ! hardware.busy);
        restoreClipGuardToggle.setToggleState ((int) state.getSetting (ids::restoreClipGuard) != 0, juce::dontSendNotification);
        safeGainToggle.setToggleState ((int) state.getSetting (ids::safeGain) != 0, juce::dontSendNotification);
        const bool asking = hardware.busy || hardware.experimental.busy;
        experimentalToggle.setToggleState (hardware.experimental.on, juce::dontSendNotification);
        allowHardware.setEnabled (! asking);
        safeGainToggle.setEnabled (hardware.usbPermission);         // Safe gain checks what the mic did over USB: it needs the permission
        allowHardware.setButtonText (asking ? "Waiting for your password" + theme::glyph::ellipsis
                                            : "Allow hardware controls" + theme::glyph::ellipsis);
        micGain.setEnabled (! safeInfo.on);         // Safe gain owns the mic's own gain while it is on

        const int frames = state.getLatency();
        latency.setSelected (frames <= 128 ? 0 : (frames >= 512 ? 2 : 1));
        makeDefault.setToggleState (state.getMakeDefault(), juce::dontSendNotification);
        keepDefault.setToggleState (state.getKeepDefault(), juce::dontSendNotification);
        keepDefault.setEnabled (state.getMakeDefault());        // the lock only means something while Headroom is the default
        startAtLogin.setToggleState ((int) state.getSetting (ids::startAtLogin) != 0, juce::dontSendNotification);

        // UI size: "Automatic (125%)" names what automatic comes to on this desktop.
        const auto automaticLabel = "Automatic (" + juce::String (juce::roundToInt (UiScale::effectiveScale (juce::var (0.0)) * 100.0f)) + "%)";

        if (automaticLabel != shownAutomaticLabel)
        {
            shownAutomaticLabel = automaticLabel;
            uiSize.setLabels ({ automaticLabel, "100%", "125%", "150%", "200%" }, false);
        }

        const auto scaleSetting = state.getSetting (kUiScale);
        int chosen = 0;

        if (! UiScale::isAutomatic (scaleSetting))
        {
            chosen = 1;

            for (int i = 1; i < 4; ++i)
                if (std::abs (kScales[i] - (double) scaleSetting) < std::abs (kScales[chosen - 1] - (double) scaleSetting))
                    chosen = i + 1;
        }

        uiSize.setSelected (chosen);

        // Plugin folders.
        const auto folders = state.getPluginFolders();

        if (folders != shownFolders)
        {
            shownFolders = folders;
            folderRemoveButtons.clear();

            for (int i = 0; i < folders.size(); ++i)
            {
                auto button = std::make_unique<IconButton> ("x", 14.0f);
                button->setTooltip ("Stop looking in this folder");
                button->onClick = [this, folder = folders[i]] { removeFolder (folder); };
                addAndMakeVisible (*button);
                folderRemoveButtons.push_back (std::move (button));
            }
        }

        updateRescanButton();
        layoutFor (getWidth());
    }

    // ---- Layout ------------------------------------------------------------------------------------
    int layoutFor (int width)
    {
        const int column = juce::jmin (kMaxColumn, width - 48);
        left = (width - column) / 2;
        columnWidth = column;
        const int controlX = left + kLabelWidth;
        const int controlW = column - kLabelWidth;

        texts.clear();
        rules.clear();
        bool firstSection = true;
        int y = 32;

        auto text = [this] (const juce::String& s, juce::Rectangle<int> r, Kind kind, juce::Colour colour = {})
        {
            texts.push_back ({ s, r, kind, colour });
        };

        auto section = [&] (const juce::String& title)
        {
            if (! firstSection)
            {
                y += 20;
                rules.push_back ({ left, y, column, 1 });
                y += 20;
            }

            firstSection = false;
            text (title, { left, y, column, 20 }, Kind::section);
            y += 28;
        };

        auto label = [&] (const juce::String& s, int h) { text (s, { left, y, kLabelWidth - 12, h }, Kind::label); };

        text ("Settings", { left, y, column, 30 }, Kind::title);
        y += 48;

        // ---- Microphone
        micSectionY = y - 16;
        section ("Microphone");

        label ("Input", kRowHeight);
        inputDropdown.setBounds (controlX, y + 4, juce::jmin (320, controlW), 32);
        y += kRowHeight;

        label ("Status", 48);
        {
            const auto mic = describeMic (status);
            micStatusDot = mic.dot;
            const int buttonW = 104;
            fixMic.setBounds (controlX + controlW - buttonW, y + 8, buttonW, 32);

            // Amber only when it is the thing to press.
            const bool needed = status.mic == MicState::SilentFromStart || status.mic == MicState::MutedOnDevice || status.mic == MicState::NotFound;
            fixMic.getProperties().set (style::button, needed ? "primary" : "");
            fixMic.repaint();
            text (mic.sentence, { controlX + 18, y, controlW - buttonW - 18 - 12, 48 }, Kind::status, mic.dot);
            y += 48;
        }

        micGainRowY = y;

        if (micGainKnown)
        {
            label (safeInfo.on && safeInfo.available ? "Mic gain (Safe gain)" : "Mic gain (on the " + shortDeviceName (status.inputLabel) + ")", kRowHeight);
            micGain.setBounds (controlX - 7, y + 4, juce::jmin (272, controlW - 48) + 14, 32);
            micGain.setVisible (true);
            y += kRowHeight;

            label ("At startup", kRowHeight);
            restoreGain.setBounds (controlX, y + 11, 32, 18);
            restoreGain.setVisible (true);
            text ("Restore mic gain when Headroom starts", { controlX + 44, y, controlW - 44, kRowHeight }, Kind::body);
            y += kRowHeight;
        }
        else
        {
            micGain.setVisible (false);
            restoreGain.setVisible (false);
            text ("Hardware gain is available for Elgato Wave mics.", { controlX, y, controlW, 24 }, Kind::hint);
            y += 28;
        }

        // ---- Wave:3 hardware: ClipGuard and Safe gain. Nothing of this for any other input.
        {
            constexpr int indent = 28;
            const bool waveThree = hardware.usbAvailable;
            const bool allowed = waveThree && hardware.usbPermission;

            allowHardware.setVisible (waveThree && ! hardware.usbPermission);
            clipGuardToggle.setVisible (allowed);
            restoreClipGuardToggle.setVisible (allowed);
            safeGainToggle.setVisible (waveThree);

            if (waveThree)
            {
                label ("ClipGuard", kRowHeight);

                if (allowed)
                {
                    clipGuardToggle.setBounds (controlX, y + 11, 32, 18);
                    text ("Stops distortion when you" + theme::glyph::quoteClose + "re loud", { controlX + 44, y, controlW - 44, kRowHeight }, Kind::body);
                    y += kRowHeight;

                    restoreClipGuardToggle.setBounds (controlX + indent, y + 11, 32, 18);
                    text ("Re-apply at startup", { controlX + indent + 44, y, controlW - indent - 44, kRowHeight }, Kind::body);
                    y += kRowHeight;
                }
                else
                {
                    allowHardware.setBounds (controlX, y + 4, juce::jmin (240, controlW), 32);
                    y += kRowHeight;
                    text ("Asks for your password once.", { controlX, y - 4, controlW, 20 }, Kind::hint);
                    y += 24;
                }

                if (hardware.error.isNotEmpty())
                {
                    text (hardware.error, { controlX, y, controlW, 36 }, Kind::bodyWrapped, theme::warn);
                    y += 40;
                }

                label ("Safe gain", kRowHeight);
                safeGainToggle.setBounds (controlX, y + 11, 32, 18);
                text ("Lower the mic" + theme::glyph::quoteClose + "s own gain, make it up in Headroom", { controlX + 44, y, controlW - 44, kRowHeight },
                      Kind::bodyWrapped, safeGainToggle.isEnabled() ? theme::text0 : theme::text2);
                y += kRowHeight;

                if (! hardware.usbPermission)
                {
                    text ("Needs hardware controls", { controlX + 44, y - 4, controlW - 44, 20 }, Kind::hint);
                    y += 20;
                }
                else if (safeInfo.on)
                {
                    if (safeInfo.hardwareKnown)
                        text ("Wave:3 +" + plainDb (safeInfo.hardwareDb) + " dB " + theme::glyph::dot + " Headroom +" + plainDb (safeInfo.makeupDb) + " dB",
                              { controlX + 44, y - 4, controlW - 44, 20 }, Kind::hint);
                    else
                        text ("Starts when the Wave:3 has been read.", { controlX + 44, y - 4, controlW - 44, 20 }, Kind::hint);

                    y += 20;

                    if (safeInfo.problem.isNotEmpty())
                    {
                        text (safeInfo.problem, { controlX + 44, y, controlW - 44, 36 }, Kind::bodyWrapped, theme::warn);
                        y += 40;
                    }
                }
            }
        }

        // ---- Experimental: the USB controls of another model, only with the opt-in. Nothing of this for any other input.
        {
            const auto& experimental = hardware.experimental;
            const bool offered = experimental.model.isNotEmpty();
            const bool askForPermission = offered && experimental.on && experimental.available && ! experimental.permission;

            experimentalToggle.setVisible (offered);
            allowHardware.setVisible (allowHardware.isVisible() || askForPermission);

            if (offered)
            {
                label ("Experimental", kRowHeight);
                experimentalToggle.setBounds (controlX, y + 11, 32, 18);
                text ("Experimental: hardware controls for " + experimental.model + " (untested by us)", { controlX + 44, y, controlW - 44, kRowHeight },
                      Kind::bodyWrapped);
                y += kRowHeight;

                if (askForPermission)
                {
                    allowHardware.setBounds (controlX + 44, y, juce::jmin (240, controlW - 44), 32);
                    y += 36;
                }
                else if (experimental.on)
                {
                    const juce::String hint = experimental.permission ? juce::String ("The mic gain is read and set over USB.")
                                                                      : "The " + experimental.model + " was not found on USB.";
                    text (hint, { controlX + 44, y - 4, controlW - 44, 20 }, Kind::hint);
                    y += 20;
                }

                if (experimental.on && experimental.error.isNotEmpty())
                {
                    text (experimental.error, { controlX + 44, y, controlW - 44, 36 }, Kind::bodyWrapped, theme::warn);
                    y += 40;
                }
            }
        }

        // ---- Headphones
        section ("Headphones");
        label ("Play through", kRowHeight);
        outputDropdown.setBounds (controlX, y + 4, juce::jmin (320, controlW), 32);
        y += kRowHeight;

        headphoneRowY = y;

        if (volumeKnown)
        {
            label ("Volume", kRowHeight);
            headphoneVolume.setBounds (controlX - 7, y + 4, juce::jmin (272, controlW - 48) + 14, 32);
            headphoneVolume.setVisible (true);
            y += kRowHeight;
        }
        else
        {
            headphoneVolume.setVisible (false);
            text ("Hardware volume is available for Elgato Wave headphones.", { controlX, y, controlW, 24 }, Kind::hint);
            y += 28;
        }

        // The Stream mix always feeds "Headroom Stream Mix" (what OBS records); it can also play to other devices.
        label ("Stream mix", kRowHeight);
        streamOutputDropdown.setBounds (controlX, y + 4, juce::jmin (320, controlW), 32);
        y += kRowHeight;
        text ("Besides Headroom Stream Mix, which OBS and other apps can record.", { controlX, y - 4, controlW, 20 }, Kind::hint);
        y += 24;

        // ---- Audio
        section ("Audio");
        label ("Delay", kRowHeight);
        latency.setBounds (controlX, y + 6, juce::jmin (latency.getIdealWidth() + 16, controlW), 28);
        y += kRowHeight;
        text ("Lower is quicker but needs a steadier computer. Balanced suits most setups.", { controlX, y, controlW, 20 }, Kind::hint);
        y += 24;

        // ---- System
        section ("System");
        label ("Default devices", kRowHeight);
        makeDefault.setBounds (controlX, y + 11, 32, 18);
        text ("Use Headroom for all apps (set as default devices)", { controlX + 44, y, controlW - 44, kRowHeight }, Kind::body);
        y += kRowHeight;

        // The lock is a sub-option of the switch above: indented, and grey while that one is off.
        {
            constexpr int indent = 28, lockRowHeight = 48;
            keepDefault.setBounds (controlX + indent, y + (lockRowHeight - 18) / 2, 32, 18);
            text ("Keep Headroom as the default, even when you plug in other speakers or headsets",
                  { controlX + indent + 44, y, controlW - indent - 44, lockRowHeight }, Kind::bodyWrapped,
                  keepDefault.isEnabled() ? theme::text0 : theme::text2);
            y += lockRowHeight;
        }

        label ("Startup", kRowHeight);
        startAtLogin.setBounds (controlX, y + 11, 32, 18);
        text ("Start Headroom when I log in", { controlX + 44, y, controlW - 44, kRowHeight }, Kind::body);
        y += kRowHeight;

        label ("Size", kRowHeight);
        uiSize.setBounds (controlX, y + 6, juce::jmin (uiSize.getIdealWidth() + 16, controlW), 28);
        y += kRowHeight + 12;

        // Discord card.
        {
            juce::AttributedString s;
            const auto regular = theme::sans (13.0f), bold = theme::sans (13.0f, theme::Sans::SemiBold);
            const auto chat = controller.getState().findChannel ("chat");
            const auto chatName = chat.isValid() && chat[ids::name].toString().isNotEmpty() ? chat[ids::name].toString() : juce::String ("Chat");

            s.setLineSpacing (3.0f);
            s.append ("In Discord " + theme::glyph::angleRight + " Voice & Video, set Input to ", regular, theme::text1);
            s.append ("Headroom Mic", bold, theme::text0);
            s.append (" and Output to ", regular, theme::text1);
            s.append ("Headroom " + chatName, bold, theme::text0);
            s.append (". Turn off Discord" + theme::glyph::quoteClose + "s Noise suppression, Echo cancellation and Automatic gain control "
                      + theme::glyph::emDash + " Headroom does this for you.", regular, theme::text1);

            const int textX = left + 16 + 20 + 12;
            discordLayout.createLayout (s, (float) (left + column - 16 - textX));
            const int cardH = juce::jmax (56, (int) std::ceil (discordLayout.getHeight()) + 32);
            discordCard = { left, y, column, cardH };
            discordTextArea = { textX, y + 16, left + column - 16 - textX, cardH - 32 };
            y += cardH;
        }

        // ---- Plugins
        section ("Plugins");

        for (int i = 0; i < shownFolders.size(); ++i)
        {
            text (shownFolders[i], { left, y, column - 40, 32 }, Kind::path);

            if (i < (int) folderRemoveButtons.size())
                folderRemoveButtons[(size_t) i]->setBounds (left + column - 28, y + 2, 28, 28);

            y += 32;
        }

        if (shownFolders.isEmpty())
        {
            text ("Headroom looks in the usual places. Add a folder if your plugins live somewhere else.", { left, y, column, 32 }, Kind::hint);
            y += 36;
        }

        y += 8;
        addFolder.setBounds (left, y, 116, 32);
        rescan.setBounds (left + 124, y, 96, 32);
        scanTextArea = { left + 232, y, column - 232, 32 };
        y += 32;

        // ---- About
        section ("About");
        label ("Version", kRowHeight);
        text (juce::String ("Headroom ") + HEADROOM_VERSION, { controlX, y, controlW - 140, kRowHeight }, Kind::body);
        openLog.setBounds (controlX + controlW - 132, y + 4, 132, 32);
        y += kRowHeight;

        // Only for a Headroom that installed itself (or the tarball's install): there is something to set up again and to remove.
        const bool offerSetup = installManager != nullptr && installManager->offersSetupAndUninstall();
        runSetup.setVisible (offerSetup);
        uninstall.setVisible (offerSetup);

        if (offerSetup)
        {
            label ("Setup", kRowHeight);
            runSetup.setBounds (controlX, y + 4, 168, 32);
            text ("Pick your devices, channels and voice again.", { controlX + 180, y + 12, controlW - 180, 20 }, Kind::hint);
            y += kRowHeight;

            label ("Uninstall", kRowHeight);
            uninstall.setBounds (controlX, y + 4, 168, 32);
            text ("Removes Headroom from this computer.", { controlX + 180, y + 12, controlW - 180, 20 }, Kind::hint);
            y += kRowHeight;
        }

        y += 32;

        setSize (width, y);
        repaint();
        return y;
    }

    void resized() override     { layoutFor (getWidth()); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::bg0);

        for (const auto& r : rules)
        {
            g.setColour (theme::line);
            g.fillRect (r);
        }

        for (const auto& t : texts)
        {
            switch (t.kind)
            {
                case Kind::title:
                    g.setColour (theme::text0);
                    g.setFont (theme::sans (20.0f, theme::Sans::SemiBold));
                    g.drawText (t.text, t.area, juce::Justification::centredLeft, false);
                    break;

                case Kind::section:
                    g.setColour (theme::text2);
                    g.setFont (theme::caps (11.0f));
                    g.drawText (t.text.toUpperCase(), t.area, juce::Justification::centredLeft, false);
                    break;

                case Kind::label:
                    g.setColour (theme::text1);
                    g.setFont (theme::sans (13.0f));
                    g.drawText (t.text, t.area, juce::Justification::centredLeft, true);
                    break;

                case Kind::body:
                    g.setColour (theme::text0);
                    g.setFont (theme::sans (13.0f));
                    g.drawText (t.text, t.area, juce::Justification::centredLeft, true);
                    break;

                case Kind::bodyWrapped:
                    g.setColour (t.colour);
                    g.setFont (theme::sans (13.0f));
                    g.drawFittedText (t.text, t.area, juce::Justification::centredLeft, 2, 1.0f);
                    break;

                case Kind::hint:
                    g.setColour (theme::text2);
                    g.setFont (theme::sans (12.0f));
                    g.drawFittedText (t.text, t.area, juce::Justification::topLeft, 2);
                    break;

                case Kind::path:
                    g.setColour (theme::text1);
                    g.setFont (theme::mono (12.0f));
                    g.drawText (t.text, t.area, juce::Justification::centredLeft, true);
                    break;

                case Kind::status:
                    g.setColour (t.colour);
                    g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ (float) t.area.getX() - 10.0f, (float) t.area.getCentreY() }));
                    g.setColour (theme::text0);
                    g.setFont (theme::sans (13.0f));
                    g.drawFittedText (t.text, t.area, juce::Justification::centredLeft, 2, 1.0f);
                    break;
            }
        }

        // Slider values.
        g.setColour (theme::text1);
        g.setFont (theme::mono (11.0f));

        if (micGain.isVisible())
            g.drawText (juce::String (juce::roundToInt (micGain.getValue() * 100.0)) + "%",
                        micGain.getRight() + 12, micGain.getY(), 44, micGain.getHeight(), juce::Justification::centredLeft, false);

        if (headphoneVolume.isVisible())
            g.drawText (juce::String (juce::roundToInt (headphoneVolume.getValue() * 100.0)) + "%",
                        headphoneVolume.getRight() + 12, headphoneVolume.getY(), 44, headphoneVolume.getHeight(), juce::Justification::centredLeft, false);

        // Discord card.
        g.setColour (theme::bg1);
        g.fillRoundedRectangle (discordCard.toFloat(), theme::kPanelRadius);
        g.setColour (theme::line);
        g.drawRoundedRectangle (discordCard.toFloat().reduced (0.5f), theme::kPanelRadius - 0.5f, 1.0f);
        Icons::draw (g, "message-circle", juce::Point<float> ((float) discordCard.getX() + 16.0f + 10.0f, (float) discordCard.getY() + 16.0f + 10.0f), 20.0f, theme::text1);
        discordLayout.draw (g, discordTextArea.toFloat());

        paintScanStatus (g);
    }

    void scrollTargets (juce::Viewport& target) const
    {
        target.setViewPosition (0, juce::jmax (0, micSectionY));
    }

    // The buttons call the owner's callbacks, which are set after construction.
    void wire (SettingsView& owner)     { settingsView = &owner; }

    juce::Component* findButton (const juce::String& buttonText)
    {
        for (auto* b : { static_cast<juce::Button*> (&runSetup), static_cast<juce::Button*> (&uninstall) })
            if (b->getButtonText().startsWith (buttonText))
                return b;

        return nullptr;
    }

private:
    enum class Kind { title, section, label, body, bodyWrapped, hint, path, status };
    struct Text { juce::String text; juce::Rectangle<int> area; Kind kind; juce::Colour colour; };

    void updateRescanButton()
    {
        const bool scanning = controller.getPlugins().isScanning();
        rescan.setButtonText (scanning ? "Stop" : "Rescan");
        rescan.getProperties().set (style::icon, scanning ? "x" : "refresh-cw");
        rescan.repaint();
    }

    void paintScanStatus (juce::Graphics& g) const
    {
        auto& plugins = controller.getPlugins();
        g.setFont (theme::sans (12.0f));

        if (plugins.isScanning())
        {
            g.setColour (theme::text1);
            const auto scanText = plugins.getScanStatusText();
            g.drawText (scanText.isNotEmpty() ? scanText : juce::String ("Scanning") + theme::glyph::ellipsis,
                        scanTextArea.withTrimmedBottom (10), juce::Justification::centredLeft, true);

            const auto bar = juce::Rectangle<float> ((float) scanTextArea.getX(), (float) scanTextArea.getBottom() - 8.0f, (float) scanTextArea.getWidth(), 2.0f);
            g.setColour (theme::bg3);
            g.fillRect (bar);
            g.setColour (theme::accent);
            g.fillRect (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, plugins.getScanProgress())));
        }
        else
        {
            const int count = plugins.getKnownEffects().size();
            g.setColour (theme::text1);
            g.drawText (count == 0 ? juce::String ("No plugins found") : juce::String (count) + (count == 1 ? " plugin found" : " plugins found"),
                        scanTextArea, juce::Justification::centredLeft, true);
        }
    }

    void meterTick (double) override
    {
        if (! isVisible())
            return;

        auto& plugins = controller.getPlugins();
        const int percent = juce::roundToInt (plugins.getScanProgress() * 100.0f);

        if (plugins.isScanning() != wasScanning || (plugins.isScanning() && percent != lastScanPercent))
        {
            wasScanning = plugins.isScanning();
            lastScanPercent = percent;
            updateRescanButton();
            repaint (scanTextArea);
        }
    }

    void chooseFolder()
    {
        chooser = std::make_shared<juce::FileChooser> ("Choose a plugin folder", juce::File(), "");
        juce::Component::SafePointer<Content> safe (this);

        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [safe] (const juce::FileChooser& fc)
                              {
                                  const auto folder = fc.getResult();

                                  if (safe == nullptr || ! folder.isDirectory())
                                      return;

                                  auto folders = safe->controller.getState().getPluginFolders();
                                  folders.addIfNotAlreadyThere (folder.getFullPathName());
                                  safe->controller.getState().setSetting (ids::pluginFolders, folders.joinIntoString ("\n"));
                              });
    }

    void removeFolder (const juce::String& folder)
    {
        auto folders = controller.getState().getPluginFolders();
        folders.removeString (folder);
        controller.getState().setSetting (ids::pluginFolders, folders.joinIntoString ("\n"));
    }

    AppController& controller;
    MeterClock& clock;
    InstallManager* installManager = nullptr;
    SettingsView* settingsView = nullptr;
    BackendStatus status;
    HardwareControls hardware;
    AppController::SafeGainInfo safeInfo;
    bool syncing = false, micGainKnown = false, volumeKnown = false, wasScanning = false;
    double lastSliderUseMs = 0.0;
    juce::String shownAutomaticLabel;
    int lastScanPercent = -1;

    Dropdown inputDropdown, outputDropdown, streamOutputDropdown;
    juce::TextButton fixMic { "Fix my mic" };
    juce::Slider micGain, headphoneVolume;
    SwitchToggle restoreGain, makeDefault, keepDefault, startAtLogin;
    SwitchToggle clipGuardToggle, restoreClipGuardToggle, safeGainToggle, experimentalToggle;
    juce::TextButton allowHardware { "Allow hardware controls" };
    SegmentedControl latency, uiSize;
    juce::TextButton addFolder { "Add folder" }, rescan { "Rescan" }, openLog { "Open log folder" };
    juce::TextButton runSetup { juce::String ("Run setup again") + theme::glyph::ellipsis }, uninstall { juce::String ("Uninstall Headroom") + theme::glyph::ellipsis };
    std::vector<std::unique_ptr<IconButton>> folderRemoveButtons;
    juce::StringArray shownFolders;
    std::shared_ptr<juce::FileChooser> chooser;

    std::vector<Text> texts;
    std::vector<juce::Rectangle<int>> rules;
    juce::TextLayout discordLayout;
    juce::Rectangle<int> discordCard, discordTextArea, scanTextArea;
    juce::Colour micStatusDot;
    int left = 0, columnWidth = 0, micSectionY = 0, micGainRowY = 0, headphoneRowY = 0;
};

SettingsView::SettingsView (AppController& c, MeterClock& clock, InstallManager* installManager)
{
    content = std::make_unique<Content> (c, clock, installManager);
    content->wire (*this);
    viewport.setViewedComponent (content.get(), false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (10);
    viewport.setWantsKeyboardFocus (false);
    addAndMakeVisible (viewport);

    syncFromModel();
}

SettingsView::~SettingsView()
{
    viewport.setViewedComponent (nullptr, false);
}

void SettingsView::syncFromModel()
{
    content->sync();
}

void SettingsView::scrollToMicrophone()
{
    content->scrollTargets (viewport);
}

juce::Component* SettingsView::findButton (const juce::String& text)
{
    return content->findButton (text);
}

void SettingsView::resized()
{
    viewport.setBounds (getLocalBounds());
    content->layoutFor (viewport.getMaximumVisibleWidth());
}

void SettingsView::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg0);
}
}
