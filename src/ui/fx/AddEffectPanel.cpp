// AddEffectPanel implementation.
#include "ui/fx/AddEffectPanel.h"

#include "engine/builtin/BuiltinFx.h"
#include "ui/Focus.h"
#include "ui/HeadroomLookAndFeel.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

#include <map>

namespace hr
{
// A text field that hands Up / Down / Enter to the panel instead of moving the caret.
class AddEffectPanel::SearchField final : public juce::TextEditor
{
public:
    std::function<void (int)> onMove;
    std::function<void()> onEnter;

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key.getKeyCode() == juce::KeyPress::downKey)    { if (onMove) onMove (1);   return true; }
        if (key.getKeyCode() == juce::KeyPress::upKey)      { if (onMove) onMove (-1);  return true; }

        return juce::TextEditor::keyPressed (key);
    }
};

// The painted list. One component, no per-row children: a few hundred plugins cost nothing to show.
class AddEffectPanel::List final : public juce::Component
{
public:
    explicit List (AddEffectPanel& o) : owner (o) {}

    int selected = -1, hovered = -1;

    int getTotalHeight() const
    {
        int h = 4;

        for (auto& e : owner.entries)
            h += e.height;

        return h + 4;
    }

    juce::Rectangle<int> rowBounds (int index) const
    {
        int y = 4;

        for (int i = 0; i < index; ++i)
            y += owner.entries[(size_t) i].height;

        return { 0, y, getWidth(), owner.entries[(size_t) index].height };
    }

    int indexAt (int y) const
    {
        int top = 4;

        for (size_t i = 0; i < owner.entries.size(); ++i)
        {
            if (y >= top && y < top + owner.entries[i].height)
                return (int) i;

            top += owner.entries[i].height;
        }

        return -1;
    }

    void scrollIntoView (int index)
    {
        if (auto* vp = findParentComponentOfClass<juce::Viewport>())
        {
            const auto r = rowBounds (index);
            const int viewTop = vp->getViewPositionY(), viewH = vp->getMaximumVisibleHeight();

            if (r.getY() < viewTop)
                vp->setViewPosition (0, r.getY() - 4);
            else if (r.getBottom() > viewTop + viewH)
                vp->setViewPosition (0, r.getBottom() - viewH + 4);
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto clip = g.getClipBounds();
        int y = 4;

        for (size_t i = 0; i < owner.entries.size(); ++i)
        {
            const auto& e = owner.entries[i];
            const juce::Rectangle<int> r (0, y, getWidth(), e.height);
            y += e.height;

            if (! r.intersects (clip))
                continue;

            switch (e.type)
            {
                case Entry::Type::header:
                    g.setColour (theme::text2);
                    g.setFont (theme::caps (10.0f));
                    g.drawText (e.text.toUpperCase(), r.withTrimmedLeft (16).withTrimmedTop (8), juce::Justification::centredLeft, false);
                    break;

                case Entry::Type::vendor:
                    g.setColour (theme::text1);
                    g.setFont (theme::sans (11.0f, theme::Sans::Medium));
                    g.drawText (e.text, r.withTrimmedLeft (16).withTrimmedTop (4), juce::Justification::centredLeft, true);
                    break;

                case Entry::Type::note:
                    g.setColour (theme::text2);
                    g.setFont (theme::sans (12.0f));
                    g.drawFittedText (e.text, r.reduced (16, 6), juce::Justification::topLeft, 3);
                    break;

                case Entry::Type::builtin:
                case Entry::Type::plugin:
                {
                    const bool highlight = (int) i == selected || (int) i == hovered;

                    if (highlight)
                    {
                        g.setColour (theme::bg3);
                        g.fillRoundedRectangle (r.reduced (6, 1).toFloat(), 5.0f);
                    }

                    const bool builtin = e.type == Entry::Type::builtin;
                    auto text = r.withTrimmedLeft (16).withTrimmedRight (16);

                    if (! e.tag.isEmpty())
                    {
                        const int tagW = (int) std::ceil (theme::textWidth (theme::caps (9.0f), e.tag)) + 2;
                        g.setColour (theme::text2);
                        g.setFont (theme::caps (9.0f));
                        g.drawText (e.tag, text.removeFromRight (tagW), juce::Justification::centredRight, false);
                        text.removeFromRight (8);
                    }

                    g.setColour (theme::text0);
                    g.setFont (theme::sans (13.0f, theme::Sans::Medium));

                    if (builtin)
                    {
                        g.drawText (e.text, text.removeFromTop (r.getHeight() / 2 + 2).withTrimmedTop (6), juce::Justification::bottomLeft, true);
                        g.setColour (theme::text2);
                        g.setFont (theme::sans (11.0f));
                        g.drawText (e.sub, text.withTrimmedBottom (4), juce::Justification::topLeft, true);
                    }
                    else
                    {
                        g.drawText (e.text, text, juce::Justification::centredLeft, true);
                    }

                    break;
                }
            }
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int i = indexAt (e.y);
        const int h = i >= 0 && owner.entries[(size_t) i].selectable() ? i : -1;

        if (h != hovered)
        {
            hovered = h;
            repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        hovered = -1;
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        const int i = indexAt (e.y);

        if (i >= 0 && owner.entries[(size_t) i].selectable() && ! e.mouseWasDraggedSinceMouseDown())
            owner.pick (i);
    }

private:
    AddEffectPanel& owner;
};

AddEffectPanel::AddEffectPanel (AppController& c) : controller (c)
{
    search = std::make_unique<SearchField>();
    search->setFont (theme::sans (13.0f));
    search->setJustification (juce::Justification::centredLeft);
    search->setIndents (32, 0);
    search->setBorder (juce::BorderSize<int> (0));
    search->setColour (juce::TextEditor::backgroundColourId, theme::bg0);
    search->onTextChange = [this] { rebuildEntries(); };
    search->onEscapeKey = [this] { if (onDismiss) onDismiss(); };
    search->onReturnKey = [this]
    {
        if (list->selected >= 0)
            pick (list->selected);
    };
    search->onMove = [this] (int direction) { moveSelection (direction); };
    addAndMakeVisible (*search);

    list = std::make_unique<List> (*this);
    viewport.setViewedComponent (list.get(), false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (10);
    addAndMakeVisible (viewport);

    rescanButton.setMouseClickGrabsKeyboardFocus (false);
    rescanButton.getProperties().set (style::icon, "refresh-cw");
    rescanButton.onClick = [this]
    {
        auto& plugins = controller.getPlugins();

        if (plugins.isScanning())
            plugins.stopScan();
        else
            plugins.startBackgroundScan (true);

        refresh();
    };
    addAndMakeVisible (rescanButton);

    setWantsKeyboardFocus (true);
    setFocusContainerType (FocusContainerType::keyboardFocusContainer);
    rebuildEntries();
}

AddEffectPanel::~AddEffectPanel()
{
    stopTimer();
    viewport.setViewedComponent (nullptr, false);
}

void AddEffectPanel::open()
{
    search->clear();
    rebuildEntries();
    viewport.setViewPosition (0, 0);
    wasScanning = controller.getPlugins().isScanning();

    focusIfShowing (*search);
}

void AddEffectPanel::visibilityChanged()
{
    // Poll the scan state only while the panel is on screen.
    if (isVisible())
        startTimerHz (6);
    else
        stopTimer();
}

void AddEffectPanel::timerCallback()
{
    auto& plugins = controller.getPlugins();
    const int percent = juce::roundToInt (plugins.getScanProgress() * 100.0f);

    if (plugins.isScanning() != wasScanning || (plugins.isScanning() && percent != lastProgressPercent))
    {
        wasScanning = plugins.isScanning();
        lastProgressPercent = percent;
        refresh();
    }
}

void AddEffectPanel::refresh()
{
    const bool scanning = controller.getPlugins().isScanning();
    rescanButton.setButtonText (scanning ? "Stop" : "Rescan plugins");
    rescanButton.getProperties().set (style::icon, scanning ? "x" : "refresh-cw");
    rescanButton.repaint();

    rebuildEntries();
    repaint (footerArea);
}

void AddEffectPanel::rebuildEntries()
{
    entries.clear();
    const auto filter = search != nullptr ? search->getText().trim().toLowerCase() : juce::String();

    auto matches = [&filter] (const juce::String& a, const juce::String& b = {})
    {
        return filter.isEmpty() || a.toLowerCase().contains (filter) || b.toLowerCase().contains (filter);
    };

    // Built-in effects.
    {
        std::vector<Entry> builtin;

        for (const auto& info : BuiltinFx::list())
        {
            if (! matches (info.displayName, info.description))
                continue;

            Entry e;
            e.type = Entry::Type::builtin;
            e.text = info.displayName;
            e.sub = info.description;
            e.builtinId = info.id;
            e.height = 44;
            builtin.push_back (std::move (e));
        }

        if (! builtin.empty())
        {
            Entry header;
            header.type = Entry::Type::header;
            header.text = "Built-in";
            header.height = 28;
            entries.push_back (std::move (header));

            for (auto& e : builtin)
                entries.push_back (std::move (e));
        }
    }

    // Plugins, grouped by vendor.
    {
        std::map<juce::String, std::vector<juce::PluginDescription>> byVendor;

        for (const auto& description : controller.getPlugins().getKnownEffects())
            if (matches (description.name, description.manufacturerName))
                byVendor[description.manufacturerName.isNotEmpty() ? description.manufacturerName : juce::String ("Other")].push_back (description);

        if (! byVendor.empty())
        {
            Entry header;
            header.type = Entry::Type::header;
            header.text = "Plugins";
            header.height = 28;
            entries.push_back (std::move (header));

            for (auto& [vendor, plugins] : byVendor)
            {
                std::sort (plugins.begin(), plugins.end(), [] (const juce::PluginDescription& a, const juce::PluginDescription& b)
                {
                    return a.name.compareIgnoreCase (b.name) < 0;
                });

                Entry v;
                v.type = Entry::Type::vendor;
                v.text = vendor;
                v.height = 26;
                entries.push_back (std::move (v));

                for (auto& description : plugins)
                {
                    Entry e;
                    e.type = Entry::Type::plugin;
                    e.text = description.name;
                    e.tag = description.pluginFormatName.toUpperCase();
                    e.description = description;
                    e.height = 32;
                    entries.push_back (std::move (e));
                }
            }
        }
        else if (filter.isEmpty())
        {
            Entry header;
            header.type = Entry::Type::header;
            header.text = "Plugins";
            header.height = 28;
            entries.push_back (std::move (header));

            Entry note;
            note.type = Entry::Type::note;
            note.text = "No plugins found yet. Add a folder in Settings, or rescan below.";
            note.height = 52;
            entries.push_back (std::move (note));
        }
    }

    if (entries.empty())
    {
        Entry note;
        note.type = Entry::Type::note;
        note.text = "Nothing matches " + theme::glyph::quoteOpen + search->getText().trim() + theme::glyph::quoteClose + ".";
        note.height = 44;
        entries.push_back (std::move (note));
    }

    // Keyboard selection: the first real entry while searching, none otherwise.
    list->selected = -1;
    list->hovered = -1;

    if (filter.isNotEmpty())
        for (size_t i = 0; i < entries.size(); ++i)
            if (entries[i].selectable())
            {
                list->selected = (int) i;
                break;
            }

    list->setSize (juce::jmax (1, viewport.getMaximumVisibleWidth()), list->getTotalHeight());
    list->repaint();
}

void AddEffectPanel::moveSelection (int direction)
{
    if (entries.empty())
        return;

    int i = list->selected;

    for (int step = 0; step < (int) entries.size(); ++step)
    {
        i += direction;

        if (i < 0 || i >= (int) entries.size())
            return;

        if (entries[(size_t) i].selectable())
        {
            list->selected = i;
            list->scrollIntoView (i);
            list->repaint();
            return;
        }
    }
}

void AddEffectPanel::pick (int index)
{
    if (! juce::isPositiveAndBelow (index, (int) entries.size()))
        return;

    const auto e = entries[(size_t) index];

    if (e.type == Entry::Type::builtin && onPickBuiltin)
        onPickBuiltin (e.builtinId);
    else if (e.type == Entry::Type::plugin && onPickPlugin)
        onPickPlugin (e.description);
}

bool AddEffectPanel::keyPressed (const juce::KeyPress& key)
{
    if (key.getKeyCode() == juce::KeyPress::escapeKey)
    {
        if (onDismiss)
            onDismiss();

        return true;
    }

    return false;
}

void AddEffectPanel::resized()
{
    auto area = getLocalBounds().reduced (1);
    search->setBounds (area.removeFromTop (44).reduced (8, 8));
    footerArea = area.removeFromBottom (44);
    rescanButton.setBounds (footerArea.getX() + 8, footerArea.getY() + 8, 128, 28);
    viewport.setBounds (area);
    list->setSize (juce::jmax (1, viewport.getMaximumVisibleWidth()), list->getTotalHeight());
}

void AddEffectPanel::paintFooter (juce::Graphics& g, juce::Rectangle<int> area) const
{
    auto& plugins = controller.getPlugins();
    auto text = area.withTrimmedLeft (8 + 128 + 12).withTrimmedRight (12);

    g.setFont (theme::sans (11.0f));

    if (plugins.isScanning())
    {
        const auto status = plugins.getScanStatusText();
        g.setColour (theme::text1);
        g.drawText (status.isNotEmpty() ? status : juce::String ("Scanning") + theme::glyph::ellipsis, text.withTrimmedBottom (8),
                    juce::Justification::centredLeft, true);

        const auto bar = juce::Rectangle<float> ((float) text.getX(), (float) text.getBottom() - 12.0f, (float) text.getWidth(), 2.0f);
        g.setColour (theme::bg3);
        g.fillRect (bar);
        g.setColour (theme::accent);
        g.fillRect (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, plugins.getScanProgress())));
    }
    else
    {
        const int count = plugins.getKnownEffects().size();
        g.setColour (theme::text2);
        g.drawText (count == 0 ? juce::String ("No plugins found") : juce::String (count) + (count == 1 ? " plugin found" : " plugins found"),
                    text, juce::Justification::centredRight, true);
    }
}

void AddEffectPanel::paintOverChildren (juce::Graphics& g)
{
    // The search icon and, while nothing is typed, the hint: drawn here because TextEditor hides its own hint on focus.
    const auto field = search->getBounds();
    Icons::draw (g, "search", juce::Point<float> ((float) field.getX() + 16.0f, (float) field.getCentreY()), 16.0f, theme::text2);

    if (search->isEmpty())
    {
        g.setColour (theme::text2);
        g.setFont (theme::sans (13.0f));
        g.drawText ("Search effects", field.withTrimmedLeft (32), juce::Justification::centredLeft, false);
    }
}

void AddEffectPanel::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();

    g.setColour (theme::bg2);
    g.fillRoundedRectangle (bounds, theme::kPanelRadius);

    g.setColour (theme::line);
    g.fillRect (1, footerArea.getY(), getWidth() - 2, 1);

    paintFooter (g, footerArea);

    g.setColour (theme::lineStrong);
    g.drawRoundedRectangle (bounds.reduced (0.5f), theme::kPanelRadius - 0.5f, 1.0f);
}
}
