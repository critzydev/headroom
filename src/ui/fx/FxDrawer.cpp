// FxDrawer implementation.
#include "ui/fx/FxDrawer.h"

#include "engine/builtin/Presets.h"
#include "ui/Focus.h"
#include "ui/HeadroomLookAndFeel.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

namespace hr
{
namespace
{
    // Two-line preset entry: name, then what it does.
    class PresetItem final : public juce::PopupMenu::CustomComponent
    {
    public:
        PresetItem (juce::String n, juce::String d, bool isCurrent) : name (std::move (n)), description (std::move (d)), current (isCurrent) {}

        void getIdealSize (int& width, int& height) override
        {
            width = (int) std::ceil (juce::jmax (theme::textWidth (theme::sans (13.0f, theme::Sans::Medium), name),
                                                  theme::textWidth (theme::sans (11.0f), description))) + 28;
            height = 46;
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().reduced (0, 1).toFloat();

            if (isItemHighlighted())
            {
                g.setColour (theme::bg3);
                g.fillRoundedRectangle (r, 4.0f);
            }

            if (current)
                Icons::draw (g, "check", juce::Point<float> (r.getRight() - 17.0f, r.getCentreY()), 14.0f, theme::accent);

            r = r.reduced (10.0f, 0.0f);
            r.removeFromRight (current ? 20.0f : 0.0f);
            g.setColour (theme::text0);
            g.setFont (theme::sans (13.0f, theme::Sans::Medium));
            g.drawText (name, r.removeFromTop (r.getHeight() * 0.5f + 3.0f).withTrimmedTop (5.0f), juce::Justification::bottomLeft, true);
            g.setColour (theme::text2);
            g.setFont (theme::sans (11.0f));
            g.drawText (description, r.withTrimmedBottom (4.0f), juce::Justification::topLeft, true);
        }

    private:
        juce::String name, description;
        bool current;
    };
}

// Swallows clicks outside the add panel so a click anywhere else closes it.
class FxDrawer::DismissCatcher final : public juce::Component
{
public:
    explicit DismissCatcher (FxDrawer& o) : owner (o) {}
    void mouseDown (const juce::MouseEvent&) override   { owner.showAddPanel (false); }

private:
    FxDrawer& owner;
};

FxDrawer::FxDrawer (AppController& c, MeterClock& clock)
    : controller (c), list (c, clock), addPanel (c)
{
    setOpaque (true);
    setWantsKeyboardFocus (true);
    setTitle ("Effects");

    closeButton.setTooltip ("Close (Esc)");
    closeButton.onClick = [this] { if (onClose) onClose(); };
    addAndMakeVisible (closeButton);

    presetDropdown.setPlaceholder ("Preset");
    presetDropdown.onOpenRequested = [this] (Dropdown& source) { showPresetMenu (source); };
    addChildComponent (presetDropdown);

    viewport.setViewedComponent (&list, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (10);
    viewport.setWantsKeyboardFocus (false);
    addAndMakeVisible (viewport);

    list.onContentChanged = [this] { viewport.repaint(); };

    addButton.getProperties().set (style::button, "primary");
    addButton.getProperties().set (style::icon, "plus");
    addButton.setMouseClickGrabsKeyboardFocus (false);
    addButton.onClick = [this] { showAddPanel (! addPanelOpen); };
    addAndMakeVisible (addButton);

    catcher = std::make_unique<DismissCatcher> (*this);
    addChildComponent (*catcher);

    addPanel.onDismiss = [this] { showAddPanel (false); };
    addPanel.onPickBuiltin = [this] (const juce::String& id)
    {
        const auto uid = controller.addBuiltinFx (channelId, id);
        showAddPanel (false);
        list.sync();

        if (auto* row = list.findRow (uid))
        {
            row->setExpanded (true);
            scrollToRow (*row);
        }
    };
    addPanel.onPickPlugin = [this] (const juce::PluginDescription& description)
    {
        const auto uid = controller.addPluginFx (channelId, description);
        showAddPanel (false);
        list.sync();

        if (auto* row = list.findRow (uid))
            scrollToRow (*row);
    };
    addChildComponent (addPanel);
}

FxDrawer::~FxDrawer()
{
    viewport.setViewedComponent (nullptr, false);
}

void FxDrawer::open (const juce::String& id)
{
    showAddPanel (false);
    channelId = id;

    const auto info = AppState::describe (controller.getState().findChannel (id));
    title = info.name + " " + theme::glyph::emDash + " Effects";
    dotColour = theme::colourFromHex (info.colour);
    presetDropdown.setVisible (hasPresetRow());

    refreshPresetLabel();
    list.setChannel (id);
    viewport.setViewPosition (0, 0);
    resized();
    repaint();
}

void FxDrawer::clear()
{
    showAddPanel (false);
    channelId = {};
    list.setChannel ({});
}

void FxDrawer::syncFromModel()
{
    const auto channel = controller.getState().findChannel (channelId);

    if (! channel.isValid())
    {
        if (onClose)
            onClose();

        return;
    }

    const auto info = AppState::describe (channel);
    const auto newTitle = info.name + " " + theme::glyph::emDash + " Effects";

    if (newTitle != title || theme::colourFromHex (info.colour) != dotColour)
    {
        title = newTitle;
        dotColour = theme::colourFromHex (info.colour);
        repaint (getLocalBounds().withHeight (kHeaderHeight));
    }

    refreshPresetLabel();
    list.sync();

    if (addPanelOpen)
        addPanel.refresh();
}

void FxDrawer::refreshPresetLabel()
{
    // The applied preset's name; "Custom" once effects were added, removed or reordered afterwards.
    const auto id = controller.getState().getSetting (ids::micPreset).toString();
    const auto* preset = findMicPreset (id);

    Dropdown::Item item;
    item.key = id.isEmpty() ? juce::String ("custom") : id;
    item.text = preset != nullptr ? preset->displayName : (id == "custom" ? juce::String ("Custom") : juce::String ("Preset"));

    presetDropdown.setItems ({ item });
    presetDropdown.setSelectedKey (item.key);
    presetDropdown.setTooltip (id == "custom" ? "You have changed the effects. Pick a preset to start from a ready-made set again."
                                              : "Replace the effects on your mic with a ready-made set");
}

void FxDrawer::expandBuiltin (const juce::String& builtinId)
{
    if (auto* row = list.findFirstBuiltin (builtinId))
    {
        row->setExpanded (true);
        scrollToRow (*row);
    }
}

void FxDrawer::scrollToRow (SlotRow& row)
{
    const int maxY = juce::jmax (0, list.getHeight() - viewport.getMaximumVisibleHeight());
    viewport.setViewPosition (0, juce::jlimit (0, maxY, row.getY()));
}

void FxDrawer::showAddPanel (bool show)
{
    if (show == addPanelOpen)
        return;

    addPanelOpen = show;
    catcher->setVisible (show);
    addPanel.setVisible (show);

    if (show)
    {
        addPanel.toFront (false);
        addPanel.open();            // focuses the search field
    }
    else
    {
        focusIfShowing (*this);
    }

    addButton.setToggleState (show, juce::dontSendNotification);
}

void FxDrawer::showPresetMenu (Dropdown& source)
{
    juce::PopupMenu menu;
    int id = 1;
    const auto current = controller.getState().getSetting (ids::micPreset).toString();

    for (const auto& preset : micPresets())
        menu.addCustomItem (id++, std::make_unique<PresetItem> (preset.displayName, preset.description, preset.id == current),
                            nullptr, preset.displayName);

    juce::Component::SafePointer<FxDrawer> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&source).withMinimumWidth (source.getWidth()),
                        [safe] (int result)
                        {
                            if (safe == nullptr || result <= 0 || result > (int) micPresets().size())
                                return;

                            safe->controller.applyMicPreset (micPresets()[(size_t) result - 1].id);
                            safe->list.sync();
                        });
}

void FxDrawer::resized()
{
    auto area = getLocalBounds();

    auto header = area.removeFromTop (kHeaderHeight);
    closeButton.setBounds (header.getRight() - theme::kSidePadding - 28 + 4, (kHeaderHeight - 28) / 2, 28, 28);

    if (hasPresetRow())
        presetDropdown.setBounds (area.removeFromTop (kPresetRowHeight).reduced (theme::kSidePadding, 8));

    auto footer = area.removeFromBottom (kFooterHeight);
    addButton.setBounds (footer.withTrimmedLeft (theme::kSidePadding).withTrimmedRight (theme::kSidePadding).withSizeKeepingCentre (footer.getWidth() - 2 * theme::kSidePadding, 32));

    viewport.setBounds (area.withTrimmedTop (1));
    list.setMinimumHeight (viewport.getMaximumVisibleHeight());
    list.setSize (viewport.getMaximumVisibleWidth(), list.getHeight());

    catcher->setBounds (getLocalBounds().withTrimmedTop (kHeaderHeight));

    const int panelW = getWidth() - 2 * theme::kSidePadding;
    const int available = footer.getY() - 8 - (kHeaderHeight + 8);
    const int panelH = juce::jmin (AddEffectPanel::kMaxHeight, available);
    addPanel.setBounds (theme::kSidePadding, footer.getY() - 8 - panelH, panelW, panelH);
}

void FxDrawer::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg1);

    // Header: colour dot, "Mic - Effects".
    g.setColour (dotColour);
    g.fillEllipse (juce::Rectangle<float> (10.0f, 10.0f).withCentre ({ (float) theme::kSidePadding + 5.0f, (float) kHeaderHeight * 0.5f }));

    g.setColour (theme::text0);
    g.setFont (theme::sans (15.0f, theme::Sans::SemiBold));
    g.drawText (title, juce::Rectangle<int> (theme::kSidePadding + 10 + 10, 0, getWidth() - 120, kHeaderHeight), juce::Justification::centredLeft, true);

    g.setColour (theme::line);
    const int headerBottom = kHeaderHeight + (hasPresetRow() ? kPresetRowHeight : 0);
    g.fillRect (0, headerBottom, getWidth(), 1);
    g.fillRect (0, getHeight() - kFooterHeight, getWidth(), 1);
    g.fillRect (0, 0, 1, getHeight());      // the drawer's left edge
}

bool FxDrawer::keyPressed (const juce::KeyPress& key)
{
    if (key.getKeyCode() == juce::KeyPress::escapeKey)
    {
        if (addPanelOpen)
            showAddPanel (false);
        else if (onClose)
            onClose();

        return true;
    }

    return false;
}
}
