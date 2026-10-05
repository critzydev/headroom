// FxDrawer: the 400 px panel that slides over the mixer with one channel's effect chain.
#pragma once

#include "Juce.h"
#include "app/AppController.h"
#include "ui/fx/AddEffectPanel.h"
#include "ui/fx/SlotList.h"
#include "ui/widgets/Buttons.h"
#include "ui/widgets/Dropdown.h"
#include "ui/widgets/Meter.h"
#include "ui/widgets/ScrollView.h"

#include <functional>
#include <memory>

namespace hr
{
class FxDrawer final : public juce::Component
{
public:
    FxDrawer (AppController& controller, MeterClock& clock);
    ~FxDrawer() override;

    void open (const juce::String& channelId);
    const juce::String& getChannelId() const noexcept   { return channelId; }

    // Model changed: refresh rows, or ask to close if the channel is gone.
    void syncFromModel();

    // A slot is about to disappear: tear down its panel before the processor goes.
    void dropPanelFor (const juce::String& slotUid)     { list.dropPanelFor (slotUid); }

    void pluginListChanged()                            { addPanel.refresh(); }
    void expandBuiltin (const juce::String& builtinId);     // first slot of that effect (screenshots, new slots)
    void collapseAll()                                  { list.collapseAll(); }

    // Empties the list (and with it every panel that points into a live processor). Called whenever the drawer closes.
    void clear();
    void showAddPanel (bool show);
    bool isAddPanelOpen() const noexcept                { return addPanelOpen; }

    std::function<void()> onClose;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    static constexpr int kHeaderHeight = 56, kPresetRowHeight = 48, kFooterHeight = 64;

private:
    class DismissCatcher;

    void showPresetMenu (Dropdown& source);
    void refreshPresetLabel();
    void scrollToRow (SlotRow& row);
    bool hasPresetRow() const noexcept                  { return channelId == values::micChannelId; }

    AppController& controller;
    juce::String channelId;

    IconButton closeButton { "x", 14.0f };
    Dropdown presetDropdown;
    SlotList list;
    ScrollView viewport { false, 56 };
    juce::TextButton addButton { "Add effect" };
    AddEffectPanel addPanel;
    std::unique_ptr<DismissCatcher> catcher;
    bool addPanelOpen = false;

    juce::String title;
    juce::Colour dotColour;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxDrawer)
};
}
