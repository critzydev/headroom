// SlotRow / SlotList: the effect rows of the FX drawer (drag handle, power, name, format tag, inline panel).
#pragma once

#include "Juce.h"
#include "app/AppController.h"
#include "ui/fx/BuiltinPanel.h"
#include "ui/widgets/Buttons.h"
#include "ui/widgets/Meter.h"

#include <functional>
#include <memory>
#include <set>
#include <vector>

namespace hr
{
class SlotRow final : public juce::Component,
                      public juce::SettableTooltipClient
{
public:
    static constexpr int kHeaderHeight = 44;
    static constexpr int kNoteHeight = 32;

    SlotRow (AppController& controller, const juce::String& channelId, const SlotInfo& slot);
    ~SlotRow() override;

    const juce::String& getUid() const noexcept         { return uid; }
    const SlotInfo& getInfo() const noexcept            { return info; }
    juce::Point<int> getGripCentre() const noexcept     { return gripArea.getCentre(); }

    void sync (const SlotInfo& slot);
    bool isExpanded() const noexcept                    { return expanded; }
    void setExpanded (bool shouldExpand);
    int getPreferredHeight() const;

    void tickMeter (double dt);
    void dropPanel();                                   // the processor behind it is going away

    std::function<void()> onLayoutChanged;
    std::function<void (SlotRow&)> onDragBegin;
    std::function<void (SlotRow&, const juce::MouseEvent&)> onDragMove;
    std::function<void (SlotRow&)> onDragEnd;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseEnter (const juce::MouseEvent&) override  { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override   { hover = false; repaint(); }
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    bool isDisabledSlot() const noexcept                { return info.disabled; }
    bool hasLoadError() const;
    bool canExpand() const;
    void showMenu();
    void updatePanel();
    void updateFormatTag();

    AppController& controller;
    const juce::String channelId, uid;
    SlotInfo info;
    juce::String formatTag, errorText;
    bool expanded = false, hover = false, draggingHandle = false;

    LedToggle power;
    IconButton chevron { "chevron-right", 16.0f };
    juce::TextButton openButton { "Open" };
    juce::TextButton reenableButton { "Turn back on" };

    // The panel points into the processor, so it also keeps it alive; declared first so it is released last.
    std::shared_ptr<juce::AudioProcessor> panelKeepAlive;
    std::unique_ptr<BuiltinPanel> panel;
    juce::AudioProcessor* panelProcessor = nullptr;

    juce::Rectangle<int> gripArea, nameArea, tagArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlotRow)
};

class SlotList final : public juce::Component,
                       private MeterClock::Listener
{
public:
    SlotList (AppController& controller, MeterClock& clock);
    ~SlotList() override;

    void setChannel (const juce::String& id);
    void sync();
    SlotRow* findRow (const juce::String& slotUid) const;
    SlotRow* findFirstBuiltin (const juce::String& builtinId) const;
    void dropPanelFor (const juce::String& slotUid);
    void collapseAll();

    int getPreferredHeight() const;
    void setMinimumHeight (int h)               { minimumHeight = h; layoutRows(); }
    std::function<void()> onContentChanged;     // height changed: the drawer updates its scrollbar

    void layoutRows();
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override                     { layoutRows(); }

private:
    void meterTick (double dt) override;
    void beginDrag (SlotRow& row);
    void moveDrag (SlotRow& row, const juce::MouseEvent& e);
    void endDrag (SlotRow& row);
    int computeDropIndex (const SlotRow& dragged) const;

    AppController& controller;
    MeterClock& clock;
    juce::String channelId;
    std::vector<std::unique_ptr<SlotRow>> rows;
    juce::String emptyText;
    std::set<juce::String> expandedSlots;       // remembered across closing the drawer
    int minimumHeight = 0;

    SlotRow* dragged = nullptr;
    int dragOffsetY = 0, dragCursorY = 0, dropIndex = -1, dragOriginIndex = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlotList)
};
}
