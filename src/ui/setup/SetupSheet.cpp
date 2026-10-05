// SetupSheet implementation: the frame (progress, title, Back / Next) and one small component per step.
#include "ui/setup/SetupSheet.h"

#include "app/Log.h"
#include "engine/builtin/Presets.h"
#include "ui/Focus.h"
#include "ui/HeadroomLookAndFeel.h"
#include "ui/Icons.h"
#include "ui/MicStatus.h"
#include "ui/Theme.h"
#include "ui/widgets/Buttons.h"
#include "ui/setup/SetupWidgets.h"
#include "ui/widgets/Dropdown.h"
#include "ui/widgets/HoverFade.h"
#include "ui/widgets/Meter.h"

#include <algorithm>

namespace hr
{
namespace
{
    constexpr int kPad = 32, kHeaderBottom = 144, kFooterHeight = 72;
    constexpr int kTitleY = 52, kDescriptionY = 84;

    void sectionLabel (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area)
    {
        g.setColour (theme::text2);
        g.setFont (theme::caps (11.0f));
        g.drawText (text.toUpperCase(), area, juce::Justification::centredLeft, false);
    }

    void hint (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area, juce::Colour colour = theme::text2)
    {
        g.setColour (colour);
        g.setFont (theme::sans (12.0f));
        g.drawFittedText (text, area, juce::Justification::topLeft, 3, 1.0f);
    }

    void status (juce::Graphics& g, juce::Colour dot, const juce::String& text, juce::Rectangle<int> area)
    {
        g.setColour (dot);
        g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ (float) area.getX() + 4.0f, (float) area.getCentreY() }));
        g.setColour (theme::text1);
        g.setFont (theme::sans (12.0f));
        g.drawText (text, area.withTrimmedLeft (18), juce::Justification::centredLeft, true);
    }

    juce::String chatChannelName (AppController& controller)
    {
        auto& state = controller.getState();
        const auto chat = state.findChannel ("chat");

        if (chat.isValid() && chat[ids::name].toString().isNotEmpty())
            return chat[ids::name].toString();

        for (const auto& info : state.getChannelInfos())
            if (info.isApp())
                return info.name;

        return "Chat";
    }
}

// ---------------------------------------------------------------------------------------------------
// The pages
// ---------------------------------------------------------------------------------------------------

class SetupSheet::PageBase : public juce::Component
{
public:
    explicit PageBase (SetupSheet& owner) : sheet (owner) {}

    virtual juce::String title() const = 0;
    virtual juce::String description() const = 0;
    virtual void refresh() {}
    virtual bool canContinue() const        { return true; }
    virtual juce::String nextLabel() const  { return "Next"; }
    virtual juce::String backLabel() const  { return "Back"; }
    virtual bool showsBack() const          { return true; }
    virtual bool showsNext() const          { return true; }
    virtual bool numbered() const           { return true; }       // counts as a step (the closing pages do not)

protected:
    SetupSheet& sheet;
    SetupChoices& choices() { return sheet.choices; }
    const SetupChoices& choices() const { return sheet.choices; }
    AppController& controller() { return sheet.controller; }
    InstallManager& installer() { return sheet.installManager; }
    bool isFirstRun() const { return sheet.mode == Mode::firstRun; }
    void sheetChanged() { sheet.updateButtons(); }
};

namespace
{
using PageBase = SetupSheet::PageBase;

// ---- Welcome --------------------------------------------------------------------------------------
// A small picture of what Headroom is: five faders, each with the colour of its channel tag, as the mixer draws them.
class WelcomePage final : public PageBase
{
public:
    explicit WelcomePage (SetupSheet& s) : PageBase (s) {}

    juce::String title() const override         { return "Welcome to Headroom"; }
    juce::String description() const override   { return "Headroom gives your mic, game, music and chat their own faders, and makes your voice sound clean. Setup takes about a minute."; }
    juce::String nextLabel() const override     { return "Get started"; }
    juce::String backLabel() const override     { return "Not now"; }

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().withTrimmedTop (12).withHeight (juce::jmin (getHeight() - 12, 236)).toFloat();
        g.setColour (theme::bg0);
        g.fillRoundedRectangle (area, theme::kPanelRadius);
        g.setColour (theme::line);
        g.drawRoundedRectangle (area.reduced (0.5f), theme::kPanelRadius - 0.5f, 1.0f);

        struct Strip { const char* name; juce::Colour tag; float cap; float meterL, meterR; };
        const Strip strips[] = {
            { "Mic",    theme::channelTags[0], 0.64f, 0.58f, 0.55f },
            { "System", theme::channelTags[7], 0.74f, 0.34f, 0.31f },
            { "Game",   theme::channelTags[2], 0.70f, 0.72f, 0.66f },
            { "Music",  theme::channelTags[1], 0.52f, 0.46f, 0.50f },
            { "Chat",   theme::channelTags[4], 0.66f, 0.40f, 0.28f },
        };

        const float stripWidth = area.getWidth() / 5.0f;

        for (int i = 0; i < 5; ++i)
        {
            const auto& strip = strips[i];
            const float cx = area.getX() + stripWidth * ((float) i + 0.5f);

            g.setColour (strip.tag);
            g.fillRoundedRectangle (cx - 12.0f, area.getY() + 20.0f, 24.0f, 3.0f, 1.5f);

            g.setColour (theme::text1);
            g.setFont (theme::sans (12.0f, theme::Sans::Medium));
            g.drawText (strip.name, juce::Rectangle<float> (cx - 40.0f, area.getY() + 30.0f, 80.0f, 16.0f), juce::Justification::centred, false);

            // Track with its two meter bars, then the cap.
            const float trackTop = area.getY() + 62.0f, trackBottom = area.getBottom() - 28.0f, trackHeight = trackBottom - trackTop;
            const juce::Rectangle<float> track (cx - 3.0f, trackTop, 6.0f, trackHeight);
            g.setColour (theme::bg2);
            g.fillRoundedRectangle (track, 3.0f);

            auto meter = [&] (float x, float level)
            {
                const float h = trackHeight * level;
                g.setColour (theme::ok);
                g.fillRect (juce::Rectangle<float> (x, trackBottom - h, 3.0f, h));
            };

            meter (cx - 3.0f, strip.meterL);
            meter (cx, strip.meterR);

            const float capY = trackBottom - trackHeight * strip.cap;
            const juce::Rectangle<float> cap (cx - 20.0f, capY - 10.0f, 40.0f, 20.0f);
            g.setGradientFill (juce::ColourGradient (theme::capTop, cap.getX(), cap.getY(), theme::capBottom, cap.getX(), cap.getBottom(), false));
            g.fillRoundedRectangle (cap, 4.0f);
            g.setColour (theme::bg0.withAlpha (0.55f));
            g.fillRect (cap.getX() + 6.0f, cap.getCentreY() - 0.5f, cap.getWidth() - 12.0f, 1.0f);
        }

        hint (g, "You can change everything later in Settings.", juce::Rectangle<int> (0, (int) area.getBottom() + 14, getWidth(), 18));
    }
};

// ---- Devices --------------------------------------------------------------------------------------
class DevicesPage final : public PageBase, private juce::Timer
{
public:
    explicit DevicesPage (SetupSheet& s) : PageBase (s)
    {
        startTimerHz (30);
        micDropdown.setTooltip ("The microphone Headroom listens to");
        micDropdown.onChange = [this] (const juce::String& key) { choices().inputDevice = key; controller().setInputDevice (key); refresh(); };
        addAndMakeVisible (micDropdown);

        outputDropdown.setTooltip ("Where your headphones mix plays");
        outputDropdown.onChange = [this] (const juce::String& key) { choices().outputDevice = key; controller().setOutputDevice (key); refresh(); };
        addAndMakeVisible (outputDropdown);

        refresh();
    }

    juce::String title() const override         { return "Your mic and headphones"; }
    juce::String description() const override   { return "Headroom listens to your microphone and plays through your headphones. Elgato Wave devices are picked for you, and any other device works too."; }

    void refresh() override
    {
        auto& backend = controller().getBackend();
        const auto state = controller().getBackendStatus();
        const auto inputs = backend.getInputs(), outputs = backend.getOutputs();

        micStatus = describe (inputs, choices().inputDevice, state.inputLabel, "microphone");
        outputStatus = describe (outputs, choices().outputDevice, state.outputLabel, "headphones");

        const auto key = signature (inputs) + "|" + signature (outputs) + "|" + state.inputLabel + "|" + state.outputLabel;

        if (key != shownKey)
        {
            shownKey = key;
            micDropdown.setItems (items (inputs, state.inputLabel));
            outputDropdown.setItems (items (outputs, state.outputLabel));
        }

        micDropdown.setSelectedKeyOr (choices().inputDevice, "auto");
        outputDropdown.setSelectedKeyOr (choices().outputDevice, "auto");
        repaint();
    }

    void resized() override
    {
        micDropdown.setBounds (0, 24, getWidth(), 32);
        outputDropdown.setBounds (0, 168, getWidth(), 32);
    }

    void paint (juce::Graphics& g) override
    {
        sectionLabel (g, "Microphone", { 0, 0, getWidth(), 20 });
        status (g, micStatus.first, micStatus.second, { 0, 62, getWidth(), 20 });
        paintLevel (g, { 0, 94, getWidth(), 6 });
        hint (g, "Say something: the bar moves when Headroom hears your mic.", { 0, 108, getWidth(), 18 });
        sectionLabel (g, "Headphones", { 0, 144, getWidth(), 20 });
        status (g, outputStatus.first, outputStatus.second, { 0, 206, getWidth(), 20 });
        hint (g, "Not in the list? Plug it in and it shows up here by itself.", { 0, 252, getWidth(), 18 });
    }

private:
    void timerCallback() override
    {
        if (! isVisible())
            return;

        // The same peaks the mic's meter in the mixer reads; the engine keeps the largest since the last read.
        const auto levels = controller().getEngine().readChannelLevels (kMicChannel);
        level.push (juce::jmax (levels.input.l, levels.input.r), 1.0 / 30.0);
        repaint (0, 90, getWidth(), 14);
    }

    // A thin bar, green up to -12 dB, amber to -3 dB, red above: the same zones as the mixer's meters.
    void paintLevel (juce::Graphics& g, juce::Rectangle<int> area) const
    {
        const auto bar = area.toFloat();
        g.setColour (theme::bg2);
        g.fillRoundedRectangle (bar, 3.0f);

        const float db = juce::jlimit (-48.0f, 0.0f, level.displayDb);
        const float width = bar.getWidth() * (db + 48.0f) / 48.0f;
        const float x12 = bar.getWidth() * (48.0f - 12.0f) / 48.0f, x3 = bar.getWidth() * (48.0f - 3.0f) / 48.0f;

        g.saveState();
        g.reduceClipRegion (bar.toNearestInt());

        auto zone = [&] (float from, float to, juce::Colour colour)
        {
            if (width > from)
            {
                g.setColour (colour);
                g.fillRect (juce::Rectangle<float> (bar.getX() + from, bar.getY(), juce::jmin (width, to) - from, bar.getHeight()));
            }
        };

        zone (0.0f, x12, theme::ok);
        zone (x12, x3, theme::warn);
        zone (x3, bar.getWidth(), theme::bad);
        g.restoreState();
    }

    static juce::String signature (const std::vector<DeviceInfo>& list)
    {
        juce::String s;

        for (const auto& d : list)
            s << d.id << "=" << d.label << ";";

        return s;
    }

    static std::vector<Dropdown::Item> items (const std::vector<DeviceInfo>& list, const juce::String& resolved)
    {
        std::vector<Dropdown::Item> out;
        Dropdown::Item automatic;
        automatic.key = "auto";
        automatic.text = resolved.isNotEmpty() ? "Automatic (" + resolved + ")" : juce::String ("Automatic");
        out.push_back (std::move (automatic));

        for (const auto& d : list)
        {
            Dropdown::Item item;
            item.key = d.id;
            item.text = d.label;
            out.push_back (std::move (item));
        }

        return out;
    }

    // What to say under a list: which device it is on, and whether it is a Wave.
    static std::pair<juce::Colour, juce::String> describe (const std::vector<DeviceInfo>& list, const juce::String& chosen,
                                                            const juce::String& resolvedLabel, const char* what)
    {
        if (chosen != "auto")
        {
            for (const auto& d : list)
                if (d.id == chosen)
                    return { theme::ok, d.label + (d.isWave ? " (Elgato)" : "") };

            return { theme::warn, juce::String ("That ") + what + " is not plugged in." };
        }

        if (resolvedLabel.isEmpty())
            return { theme::warn, juce::String ("No ") + what + " found yet. Plug one in." };

        for (const auto& d : list)
            if (d.label == resolvedLabel && d.isWave)
                return { theme::ok, resolvedLabel + ", picked for you" };

        return { theme::ok, resolvedLabel + ", picked for you" };
    }

    Dropdown micDropdown, outputDropdown;
    std::pair<juce::Colour, juce::String> micStatus, outputStatus;
    juce::String shownKey;
    MeterBar level;
};

// ---- Channels -------------------------------------------------------------------------------------
class ChannelsPage final : public PageBase
{
public:
    explicit ChannelsPage (SetupSheet& s) : PageBase (s)
    {
        addButton.getProperties().set (style::button, "ghost");
        addButton.getProperties().set (style::icon, "plus");
        addButton.setMouseClickGrabsKeyboardFocus (false);
        addButton.onClick = [this] { addChannel(); };
        addAndMakeVisible (addButton);
        rebuild();
    }

    juce::String title() const override         { return "Your channels"; }
    juce::String description() const override   { return "Each channel is one fader. Apps are sorted into them for you. Keep the ones you want, rename them, or add your own."; }
    bool canContinue() const override
    {
        return std::any_of (choices().channels.begin(), choices().channels.end(), [] (const SetupChoices::Channel& c) { return c.keep; });
    }

    void resized() override
    {
        int y = 0;

        for (size_t i = 0; i < rows.size(); ++i)
        {
            auto& row = *rows[i];
            row.tick.setBounds (0, y + 11, 18, 18);
            row.editor.setBounds (60, y + 4, juce::jmin (300, getWidth() - 60), 32);
            y += kRowHeight;
        }

        addButton.setBounds (0, y + 8, 148, 32);
        listBottom = y;
    }

    void paint (juce::Graphics& g) override
    {
        for (size_t i = 0; i < rows.size(); ++i)
        {
            const auto& channel = choices().channels[i];
            const float cy = (float) ((int) i * kRowHeight + kRowHeight / 2);
            g.setColour (theme::colourFromHex (channel.colour).withMultipliedAlpha (channel.keep ? 1.0f : 0.3f));
            g.fillEllipse (juce::Rectangle<float> (10.0f, 10.0f).withCentre ({ 38.0f, cy }));
        }

        if (! canContinue())
            hint (g, "Keep at least one channel.", { 0, listBottom + 48, getWidth(), 18 }, theme::warn);
        else if ((int) choices().channels.size() >= SetupChoices::kMaxChannels)
            hint (g, "That is plenty for now. You can add more in the mixer later.", { 0, listBottom + 48, getWidth(), 18 });
        else
            hint (g, "Your mic is always there. You can add and remove channels in the mixer later too.", { 0, listBottom + 48, getWidth(), 18 });
    }

    // For the smoke test.
    int getNumRows() const noexcept             { return (int) rows.size(); }
    TickBox* getTick (int index)                { return index < (int) rows.size() ? &rows[(size_t) index]->tick : nullptr; }
    juce::TextEditor* getEditor (int index)     { return index < (int) rows.size() ? &rows[(size_t) index]->editor : nullptr; }
    juce::TextButton& getAddButton() noexcept   { return addButton; }

    void addChannel()
    {
        if ((int) choices().channels.size() >= SetupChoices::kMaxChannels)
            return;

        SetupChoices::Channel channel;
        channel.isNew = true;
        channel.name = "Channel " + juce::String ((int) choices().channels.size() + 1);
        channel.colour = getChannelColourPalette()[choices().channels.size() % getChannelColourPalette().size()];
        choices().channels.push_back (channel);
        rebuild();

        if (auto* editor = getEditor ((int) rows.size() - 1))
        {
            editor->grabKeyboardFocus();
            editor->selectAll();
        }

        sheetChanged();
    }

private:
    static constexpr int kRowHeight = 40;

    struct Row
    {
        TickBox tick;
        juce::TextEditor editor;
    };

    void rebuild()
    {
        rows.clear();

        for (size_t i = 0; i < choices().channels.size(); ++i)
        {
            auto row = std::make_unique<Row>();
            auto& channel = choices().channels[i];

            row->tick.setToggleState (channel.keep, juce::dontSendNotification);
            row->tick.onClick = [this, i, tick = &row->tick]
            {
                choices().channels[i].keep = tick->getToggleState();
                rows[i]->editor.setEnabled (tick->getToggleState());
                repaint();
                sheetChanged();
            };

            row->editor.setFont (theme::sans (13.0f));
            row->editor.setJustification (juce::Justification::centredLeft);
            row->editor.setIndents (10, 0);
            row->editor.setBorder (juce::BorderSize<int> (0));
            row->editor.setInputRestrictions (24);
            row->editor.setText (channel.name, false);
            row->editor.setEnabled (channel.keep);
            row->editor.onTextChange = [this, i] { choices().channels[i].name = rows[i]->editor.getText(); };
            row->editor.onReturnKey = [this, i] { rows[i]->editor.giveAwayKeyboardFocus(); };
            row->editor.onEscapeKey = [this, i] { rows[i]->editor.giveAwayKeyboardFocus(); };

            addAndMakeVisible (row->tick);
            addAndMakeVisible (row->editor);
            rows.push_back (std::move (row));
        }

        addButton.setEnabled ((int) choices().channels.size() < SetupChoices::kMaxChannels);
        resized();
        repaint();
    }

    std::vector<std::unique_ptr<Row>> rows;
    juce::TextButton addButton { "Add a channel" };
    int listBottom = 0;
};

// ---- Voice ----------------------------------------------------------------------------------------
class VoicePage final : public PageBase
{
public:
    explicit VoicePage (SetupSheet& s) : PageBase (s)
    {
        for (const auto& preset : micPresets())
            addChoice (preset.id, preset.displayName, preset.description);

        if (choices().preset == "custom")
            addChoice ("custom", "Your own setup", "Keep the effects on your mic the way they are now");

        noiseMode.setLabels ({ "Best", "Fast", "Light" });
        noiseMode.onChange = [this] (int segment)
        {
            choices().noiseMode = kModeOfSegment[segment];
            choices().noiseModeChosen = true;
            repaint();
        };
        addAndMakeVisible (noiseMode);

        refresh();
    }

    juce::String title() const override         { return "Your voice"; }
    juce::String description() const override   { return "Pick how Headroom treats your microphone. You can change all of it later, effect by effect, in the mixer."; }

    void refresh() override
    {
        for (auto& choice : choiceRows)
            choice.row->setToggleState (choice.id == choices().preset, juce::dontSendNotification);

        const bool noise = choices().preset != "off";
        noiseMode.setEnabled (noise);

        for (int segment = 0; segment < 3; ++segment)
            if (kModeOfSegment[segment] == choices().noiseMode)
                noiseMode.setSelected (segment);

        repaint();
    }

    void resized() override
    {
        int y = 0;

        for (auto& choice : choiceRows)
        {
            choice.row->setBounds (0, y, getWidth(), kRowHeight);
            y += kRowHeight + 4;
        }

        noiseLabelY = y + 14;
        noiseMode.setBounds (0, noiseLabelY + 24, juce::jmin (noiseMode.getIdealWidth() + 16, getWidth()), 28);
    }

    void paint (juce::Graphics& g) override
    {
        sectionLabel (g, "Noise removal", { 0, noiseLabelY, getWidth(), 20 });

        juce::String caption;
        const int mode = choices().noiseMode;

        if (choices().preset == "off")
            caption = "Off with this preset, because there are no effects on the mic.";
        else if (mode == 1)
            caption = "The cleanest voice. This is what most people want.";
        else if (mode == 0)
            caption = "Reacts 20 ms sooner, with a little less clean-up.";
        else
            caption = "Uses the least of your computer. Good for older machines.";

        hint (g, caption, { 0, noiseLabelY + 24 + 28 + 8, getWidth(), 20 });
    }

private:
    static constexpr int kRowHeight = 48;
    static constexpr int kModeOfSegment[3] = { 1, 0, 2 };       // Best, Fast, Light -> the effect's "mode" choice index

    struct Choice { juce::String id; std::unique_ptr<ChoiceRow> row; };

    void addChoice (const juce::String& id, const juce::String& name, const juce::String& line)
    {
        Choice choice;
        choice.id = id;
        choice.row = std::make_unique<ChoiceRow> (name, line);
        choice.row->onClick = [this, id]
        {
            choices().preset = id;

            if (id != "custom" && ! choices().noiseModeChosen)
                choices().noiseMode = SetupChoices::defaultNoiseModeFor (id);

            refresh();
        };
        addAndMakeVisible (*choice.row);
        choiceRows.push_back (std::move (choice));
    }

    std::vector<Choice> choiceRows;
    SegmentedControl noiseMode;
    int noiseLabelY = 0;
};

// ---- System ---------------------------------------------------------------------------------------
class SystemPage final : public PageBase
{
public:
    explicit SystemPage (SetupSheet& s) : PageBase (s)
    {
        login = add ("Start Headroom when I log in", "It runs quietly in the background, so Discord always finds the right mic.",
                     choices().startAtLogin, [this] (bool on) { choices().startAtLogin = on; });

        defaults = add ("Use Headroom for all apps", "Games, browsers and chat apps play through Headroom, so each one gets a fader.",
                        choices().makeDefault, [this] (bool on) { choices().makeDefault = on; refresh(); });

        keep = add ("Keep Headroom as the default", "Even when you plug in other speakers or a headset.",
                    choices().keepDefault, [this] (bool on) { choices().keepDefault = on; });
        keep->setIndent (28);

        if (installer().canInstall() && installer().isInManagedFolder())
        {
            const auto folder = installer().getAppImage().getParentDirectory().getFileName();
            place = add ("Keep Headroom in " + folder, "Your AppImage manager already looks after this file. Headroom starts from it instead of making a copy.",
                         choices().keepInPlace, [this] (bool on) { choices().keepInPlace = on; });
        }

        refresh();
    }

    juce::String title() const override         { return "Starting and default devices"; }
    juce::String description() const override   { return "Headroom works best when it starts with your computer and your apps play through it. Everything here can be changed in Settings."; }

    void refresh() override
    {
        keep->setEnabledLook (choices().makeDefault);
        keep->getToggle().setToggleState (choices().keepDefault, juce::dontSendNotification);
    }

    void resized() override
    {
        int y = 0;

        for (auto* row : rows)
        {
            row->setBounds (0, y, getWidth(), SwitchRow::kHeight);
            y += SwitchRow::kHeight + 8;
        }
    }

    SwitchRow* getRow (int index)   { return index < (int) rows.size() ? rows[(size_t) index] : nullptr; }

private:
    SwitchRow* add (const juce::String& title, const juce::String& caption, bool initial, std::function<void (bool)> onChange)
    {
        owned.push_back (std::make_unique<SwitchRow> (title, caption, initial, std::move (onChange)));
        auto* row = owned.back().get();
        addAndMakeVisible (*row);
        rows.push_back (row);
        return row;
    }

    std::vector<std::unique_ptr<SwitchRow>> owned;
    std::vector<SwitchRow*> rows;
    SwitchRow *login = nullptr, *defaults = nullptr, *keep = nullptr, *place = nullptr;
};

// ---- Wave:3 hardware controls ------------------------------------------------------------------------
class HardwarePage final : public PageBase
{
public:
    explicit HardwarePage (SetupSheet& s) : PageBase (s)
    {
        allow.getProperties().set (style::button, "primary");
        allow.setTooltip ("ClipGuard stops distortion when you are loud. Headroom needs your password once to be allowed to control the microphone.");
        allow.onClick = [this] { controller().requestHardwarePermission(); refresh(); };
        addAndMakeVisible (allow);

        clipGuard = std::make_unique<SwitchRow> ("ClipGuard", "Stops distortion when you are loud: the mic switches to a quieter input just before it would clip.",
                                                 choices().clipGuard, [this] (bool on) { choices().clipGuard = on; });
        safeGain = std::make_unique<SwitchRow> ("Safe gain", "Keeps the mic's own gain low and makes the rest up in Headroom, so loud moments are less likely to distort.",
                                                choices().safeGain, [this] (bool on) { choices().safeGain = on; });
        addAndMakeVisible (*clipGuard);
        addAndMakeVisible (*safeGain);
        refresh();
    }

    juce::String title() const override         { return noun + " controls"; }
    juce::String description() const override   { return "Optional. Your " + noun + " can protect itself from loud peaks. Headroom needs your password once, to be allowed to control the mic and nothing else."; }

    void refresh() override
    {
        noun = micNoun (controller().getBackendStatus());       // the backend names the model: "Wave:3", "Wave XLR"
        hardware = controller().getHardwareControls();
        allow.setVisible (! hardware.usbPermission);
        allow.setEnabled (! hardware.busy);
        allow.setButtonText (hardware.busy ? "Waiting for your password" + theme::glyph::ellipsis : "Allow hardware controls" + theme::glyph::ellipsis);
        clipGuard->setEnabledLook (hardware.usbPermission);
        safeGain->setEnabledLook (hardware.usbPermission);
        repaint();
    }

    void resized() override
    {
        allow.setBounds (0, 6, 232, 32);
        clipGuard->setBounds (0, 78, getWidth(), SwitchRow::kHeight);
        safeGain->setBounds (0, 78 + SwitchRow::kHeight + 8, getWidth(), SwitchRow::kHeight);
    }

    void paint (juce::Graphics& g) override
    {
        if (hardware.usbPermission)
            status (g, theme::ok, "Allowed. Headroom can control your " + noun + ".", { 0, 6, getWidth(), 32 });
        else if (hardware.error.isNotEmpty())
            hint (g, hardware.error, { 244, 8, getWidth() - 244, 36 }, theme::warn);
        else
            hint (g, "Asks for your password once.", { 244, 14, getWidth() - 244, 20 });

        if (! hardware.usbPermission)
            hint (g, "Both switches below need this first.", { 0, 48, getWidth(), 20 });

        hint (g, "You can skip this and switch it on later in Settings.", { 0, 78 + 2 * (SwitchRow::kHeight + 8) + 8, getWidth(), 20 });
    }

    SwitchRow& getClipGuardRow() noexcept   { return *clipGuard; }
    SwitchRow& getSafeGainRow() noexcept    { return *safeGain; }

private:
    juce::TextButton allow { "Allow hardware controls" };
    std::unique_ptr<SwitchRow> clipGuard, safeGain;
    HardwareControls hardware;
    juce::String noun { "mic" };
};

// ---- Installing -----------------------------------------------------------------------------------
class InstallingPage final : public PageBase, private juce::Timer
{
public:
    explicit InstallingPage (SetupSheet& s) : PageBase (s)   { startTimerHz (30); }

    juce::String title() const override         { return "Setting up Headroom"; }
    juce::String description() const override   { return "This takes a few seconds."; }
    bool showsBack() const override             { return false; }
    bool showsNext() const override             { return false; }
    bool numbered() const override              { return false; }

    void paint (juce::Graphics& g) override
    {
        const auto track = juce::Rectangle<float> (0.0f, 40.0f, (float) getWidth(), 3.0f);
        g.setColour (theme::bg2);
        g.fillRoundedRectangle (track, 1.5f);

        // One short bar that travels along the track and wraps: nothing bounces.
        const float length = track.getWidth() * 0.22f;
        const float travel = track.getWidth() + length;
        const float x = std::fmod ((float) juce::Time::getMillisecondCounterHiRes() * 0.28f, travel) - length;
        g.setColour (theme::accent);
        g.saveState();
        g.reduceClipRegion (track.toNearestInt());
        g.fillRoundedRectangle (juce::Rectangle<float> (x, track.getY(), length, track.getHeight()), 1.5f);
        g.restoreState();

        hint (g, "Copying Headroom, adding it to your app menu and setting it to start when you log in.", { 0, 62, getWidth(), 36 });
    }

private:
    void timerCallback() override   { repaint(); }
};

// ---- Done -----------------------------------------------------------------------------------------
class DonePage final : public PageBase
{
public:
    explicit DonePage (SetupSheet& s) : PageBase (s) {}

    juce::String title() const override         { return "You're all set"; }
    juce::String description() const override
    {
        if (sheet.installedNow())
            return choices().startAtLogin ? "Headroom is installed. It starts when you log in, and you can open it from your app menu."
                                          : "Headroom is installed. Open it from your app menu whenever you need it.";

        return "Your choices are saved.";
    }
    bool showsBack() const override             { return false; }
    bool numbered() const override              { return false; }
    juce::String nextLabel() const override     { return sheet.installedNow() && isFirstRun() ? "Open Headroom" : "Done"; }

    void paint (juce::Graphics& g) override
    {
        juce::AttributedString s;
        const auto regular = theme::sans (13.0f), bold = theme::sans (13.0f, theme::Sans::SemiBold);
        s.setLineSpacing (3.0f);
        s.append ("In Discord " + theme::glyph::angleRight + " Voice & Video, set Input to ", regular, theme::text1);
        s.append ("Headroom Mic", bold, theme::text0);
        s.append (" and Output to ", regular, theme::text1);
        s.append ("Headroom " + chatChannelName (controller()), bold, theme::text0);
        s.append (". Turn off Discord" + theme::glyph::quoteClose + "s own Noise suppression, Echo cancellation and Automatic gain control "
                  + theme::glyph::emDash + " Headroom does this for you.", regular, theme::text1);

        const int textX = 16 + 20 + 12;
        juce::TextLayout layout;
        layout.createLayout (s, (float) (getWidth() - 16 - textX));
        const int cardHeight = juce::jmax (56, (int) std::ceil (layout.getHeight()) + 56);
        const juce::Rectangle<int> card (0, 8, getWidth(), cardHeight);

        g.setColour (theme::bg0);
        g.fillRoundedRectangle (card.toFloat(), theme::kPanelRadius);
        g.setColour (theme::line);
        g.drawRoundedRectangle (card.toFloat().reduced (0.5f), theme::kPanelRadius - 0.5f, 1.0f);

        Icons::draw (g, "message-circle", juce::Point<float> (16.0f + 10.0f, (float) card.getY() + 16.0f + 10.0f), 20.0f, theme::text1);

        g.setColour (theme::text0);
        g.setFont (theme::sans (13.0f, theme::Sans::SemiBold));
        g.drawText ("One thing in Discord", juce::Rectangle<int> (textX, card.getY() + 14, card.getWidth() - textX - 16, 20), juce::Justification::centredLeft, false);
        layout.draw (g, juce::Rectangle<float> ((float) textX, (float) card.getY() + 38.0f, (float) (card.getWidth() - textX - 16), layout.getHeight()));

        hint (g, "You can run this setup again any time from Settings.", { 0, card.getBottom() + 16, getWidth(), 18 });
    }
};

// ---- Failed ---------------------------------------------------------------------------------------
class FailedPage final : public PageBase
{
public:
    explicit FailedPage (SetupSheet& s) : PageBase (s) {}

    juce::String title() const override         { return "Setup could not finish"; }
    juce::String description() const override
    {
        const auto error = sheet.getInstallResult().error;
        return error.isNotEmpty() ? error : juce::String ("Something went wrong while setting up.");
    }
    bool numbered() const override              { return false; }
    juce::String nextLabel() const override     { return "Try again"; }

    void paint (juce::Graphics& g) override
    {
        hint (g, "Nothing else was changed. Your choices are kept, so Try again picks up where this stopped. Headroom's log has the details: "
                 "Settings " + theme::glyph::angleRight + " Open log folder.",
              { 0, 8, getWidth(), 60 });
    }
};
}

// ---------------------------------------------------------------------------------------------------
// The sheet
// ---------------------------------------------------------------------------------------------------

SetupSheet::SetupSheet (AppController& c, InstallManager& im, Mode m, std::function<void (Outcome)> closeCallback)
    : controller (c), installManager (im), mode (m), onClose (std::move (closeCallback)), choices (SetupChoices::startingPoint (c))
{
    // The same default switch as Settings has: a Gear Lever user keeps the file where it is.
    choices.keepInPlace = installManager.canInstall() && installManager.isInManagedFolder();
    choices.startAtLogin = mode == Mode::firstRun ? true : choices.startAtLogin;

    nextButton.getProperties().set (style::button, "primary");
    nextButton.onClick = [this] { next(); };
    addAndMakeVisible (nextButton);                 // first, so that it is the one that has the focus when the sheet opens

    backButton.getProperties().set (style::button, "ghost");
    backButton.onClick = [this] { back(); };
    addAndMakeVisible (backButton);

    showPage (Page::welcome);
    startTimerHz (2);
}

SetupSheet::~SetupSheet()
{
    stopTimer();
}

std::vector<SetupSheet::Page> SetupSheet::choicePages() const
{
    std::vector<Page> pages { Page::welcome, Page::devices, Page::channels, Page::voice, Page::system };

    if (controller.getHardwareControls().usbAvailable)
        pages.push_back (Page::hardware);

    return pages;
}

juce::Component* SetupSheet::getPageComponent() noexcept
{
    return content.get();
}

int SetupSheet::getStepCount() const
{
    return (int) choicePages().size();
}

int SetupSheet::getStepNumber() const
{
    const auto pages = choicePages();
    const auto it = std::find (pages.begin(), pages.end(), page);
    return it == pages.end() ? 0 : (int) (it - pages.begin()) + 1;
}

void SetupSheet::showPage (Page newPage)
{
    page = newPage;
    content.reset();

    switch (page)
    {
        case Page::welcome:     content = std::make_unique<WelcomePage> (*this); break;
        case Page::devices:     content = std::make_unique<DevicesPage> (*this); break;
        case Page::channels:    content = std::make_unique<ChannelsPage> (*this); break;
        case Page::voice:       content = std::make_unique<VoicePage> (*this); break;
        case Page::system:      content = std::make_unique<SystemPage> (*this); break;
        case Page::hardware:    content = std::make_unique<HardwarePage> (*this); break;
        case Page::installing:  content = std::make_unique<InstallingPage> (*this); break;
        case Page::done:        content = std::make_unique<DonePage> (*this); break;
        case Page::failed:      content = std::make_unique<FailedPage> (*this); break;
    }

    addAndMakeVisible (*content);
    resized();
    updateButtons();
    repaint();
}

void SetupSheet::updateButtons()
{
    if (content == nullptr)
        return;

    nextButton.setVisible (content->showsNext());
    nextButton.setButtonText (content->numbered() && isLastChoicePage() ? finishLabel() : content->nextLabel());
    nextButton.setEnabled (content->canContinue());

    backButton.setVisible (content->showsBack());
    backButton.setButtonText (content->backLabel());

    repaint();
}

bool SetupSheet::isLastChoicePage() const
{
    const auto pages = choicePages();
    return ! pages.empty() && pages.back() == page;
}

juce::String SetupSheet::finishLabel() const
{
    return installManager.canInstall() && mode == Mode::firstRun ? "Set up Headroom" : "Save";
}

void SetupSheet::timerCallback()
{
    if (content != nullptr)
    {
        content->refresh();

        // A Wave:3 may appear while the sheet is open: the step count follows.
        const int hardware = controller.getHardwareControls().usbAvailable ? 1 : 0;

        if (hardware != hardwareSeen)
        {
            hardwareSeen = hardware;
            updateButtons();
        }
    }
}

void SetupSheet::next()
{
    if (content == nullptr || ! nextButton.isVisible() || ! nextButton.isEnabled())
        return;

    const auto pages = choicePages();

    switch (page)
    {
        case Page::welcome:
        case Page::devices:
        case Page::channels:
        case Page::voice:
        case Page::system:
        case Page::hardware:
        {
            const auto it = std::find (pages.begin(), pages.end(), page);

            if (it != pages.end() && it + 1 != pages.end())
                showPage (*(it + 1));
            else
                finish();

            break;
        }

        case Page::installing:
            break;

        case Page::done:
            if (installDone && mode == Mode::firstRun)
                installManager.openInstalled (installResult.program);
            else
                leave (Outcome::finished);

            break;

        case Page::failed:
            finish();
            break;
    }
}

void SetupSheet::back()
{
    if (content == nullptr || ! backButton.isVisible())
        return;

    const auto pages = choicePages();

    if (page == Page::welcome)
    {
        leave (Outcome::notNow);
        return;
    }

    if (page == Page::failed)
    {
        showPage (pages.back());
        return;
    }

    const auto it = std::find (pages.begin(), pages.end(), page);

    if (it != pages.end() && it != pages.begin())
        showPage (*(it - 1));
}

void SetupSheet::leave (Outcome outcome)
{
    if (onClose)
        onClose (outcome);
}

void SetupSheet::finish()
{
    if (! applied)
    {
        choices.apply (controller);
        applied = true;
    }

    // Anything to install? An AppImage that is not installed yet, or one being set up again.
    const bool installs = installManager.canInstall();

    if (! installs)
    {
        if (installManager.offersSetupAndUninstall())
            installManager.setStartAtLogin (choices.startAtLogin);

        installDone = false;
        showPage (Page::done);
        return;
    }

    showPage (Page::installing);
    installRan = true;

    InstallManager::SetupRequest request;
    request.keepInPlace = choices.keepInPlace;
    request.startAtLogin = choices.startAtLogin;
    request.startNow = false;       // this Headroom is running: it starts the installed one itself once it has quit

    juce::WeakReference<SetupSheet> self (this);

    installManager.installAsync (request, [self] (const install::Result& result)
    {
        if (auto* sheet = self.get())
        {
            sheet->installResult = result;
            sheet->installDone = result.ok;

            if (! result.ok)
                Log::warn ("Setup could not finish: " + result.error);

            sheet->showPage (result.ok ? Page::done : Page::failed);
        }
    });
}

void SetupSheet::resized()
{
    const int footerTop = getHeight() - kFooterHeight;
    const int buttonY = footerTop + 20;

    nextButton.setBounds (getWidth() - kPad - 136, buttonY, 136, 32);
    backButton.setBounds (kPad - 12, buttonY, getLookAndFeel().getTextButtonWidthToFitText (backButton, 32), 32);

    if (content != nullptr)
        content->setBounds (kPad, kHeaderBottom, getWidth() - 2 * kPad, footerTop - kHeaderBottom - 8);
}

void SetupSheet::paint (juce::Graphics& g)
{
    if (content == nullptr)
        return;

    // Which step, and a row of small segments: reached ones in amber.
    const int step = getStepNumber(), steps = getStepCount();

    g.setColour (theme::text2);
    g.setFont (theme::caps (11.0f));
    const auto label = content->numbered() ? juce::String ("Step ") + juce::String (step) + " of " + juce::String (steps)
                                           : (page == Page::installing ? juce::String ("Setting up") : (page == Page::failed ? juce::String ("Stopped") : juce::String ("All set")));
    g.drawText (label.toUpperCase(), juce::Rectangle<int> (kPad, 28, 240, 16), juce::Justification::centredLeft, false);

    {
        const int segments = steps;              // one per numbered step, so it matches "Step N of M"; the closing page lights them all
        const int segmentWidth = 16, gap = 4;
        int x = getWidth() - kPad - (segments * segmentWidth + (segments - 1) * gap);
        const int reached = content->numbered() ? step : segments;      // the closing pages are all reached

        for (int i = 0; i < segments; ++i)
        {
            g.setColour (i < reached ? theme::accent : theme::line);
            g.fillRoundedRectangle ((float) x, 34.0f, (float) segmentWidth, 3.0f, 1.5f);
            x += segmentWidth + gap;
        }
    }

    g.setColour (theme::text0);
    g.setFont (theme::sans (20.0f, theme::Sans::SemiBold));
    g.drawText (content->title(), juce::Rectangle<int> (kPad, kTitleY, getWidth() - 2 * kPad, 28), juce::Justification::centredLeft, true);

    g.setColour (theme::text1);
    g.setFont (theme::sans (13.0f));
    g.drawFittedText (content->description(), juce::Rectangle<int> (kPad, kDescriptionY, getWidth() - 2 * kPad, 44), juce::Justification::topLeft, 2, 1.0f);

    g.setColour (theme::line);
    g.fillRect (0, getHeight() - kFooterHeight, getWidth(), 1);
}
}
