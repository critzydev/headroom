// MixerView implementation.
#include "ui/views/MixerView.h"

#include "ui/Icons.h"
#include "ui/MicStatus.h"
#include "ui/Theme.h"
#include "ui/widgets/PlayThrough.h"

#include <map>

namespace hr
{
// The viewport's content: every channel strip plus the ghost "Add channel" strip, laid out left to right.
class MixerView::StripRow final : public juce::Component
{
public:
    std::vector<std::unique_ptr<ChannelStrip>> strips;
    AddChannelStrip addStrip;

    StripRow() { addAndMakeVisible (addStrip); }

    int getIdealWidth() const
    {
        return theme::kSidePadding + (int) strips.size() * (theme::kStripWidth + theme::kGap) + theme::kStripWidth + theme::kSidePadding;
    }

    void resized() override
    {
        int x = theme::kSidePadding;

        for (auto& strip : strips)
        {
            strip->setBounds (x, 0, theme::kStripWidth, getHeight());
            x += theme::kStripWidth + theme::kGap;
        }

        addStrip.setBounds (x, 0, theme::kStripWidth, getHeight());
    }
};

MixerView::MixerView (AppController& c, MeterClock& meterClock)
    : controller (c), clock (meterClock), master (c)
{
    row = std::make_unique<StripRow>();

    mixSwitch.setLabels ({ "Headphones", "Stream" });
    mixSwitch.setTitle ("Mix");
    mixSwitch.onChange = [this] (int i) { setMix (i == 0 ? Mix::Monitor : Mix::Stream); };
    addAndMakeVisible (mixSwitch);

    // "Play through": every device this mix plays to, ticked in a small list (the Stream mix: the extra ones).
    outputDropdown.setPrefix ("Output: ");
    outputDropdown.onOpenRequested = [this] (Dropdown&) { playthrough::open (outputDropdown, controller, mix); };
    addAndMakeVisible (outputDropdown);

    row->addStrip.onClick = [this] { addChannelPressed(); };

    viewport.setViewedComponent (row.get(), false);
    viewport.setScrollBarsShown (false, true);
    viewport.setScrollBarThickness (10);
    viewport.setSingleStepSizes (theme::kStripWidth + theme::kGap, 0);
    viewport.setWantsKeyboardFocus (false);
    addAndMakeVisible (viewport);
    addAndMakeVisible (master);

    clock.addListener (this);
    syncFromModel();
}

MixerView::~MixerView()
{
    clock.removeListener (this);
    viewport.setViewedComponent (nullptr, false);
}

int MixerView::getNumStrips() const noexcept
{
    return (int) row->strips.size();
}

ChannelStrip* MixerView::getLastStrip() const noexcept
{
    return row->strips.empty() ? nullptr : row->strips.back().get();
}

AddChannelStrip& MixerView::getAddStrip() noexcept
{
    return row->addStrip;
}

ChannelStrip* MixerView::findStrip (const juce::String& channelId) const
{
    for (auto& strip : row->strips)
        if (strip->getChannelId() == channelId)
            return strip.get();

    return nullptr;
}

void MixerView::setMix (Mix newMix)
{
    if (newMix == mix)
        return;

    mix = newMix;
    mixSwitch.setSelected (mix == Mix::Monitor ? 0 : 1);
    syncFromModel();
    resized();
}

void MixerView::setDrawerChannel (const juce::String& channelId)
{
    if (drawerChannel != channelId)
    {
        drawerChannel = channelId;
        syncFromModel();
    }
}

int MixerView::countActiveEffects (const juce::String& channelId) const
{
    int count = 0;
    const auto fx = AppState::getFx (controller.getState().findChannel (channelId));

    for (auto slot : fx)
        if ((int) slot[ids::bypassed] == 0 && (int) slot[ids::disabled] == 0
            && controller.getFxProcessor (slot[ids::uid].toString()) != nullptr)
            ++count;

    return count;
}

void MixerView::syncFromModel()
{
    const auto infos = controller.getState().getChannelInfos();

    // Reconcile the strips with the model, keeping existing ones (and their rename editors) alive.
    bool same = infos.size() == row->strips.size();

    for (size_t i = 0; same && i < infos.size(); ++i)
        same = row->strips[i]->getChannelId() == infos[i].id;

    if (! same)
    {
        std::vector<std::unique_ptr<ChannelStrip>> old = std::move (row->strips);
        row->strips.clear();

        for (const auto& info : infos)
        {
            std::unique_ptr<ChannelStrip> strip;

            for (auto& candidate : old)
                if (candidate != nullptr && candidate->getChannelId() == info.id)
                    strip = std::move (candidate);

            if (strip == nullptr)
            {
                strip = std::make_unique<ChannelStrip> (controller, info.id);
                strip->onToggleFx = [this] (const juce::String& id) { if (onToggleFx) onToggleFx (id); };
                strip->onRemove = [this] (const juce::String& id) { if (onRemoveChannel) onRemoveChannel (id); };
                row->addAndMakeVisible (*strip);
            }

            row->strips.push_back (std::move (strip));
        }

        for (auto& gone : old)
            if (gone != nullptr)
                row->removeChildComponent (gone.get());

        layoutContent();
    }

    // Who plays into which channel.
    std::map<juce::String, juce::StringArray> routed;

    for (const auto& stream : controller.getBackend().getAppStreams())
        if (stream.channelId.isNotEmpty())
            routed[stream.channelId].addIfNotAlreadyThere (stream.appName);

    const auto status = controller.getBackendStatus();

    for (auto& strip : row->strips)
    {
        ChannelStrip::Context ctx;
        ctx.mix = mix;
        ctx.routedApps = routed[strip->getChannelId()];
        ctx.micLabel = status.mic == MicState::NotFound ? juce::String() : status.inputLabel;
        ctx.activeEffects = countActiveEffects (strip->getChannelId());
        ctx.drawerOpen = strip->getChannelId() == drawerChannel;

        // An input channel says which device it listens to, or that the device is not there.
        for (const auto& input : status.inputChannels)
            if (input.channelId == strip->getChannelId())
            {
                ctx.deviceLabel = input.label;
                ctx.deviceConnected = input.connected;
            }

        strip->sync (ctx);
    }

    int extraDevices = 0;
    playthrough::summaryFor (controller, mix, &extraDevices);
    master.sync (mix, status.outputLabel, extraDevices);

    // 16 app channels and 8 inputs are the limits: when both are full there is nothing left to add.
    row->addStrip.setVisible (controller.getState().nextFreeSlot() >= 0 || controller.getState().nextFreeInputSlot() >= 0);

    // Header: mix switch and the "Play through" control of the mix that is shown.
    mixSwitch.setSelected (mix == Mix::Monitor ? 0 : 1);

    const bool stream = mix == Mix::Stream;
    outputDropdown.setPrefix (stream ? "Also play to: " : "Output: ");
    outputDropdown.setTooltip (stream ? "Other devices the Stream mix plays to, besides Headroom Stream Mix. Click to pick."
                                      : "Where the Headphones mix plays. Click to pick more than one.");
    playthrough::refresh (outputDropdown, controller, mix);

    if (showStreamNote != stream)
    {
        showStreamNote = stream;
        repaint (getLocalBounds().removeFromTop (theme::kMixHeaderHeight));
    }

    if (pendingStrip.isNotEmpty())
    {
        if (auto* strip = findStrip (pendingStrip))
        {
            const auto target = strip->getBounds();
            viewport.setViewPosition (juce::jmax (0, target.getRight() + theme::kSidePadding - viewport.getMaximumVisibleWidth()), 0);

            if (pendingRename)
                strip->beginRename();

            pendingStrip = {};
            pendingRename = false;
        }
    }

    resized();
}

// The ghost strip: a small menu. An app channel is what it always was; an input channel takes its sound from a capture device
// (a second mic, a capture card, a line-in), which is chosen right here.
void MixerView::addChannelPressed()
{
    auto& state = controller.getState();
    const bool appRoom = state.nextFreeSlot() >= 0, inputRoom = state.nextFreeInputSlot() >= 0;
    const auto inputs = controller.getBackend().getInputs();

    juce::StringArray used;

    for (const auto& info : state.getChannelInfos())
        if (info.isInput())
            used.add (info.device);

    constexpr int kAppId = 1, kInputIdBase = 100;
    juce::PopupMenu menu, devices;

    for (size_t i = 0; i < inputs.size(); ++i)
    {
        juce::PopupMenu::Item item (inputs[i].label);
        item.setID (kInputIdBase + (int) i).setTicked (used.contains (inputs[i].id));
        devices.addItem (std::move (item));
    }

    if (inputs.empty())
        devices.addItem (1000, "No inputs found", false);

    menu.addItem (kAppId, "App channel", appRoom);
    menu.addSubMenu ("Input" + theme::glyph::ellipsis, devices, inputRoom && ! inputs.empty());

    juce::Component::SafePointer<MixerView> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&row->addStrip).withStandardItemHeight (28).withMinimumWidth (176),
                        [safe, inputs] (int result)
                        {
                            if (safe == nullptr || result == 0)
                                return;

                            if (result == kAppId)
                                safe->addAppChannelFromMenu();
                            else if (result >= kInputIdBase && result - kInputIdBase < (int) inputs.size())
                                safe->addInputFromMenu (inputs[(size_t) (result - kInputIdBase)].id);
                        });
}

void MixerView::addAppChannelFromMenu()
{
    const auto id = controller.addChannel();

    if (id.isNotEmpty())
    {
        pendingStrip = id;
        pendingRename = true;
        syncFromModel();
    }
}

void MixerView::addInputFromMenu (const juce::String& deviceId)
{
    juce::String name;

    for (const auto& device : controller.getBackend().getInputs())
        if (device.id == deviceId)
            name = shortDeviceName (device.label);

    const auto id = controller.addInputChannel (deviceId, name);

    if (id.isNotEmpty())
    {
        pendingStrip = id;          // scrolled into view; its name is the device's, which can be renamed like any other
        pendingRename = false;
        syncFromModel();
    }
}

void MixerView::layoutContent()
{
    const int stripHeight = juce::jmax (0, viewport.getHeight() - theme::kSidePadding);
    row->setSize (juce::jmax (row->getIdealWidth(), viewport.getMaximumVisibleWidth()), stripHeight);
    row->resized();         // setSize is a no-op when the width didn't change, but a strip may have been added or removed
}

void MixerView::paint (juce::Graphics& g)
{
    const int masterX = getWidth() - theme::kSidePadding - theme::kMasterWidth;

    // Hairline between the channels and the master strip.
    g.setColour (theme::line);
    g.fillRect (masterX - 9, theme::kMixHeaderHeight, 1, getHeight() - theme::kMixHeaderHeight - theme::kSidePadding);

    if (showStreamNote)
    {
        const auto note = "Available as " + theme::glyph::quoteOpen + "Headroom Stream Mix" + theme::glyph::quoteClose + " in OBS";
        g.setColour (theme::text1);
        g.setFont (theme::sans (12.0f));
        g.drawText (note, streamNoteArea, juce::Justification::centredLeft, true);
    }
}

void MixerView::resized()
{
    auto area = getLocalBounds();
    auto header = area.removeFromTop (theme::kMixHeaderHeight);
    const int masterX = getWidth() - theme::kSidePadding - theme::kMasterWidth;

    // Header controls: 28 px tall, vertically centred in the 44 px row.
    mixSwitch.setBounds (theme::kSidePadding, header.getY() + 8, juce::jmax (180, mixSwitch.getIdealWidth() + 16), 28);

    const int dropdownWidth = juce::jmin (340, juce::jmax (180, outputDropdown.getIdealWidth()));
    outputDropdown.setBounds (getWidth() - theme::kSidePadding - dropdownWidth, header.getY() + 8, dropdownWidth, 28);

    // The note about OBS sits between the mix switch and the control (it gives way when the window is narrow).
    streamNoteArea = juce::Rectangle<int> (mixSwitch.getRight() + 16, header.getY() + 8,
                                           juce::jmax (0, outputDropdown.getX() - 16 - (mixSwitch.getRight() + 16)), 28);

    master.setBounds (masterX, area.getY(), theme::kMasterWidth, area.getHeight() - theme::kSidePadding);
    viewport.setBounds (0, area.getY(), masterX - 9, area.getHeight());
    layoutContent();
}

void MixerView::meterTick (double dt)
{
    if (! isVisible())
        return;

    auto& engine = controller.getEngine();

    for (auto& strip : row->strips)
    {
        const int index = controller.engineIndexFor (strip->getChannelId());

        if (index >= 0)
            strip->pushLevels (engine.readChannelLevels (index), dt);
    }

    master.pushLevels (engine.readMasterLevels (mix), dt);
}
}
