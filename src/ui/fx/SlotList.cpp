// SlotRow / SlotList implementation.
#include "ui/fx/SlotList.h"

#include "ui/HeadroomLookAndFeel.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/widgets/HoverFade.h"

#include <algorithm>

namespace hr
{
// ---------------------------------------------------------------------------------------------------
// SlotRow
// ---------------------------------------------------------------------------------------------------

SlotRow::SlotRow (AppController& c, const juce::String& channel, const SlotInfo& slot)
    : controller (c), channelId (channel), uid (slot.uid), info (slot), power (LedToggle::Style::power)
{
    setTitle (slot.name);
    HoverFade::attach (*this);

    power.onClick = [this] { controller.setFxBypassed (channelId, uid, ! power.getToggleState()); };
    addAndMakeVisible (power);

    chevron.setTooltip ("Show the controls");
    chevron.onClick = [this] { setExpanded (! expanded); };
    addChildComponent (chevron);

    openButton.setMouseClickGrabsKeyboardFocus (false);
    openButton.setTooltip ("Open this plugin's own window");
    openButton.onClick = [this] { controller.openFxEditor (channelId, uid); };
    addChildComponent (openButton);

    reenableButton.setMouseClickGrabsKeyboardFocus (false);
    reenableButton.getProperties().set (style::button, "primary");
    reenableButton.onClick = [this] { controller.reenableFx (uid); };
    addChildComponent (reenableButton);
    updateFormatTag();
    sync (slot);
}

SlotRow::~SlotRow()
{
    panel.reset();
    panelKeepAlive.reset();
}

void SlotRow::updateFormatTag()
{
    if (info.isBuiltin())
    {
        formatTag = "BUILT-IN";
        return;
    }

    formatTag = "PLUGIN";

    if (auto xml = juce::XmlDocument::parse (info.descXml))
    {
        juce::PluginDescription description;

        if (description.loadFromXml (*xml) && description.pluginFormatName.isNotEmpty())
            formatTag = description.pluginFormatName.toUpperCase();
    }
}

bool SlotRow::hasLoadError() const
{
    return ! info.disabled && controller.getFxProcessor (uid) == nullptr;
}

bool SlotRow::canExpand() const
{
    return info.isBuiltin() && ! info.disabled && controller.getFxProcessor (uid) != nullptr;
}

void SlotRow::sync (const SlotInfo& slot)
{
    const bool typeChanged = slot.descXml != info.descXml;
    info = slot;

    if (typeChanged)
        updateFormatTag();

    errorText = hasLoadError() ? controller.getFxError (uid) : juce::String();

    power.setToggleState (! info.bypassed && ! info.disabled, juce::dontSendNotification);
    power.setEnabled (! info.disabled && ! hasLoadError());
    power.setTooltip (info.bypassed ? "Turn this effect on" : "Turn this effect off");

    const bool usable = ! info.disabled && ! hasLoadError();
    chevron.setVisible (info.isBuiltin() && usable);
    openButton.setVisible (! info.isBuiltin() && usable);
    reenableButton.setVisible (info.disabled);
    chevron.setIconName (expanded ? "chevron-down" : "chevron-right");

    if (hasLoadError() && errorText.isNotEmpty())
        setTooltip (errorText);
    else if (info.isBuiltin() && info.builtinId == "noise")
        setTooltip ("Adds about 30 to 50 ms of delay to this channel, depending on the mode.");
    else
        setTooltip ({});

    updatePanel();
    resized();
    repaint();
}

void SlotRow::setExpanded (bool shouldExpand)
{
    if (shouldExpand == expanded || (shouldExpand && ! canExpand()))
        return;

    expanded = shouldExpand;
    chevron.setIconName (expanded ? "chevron-down" : "chevron-right");
    chevron.setTooltip (expanded ? "Hide the controls" : "Show the controls");
    updatePanel();
    resized();

    if (onLayoutChanged)
        onLayoutChanged();
}

void SlotRow::updatePanel()
{
    if (expanded && canExpand())
    {
        auto shared = controller.getFxProcessorShared (uid);
        auto* processor = dynamic_cast<BuiltinProcessor*> (shared.get());

        if (panel == nullptr || panelProcessor != processor)
        {
            panel.reset();
            panelProcessor = processor;
            panelKeepAlive = shared;

            if (processor != nullptr)
            {
                panel = createBuiltinPanel (*processor);

                if (panel != nullptr)
                {
                    addAndMakeVisible (*panel);
                    panel->onHeightChanged = [this]
                    {
                        resized();

                        if (onLayoutChanged)
                            onLayoutChanged();
                    };
                }
            }
        }

        if (panel != nullptr)
            panel->setDimmed (info.bypassed);
    }
    else
    {
        panel.reset();
        panelProcessor = nullptr;
        panelKeepAlive.reset();

        if (expanded && ! canExpand())
            expanded = false;
    }
}

void SlotRow::dropPanel()
{
    panel.reset();
    panelProcessor = nullptr;
    panelKeepAlive.reset();
}

int SlotRow::getPreferredHeight() const
{
    int h = kHeaderHeight;

    if (info.disabled || hasLoadError())
        h += kNoteHeight;

    if (panel != nullptr)
        h += panel->getPreferredHeight() + 8;

    return h;
}

void SlotRow::tickMeter (double dt)
{
    if (panel != nullptr && controller.getFxProcessor (uid) == panelProcessor)
        panel->tickMeter (dt);
}

void SlotRow::resized()
{
    const int w = getWidth();

    gripArea = { 8, 0, 24, kHeaderHeight };
    power.setBounds (32, (kHeaderHeight - 28) / 2, 28, 28);

    int right = w - 8;

    if (chevron.isVisible())
    {
        chevron.setBounds (right - 28, (kHeaderHeight - 28) / 2, 28, 28);
        right -= 28;
    }
    else if (openButton.isVisible())
    {
        openButton.setBounds (w - 16 - 56, (kHeaderHeight - 24) / 2, 56, 24);
        right = w - 16 - 56;
    }
    else
    {
        right = w - 16 + 8;
    }

    const float tagWidth = theme::textWidth (theme::caps (9.0f), formatTag);
    tagArea = juce::Rectangle<int> (right - 8 - (int) std::ceil (tagWidth) - 2, 0, (int) std::ceil (tagWidth) + 2, kHeaderHeight);
    nameArea = juce::Rectangle<int> (68, 0, juce::jmax (20, tagArea.getX() - 8 - 68), kHeaderHeight);

    if (reenableButton.isVisible())
    {
        const int bw = getLookAndFeel().getTextButtonWidthToFitText (reenableButton, 24);
        reenableButton.setBounds (w - 16 - bw, kHeaderHeight + (kNoteHeight - 24) / 2, bw, 24);
    }

    if (panel != nullptr)
    {
        const int top = kHeaderHeight + ((info.disabled || hasLoadError()) ? kNoteHeight : 0);
        panel->setBounds (16, top, BuiltinPanel::kWidth, panel->getPreferredHeight());
    }
}

void SlotRow::paint (juce::Graphics& g)
{
    g.setColour (theme::bg1);
    g.fillRect (getLocalBounds());

    const float fade = draggingHandle ? 0.0f : HoverFade::get (*this, hover);

    if (fade > 0.0f)
    {
        g.setColour (theme::bg2.withAlpha (0.55f * fade));
        g.fillRect (0, 0, getWidth(), kHeaderHeight);
    }

    g.setColour (theme::line);
    g.fillRect (0, getHeight() - 1, getWidth(), 1);

    const bool off = info.disabled || hasLoadError();

    Icons::draw (g, "grip-vertical", gripArea.toFloat(), 16.0f, hover || draggingHandle ? theme::text1 : theme::text2);

    g.setColour (off ? theme::text2 : (info.bypassed ? theme::text1 : theme::text0));
    g.setFont (theme::sans (13.0f, theme::Sans::Medium));
    g.drawText (info.name, nameArea, juce::Justification::centredLeft, true);

    g.setColour (theme::text2);
    g.setFont (theme::caps (9.0f));
    g.drawText (formatTag, tagArea, juce::Justification::centredRight, false);

    if (off)
    {
        g.setColour (theme::bad);
        g.setFont (theme::sans (12.0f));
        g.drawText (info.disabled ? juce::String ("Turned off because it crashed.") : juce::String ("Couldn" + theme::glyph::quoteClose + "t be loaded."),
                    juce::Rectangle<int> (68, kHeaderHeight, getWidth() - 68 - (info.disabled ? 130 : 16), kNoteHeight),
                    juce::Justification::centredLeft, true);
    }
}

void SlotRow::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        showMenu();
        return;
    }

    if (gripArea.contains (e.getPosition()) && e.y < kHeaderHeight)
    {
        draggingHandle = true;
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);

        if (onDragBegin)
            onDragBegin (*this);

        repaint();
    }
}

void SlotRow::mouseDrag (const juce::MouseEvent& e)
{
    if (draggingHandle && onDragMove)
        onDragMove (*this, e);
}

void SlotRow::mouseUp (const juce::MouseEvent& e)
{
    if (draggingHandle)
    {
        draggingHandle = false;
        setMouseCursor (juce::MouseCursor::NormalCursor);

        if (onDragEnd)
            onDragEnd (*this);

        repaint();
        return;
    }

    // A click on the name row folds the panel open and closed, like the chevron.
    if (! e.mods.isPopupMenu() && ! e.mouseWasDraggedSinceMouseDown() && e.eventComponent == this
        && nameArea.contains (e.getPosition()) && canExpand())
        setExpanded (! expanded);
}

void SlotRow::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (e.eventComponent == this && nameArea.contains (e.getPosition()) && ! info.isBuiltin() && ! info.disabled)
        controller.openFxEditor (channelId, uid);
}

void SlotRow::showMenu()
{
    juce::PopupMenu menu;
    menu.addItem (1, "Remove effect");

    juce::Component::SafePointer<SlotRow> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options().withStandardItemHeight (28).withMinimumWidth (160),
                        [safe] (int result)
                        {
                            if (safe != nullptr && result == 1)
                                safe->controller.removeFx (safe->channelId, safe->uid);
                        });
}

// ---------------------------------------------------------------------------------------------------
// SlotList
// ---------------------------------------------------------------------------------------------------

SlotList::SlotList (AppController& c, MeterClock& meterClock) : controller (c), clock (meterClock)
{
    clock.addListener (this);
}

SlotList::~SlotList()
{
    clock.removeListener (this);
}

void SlotList::setChannel (const juce::String& id)
{
    if (id != channelId)
    {
        rows.clear();
        removeAllChildren();
        channelId = id;
    }

    emptyText = id == values::micChannelId ? "No effects on your mic. Pick a preset above or add one below."
                                           : "No effects yet. Add one below.";
    sync();
}

void SlotList::sync()
{
    const auto channel = controller.getState().findChannel (channelId);
    std::vector<SlotInfo> infos;

    for (auto slotTree : AppState::getFx (channel))
        infos.push_back (AppState::describeSlot (slotTree));

    std::vector<std::unique_ptr<SlotRow>> next;

    for (const auto& info : infos)
    {
        std::unique_ptr<SlotRow> row;

        for (auto& candidate : rows)
            if (candidate != nullptr && candidate->getUid() == info.uid)
                row = std::move (candidate);

        if (row == nullptr)
        {
            row = std::make_unique<SlotRow> (controller, channelId, info);
            row->onLayoutChanged = [this, r = row.get()]
            {
                if (r->isExpanded())    expandedSlots.insert (r->getUid());
                else                    expandedSlots.erase (r->getUid());

                layoutRows();
            };
            row->onDragBegin = [this] (SlotRow& r) { beginDrag (r); };
            row->onDragMove = [this] (SlotRow& r, const juce::MouseEvent& e) { moveDrag (r, e); };
            row->onDragEnd = [this] (SlotRow& r) { endDrag (r); };
            addAndMakeVisible (*row);

            if (expandedSlots.count (info.uid) != 0)
                row->setExpanded (true);
        }
        else
        {
            row->sync (info);
        }

        next.push_back (std::move (row));
    }

    for (auto& gone : rows)
        if (gone != nullptr)
        {
            if (gone.get() == dragged)
                dragged = nullptr;

            removeChildComponent (gone.get());
        }

    rows = std::move (next);
    layoutRows();
    repaint();
}

SlotRow* SlotList::findRow (const juce::String& slotUid) const
{
    for (auto& row : rows)
        if (row->getUid() == slotUid)
            return row.get();

    return nullptr;
}

SlotRow* SlotList::findFirstBuiltin (const juce::String& builtinId) const
{
    for (auto& row : rows)
        if (row->getInfo().isBuiltin() && row->getInfo().builtinId == builtinId)
            return row.get();

    return nullptr;
}

void SlotList::collapseAll()
{
    for (auto& row : rows)
        row->setExpanded (false);
}

void SlotList::dropPanelFor (const juce::String& slotUid)
{
    if (auto* row = findRow (slotUid))
        row->dropPanel();
}

int SlotList::getPreferredHeight() const
{
    int h = 0;

    for (auto& row : rows)
        h += row->getPreferredHeight();

    return juce::jmax (h, minimumHeight);
}

void SlotList::layoutRows()
{
    int y = 0;

    for (auto& row : rows)
    {
        const int h = row->getPreferredHeight();

        if (row.get() != dragged)
            row->setBounds (0, y, getWidth(), h);

        y += h;
    }

    const int wanted = juce::jmax (y, minimumHeight);

    if (wanted != getHeight())
    {
        setSize (getWidth(), wanted);

        if (onContentChanged)
            onContentChanged();
    }
}

void SlotList::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg1);

    if (rows.empty())
    {
        g.setColour (theme::text1);
        g.setFont (theme::sans (13.0f));
        g.drawFittedText (emptyText, juce::Rectangle<int> (32, 28, getWidth() - 64, 60), juce::Justification::centredTop, 3);
    }
}

void SlotList::paintOverChildren (juce::Graphics& g)
{
    if (dragged == nullptr || dropIndex < 0)
        return;

    int y = 0, index = 0;

    for (auto& row : rows)
    {
        if (row.get() == dragged)
            continue;

        if (index == dropIndex)
            break;

        y += row->getHeight();
        ++index;
    }

    g.setColour (theme::accent);
    g.fillRect (0, juce::jmax (0, y - 1), getWidth(), 2);
}

void SlotList::meterTick (double dt)
{
    if (! isVisible())
        return;

    for (auto& row : rows)
        row->tickMeter (dt);
}

void SlotList::beginDrag (SlotRow& row)
{
    dragged = &row;
    dragOffsetY = 0;
    dropIndex = -1;
    dragOriginIndex = -1;
    int index = 0;

    for (auto& r : rows)
    {
        if (r.get() == &row)
            dragOriginIndex = index;

        ++index;
    }

    row.toFront (false);
}

int SlotList::computeDropIndex (const SlotRow& row) const
{
    // Judged by the pointer, not by the dragged row's own centre: an expanded row is tall and its centre would lag far behind.
    const int centre = dragCursorY;
    int index = 0;

    for (auto& other : rows)
    {
        if (other.get() == &row)
            continue;

        if (other->getBounds().getCentreY() < centre)
            ++index;
    }

    return index;
}

void SlotList::moveDrag (SlotRow& row, const juce::MouseEvent& e)
{
    const auto inList = e.getEventRelativeTo (this);

    if (dragOffsetY == 0)
        dragOffsetY = e.getMouseDownPosition().y;

    const int top = juce::jlimit (0, juce::jmax (0, getHeight() - SlotRow::kHeaderHeight), (int) inList.position.y - dragOffsetY);
    dragCursorY = (int) inList.position.y;
    row.setTopLeftPosition (0, top);
    dropIndex = computeDropIndex (row);
    repaint();
}

void SlotList::endDrag (SlotRow& row)
{
    const int target = dropIndex;
    const int origin = dragOriginIndex;
    dragged = nullptr;
    dropIndex = -1;

    layoutRows();
    repaint();

    if (target >= 0 && target != origin)
        controller.moveFx (channelId, row.getUid(), target);
}
}
