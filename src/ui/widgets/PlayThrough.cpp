// PlayThrough: the "Play through" panel and the closed text of the control that opens it.
#include "ui/widgets/PlayThrough.h"

#include "ui/Focus.h"
#include "ui/HeadroomLookAndFeel.h"
#include "ui/Icons.h"
#include "ui/MicStatus.h"
#include "ui/Theme.h"

namespace hr::playthrough
{
namespace
{
    constexpr int kRowHeight = 28, kHeaderHeight = 28, kFooterHeight = 30, kPadding = 4;
    constexpr int kMinWidth = 288, kMaxWidth = 440;
}

// ---------------------------------------------------------------------------------------------------
// What is listed, what the closed control says
// ---------------------------------------------------------------------------------------------------

std::vector<Entry> entriesFor (AppController& controller, Mix mix)
{
    auto& state = controller.getState();
    const auto status = controller.getBackendStatus();
    const auto devices = controller.getBackend().getOutputs();
    const auto& states = mix == Mix::Monitor ? status.headphoneOutputs : status.streamOutputs;

    std::vector<Entry> list;

    if (mix == Mix::Monitor)
    {
        // "Automatic (Elgato Wave:3 Headphones)": what it means now. The devices are sorted with a Wave first, which is what it prefers.
        juce::String resolved;

        for (const auto& output : states)
            if (output.automatic && output.present)
                resolved = output.label;

        if (resolved.isEmpty() && ! devices.empty())
            resolved = devices.front().label;

        list.push_back ({ values::outputAuto, resolved.isNotEmpty() ? "Automatic (" + resolved + ")" : juce::String ("Automatic"), false });
    }

    for (const auto& device : devices)
        list.push_back ({ device.id, device.label, false });

    // A device that is ticked but not on the machine stays in the list, so that it can be unticked (and the person sees it is still there).
    for (const auto& id : state.getOutputs (mix))
    {
        if (id == values::outputAuto)
            continue;

        bool known = false;

        for (const auto& entry : list)
            known = known || entry.key == id;

        if (known)
            continue;

        juce::String label = id;

        for (const auto& output : states)
            if (output.id == id && output.label.isNotEmpty())
                label = output.label;

        list.push_back ({ id, label, true });
    }

    return list;
}

juce::String summaryFor (AppController& controller, Mix mix, int* extraDevices, bool spellOutAutomatic)
{
    const auto status = controller.getBackendStatus();
    const auto& states = mix == Mix::Monitor ? status.headphoneOutputs : status.streamOutputs;

    std::vector<const OutputState*> present;

    for (const auto& output : states)
        if (output.present)
            present.push_back (&output);

    // Headphones: the devices besides the first. Stream: every device is one besides "Headroom Stream Mix".
    if (extraDevices != nullptr)
        *extraDevices = mix == Mix::Monitor ? juce::jmax (0, (int) present.size() - 1) : (int) present.size();

    if (present.empty())
        return mix == Mix::Monitor ? juce::String ("Automatic") : juce::String ("none");

    if (present.size() == 1)
        return spellOutAutomatic && present[0]->automatic ? "Automatic (" + present[0]->label + ")" : present[0]->label;

    return shortDeviceName (present[0]->label) + " + " + juce::String ((int) present.size() - 1) + " more";
}

void refresh (Dropdown& box, AppController& controller, Mix mix, bool spellOutAutomatic)
{
    Dropdown::Item item;
    item.key = "summary";
    item.text = item.shortText = summaryFor (controller, mix, nullptr, spellOutAutomatic);

    box.setItems ({ std::move (item) });
    box.setSelectedKey ("summary");
}

void attach (Dropdown& box, AppController& controller, Mix mix, bool spellOutAutomatic)
{
    box.onOpenRequested = [&box, &controller, mix] (Dropdown&) { open (box, controller, mix); };
    refresh (box, controller, mix, spellOutAutomatic);
}

// ---------------------------------------------------------------------------------------------------
// One row: a box, the device, "not connected"
// ---------------------------------------------------------------------------------------------------

class Panel::RowComponent final : public juce::Component
{
public:
    RowComponent (Panel& owner, Entry e) : panel (owner), entry (std::move (e))
    {
        setWantsKeyboardFocus (true);
        setMouseClickGrabsKeyboardFocus (false);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setTitle (entry.text);
    }

    const Entry& getEntry() const noexcept      { return entry; }

    int getIdealWidth() const
    {
        float w = 10.0f + 14.0f + 10.0f + theme::textWidth (theme::sans (13.0f), entry.text) + 12.0f;

        if (entry.missing)
            w += 8.0f + theme::textWidth (theme::sans (11.0f), "not connected");

        return (int) std::ceil (w);
    }

    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat().reduced (2.0f, 1.0f);
        const bool ticked = panel.isTicked (entry.key), locked = panel.isLocked (entry.key);

        if (hover)
        {
            g.setColour (theme::bg3);
            g.fillRoundedRectangle (r, 4.0f);
        }

        const auto box = juce::Rectangle<float> (14.0f, 14.0f).withCentre ({ r.getX() + 15.0f, r.getCentreY() });

        if (ticked)
        {
            g.setColour (locked ? theme::lineStrong : theme::accent);
            g.fillRoundedRectangle (box, 3.0f);
            Icons::draw (g, "check", box.getCentre(), 12.0f, locked ? theme::text1 : theme::bg0);
        }
        else
        {
            g.setColour (theme::bg2);
            g.fillRoundedRectangle (box, 3.0f);
            g.setColour (theme::lineStrong);
            g.drawRoundedRectangle (box.reduced (0.5f), 2.5f, 1.0f);
        }

        float right = r.getRight() - 8.0f;

        if (entry.missing)
        {
            const auto note = juce::String ("not connected");
            const float w = theme::textWidth (theme::sans (11.0f), note);
            g.setColour (theme::text2);
            g.setFont (theme::sans (11.0f));
            g.drawText (note, juce::Rectangle<float> (right - w - 1.0f, r.getY(), w + 2.0f, r.getHeight()), juce::Justification::centredRight, false);
            right -= w + 8.0f;
        }

        const float x = box.getRight() + 10.0f;
        g.setColour (entry.missing ? theme::text1 : theme::text0);
        g.setFont (theme::sans (13.0f));
        g.drawText (entry.text, juce::Rectangle<float> (x, r.getY(), juce::jmax (0.0f, right - x), r.getHeight()), juce::Justification::centredLeft, true);

        if (hasKeyboardFocus (false))
            HeadroomLookAndFeel::drawFocusRing (g, r, 4.0f);
    }

    void mouseEnter (const juce::MouseEvent&) override      { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override       { hover = false; repaint(); }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu() && getLocalBounds().contains (e.getPosition()))
            panel.toggle (entry.key);
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        const int code = key.getKeyCode();

        if (code == juce::KeyPress::returnKey || code == juce::KeyPress::spaceKey)
        {
            panel.toggle (entry.key);
            return true;
        }

        if (code == juce::KeyPress::downKey || code == juce::KeyPress::upKey)
        {
            moveKeyboardFocusToSibling (code == juce::KeyPress::downKey);
            return true;
        }

        return false;
    }

    void focusGained (FocusChangeType) override             { repaint(); }
    void focusLost (FocusChangeType) override               { repaint(); }

private:
    Panel& panel;
    Entry entry;
    bool hover = false;
};

// ---------------------------------------------------------------------------------------------------
// The panel
// ---------------------------------------------------------------------------------------------------

Panel::Panel (AppController& c, Mix m) : controller (c), mix (m)
{
    setTitle (mix == Mix::Monitor ? "Play through" : "Also play to");
    setWantsKeyboardFocus (false);
    rebuild (entriesFor (controller, mix));
    startTimer (400);       // devices come and go while it is open
}

Panel::~Panel()
{
    stopTimer();
}

bool Panel::isTicked (const juce::String& key) const
{
    return controller.getState().getOutputs (mix).contains (key);
}

// The Headphones mix always plays somewhere: its last device cannot be unticked.
bool Panel::isLocked (const juce::String& key) const
{
    const auto list = controller.getState().getOutputs (mix);
    return mix == Mix::Monitor && list.size() == 1 && list.contains (key);
}

bool Panel::toggle (const juce::String& key)
{
    if (isLocked (key))
        return false;

    controller.setOutputSelected (mix, key, ! isTicked (key));
    refresh();
    return true;
}

bool Panel::isRowTicked (int index) const              { return juce::isPositiveAndBelow (index, (int) rows.size()) && isTicked (rows[(size_t) index]->getEntry().key); }
juce::String Panel::getRowKey (int index) const        { return juce::isPositiveAndBelow (index, (int) rows.size()) ? rows[(size_t) index]->getEntry().key : juce::String(); }
juce::String Panel::getRowText (int index) const       { return juce::isPositiveAndBelow (index, (int) rows.size()) ? rows[(size_t) index]->getEntry().text : juce::String(); }
bool Panel::isRowMissing (int index) const             { return juce::isPositiveAndBelow (index, (int) rows.size()) && rows[(size_t) index]->getEntry().missing; }
juce::Component* Panel::getRow (int index) const       { return juce::isPositiveAndBelow (index, (int) rows.size()) ? rows[(size_t) index].get() : nullptr; }

void Panel::refresh()
{
    rebuild (entriesFor (controller, mix));
    repaint();
}

void Panel::rebuild (std::vector<Entry> fresh)
{
    bool same = fresh.size() == entries.size();

    for (size_t i = 0; same && i < fresh.size(); ++i)
        same = fresh[i].key == entries[i].key && fresh[i].text == entries[i].text && fresh[i].missing == entries[i].missing;

    if (same)
        return;

    // Keep the keyboard on the row it was on (by key) when the list changes under it.
    juce::String focused;

    for (auto& row : rows)
        if (row->hasKeyboardFocus (false))
            focused = row->getEntry().key;

    entries = std::move (fresh);
    rows.clear();

    int widest = kMinWidth;

    for (const auto& entry : entries)
    {
        auto row = std::make_unique<RowComponent> (*this, entry);
        widest = juce::jmax (widest, row->getIdealWidth() + 2 * kPadding);
        addAndMakeVisible (*row);
        rows.push_back (std::move (row));
    }

    idealWidth = juce::jmin (kMaxWidth, widest);
    idealHeight = kPadding + kHeaderHeight + (int) rows.size() * kRowHeight + kFooterHeight;

    if (getWidth() != idealWidth || getHeight() != idealHeight)
        setSize (idealWidth, idealHeight);
    else
        resized();

    if (focused.isNotEmpty())
        for (auto& row : rows)
            if (row->getEntry().key == focused && row->isShowing())
                row->grabKeyboardFocus();
}

void Panel::resized()
{
    int y = kPadding + kHeaderHeight;

    for (auto& row : rows)
    {
        row->setBounds (kPadding, y, getWidth() - 2 * kPadding, kRowHeight);
        y += kRowHeight;
    }
}

void Panel::parentHierarchyChanged()
{
    // First time on screen: the keyboard starts on the first row (Space or Return ticks it, the arrows move).
    if (! focusedOnce && isShowing() && ! rows.empty())
    {
        focusedOnce = true;
        rows.front()->grabKeyboardFocus();
    }
}

void Panel::paint (juce::Graphics& g)
{
    // The hairline panel of the menus: bg1 with a 1 px strong line, 8 px radius.
    const auto frame = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (theme::bg1);
    g.fillRoundedRectangle (frame, theme::kPanelRadius);
    g.setColour (theme::lineStrong);
    g.drawRoundedRectangle (frame, theme::kPanelRadius, 1.0f);

    g.setColour (theme::text2);
    g.setFont (theme::caps (10.0f));
    g.drawText ((mix == Mix::Monitor ? juce::String ("Play through") : juce::String ("Also play to")).toUpperCase(),
                juce::Rectangle<int> (kPadding + 10, kPadding + 8, getWidth() - 2 * kPadding - 20, 16), juce::Justification::centredLeft, false);

    const auto hint = mix == Mix::Monitor ? juce::String ("Tick every device you want to hear. One stays on.")
                                          : "Always goes to " + theme::glyph::quoteOpen + "Headroom Stream Mix" + theme::glyph::quoteClose + " too.";
    g.setFont (theme::sans (11.0f));
    g.drawText (hint, juce::Rectangle<int> (kPadding + 10, getHeight() - kFooterHeight + 2, getWidth() - 2 * kPadding - 20, kFooterHeight - 6),
                juce::Justification::centredLeft, true);
}

// ---------------------------------------------------------------------------------------------------
// Opening it: a transparent layer over the window that holds the panel and closes it
// ---------------------------------------------------------------------------------------------------

namespace
{
    const juce::Identifier kHolderKey { "hrPlayThroughLayer" };

    // The window's content: the highest component above `c`, or the content of the window around it.
    juce::Component& windowContentOf (juce::Component& c)
    {
        juce::Component* top = &c;

        while (auto* parent = top->getParentComponent())
            top = parent;

        if (auto* window = dynamic_cast<juce::ResizableWindow*> (top))
            if (auto* content = window->getContentComponent())
                top = content;

        return *top;
    }
}

class Layer final : public juce::Component
{
public:
    Layer (Dropdown& sourceBox, AppController& controller, Mix mix) : source (&sourceBox), panel (controller, mix)
    {
        setWantsKeyboardFocus (false);
        addAndMakeVisible (panel);
    }

    Panel& getPanel() noexcept          { return panel; }

    // After the event that asked for it has been handled: a click or a key must not delete the component it is still inside.
    void dismiss()
    {
        if (dismissing)
            return;

        dismissing = true;
        juce::Component::SafePointer<Layer> safe (this);
        juce::MessageManager::callAsync ([safe]
        {
            if (safe == nullptr)
                return;

            auto* window = safe->getParentComponent();
            juce::Component::SafePointer<Dropdown> back (safe->source);

            if (window != nullptr)
                close (*window);        // deletes the layer

            if (back != nullptr && back->isShowing())
                back->grabKeyboardFocus();
        });
    }

    void place()
    {
        if (source == nullptr || getWidth() <= 0)
            return;

        // Under the control, its left edge on the panel's (kept inside the window); above it when there is no room below.
        const auto anchor = getLocalArea (source.getComponent(), source->getLocalBounds());
        const int w = panel.getIdealWidth(), h = panel.getIdealHeight();
        const int x = juce::jlimit (8, juce::jmax (8, getWidth() - w - 8), anchor.getX());
        int y = anchor.getBottom() + 4;

        if (y + h > getHeight() - 8)
            y = juce::jmax (8, anchor.getY() - 4 - h);

        panel.setBounds (x, y, w, h);
    }

    void resized() override                                     { place(); }
    void childBoundsChanged (juce::Component*) override         { place(); }
    void parentSizeChanged() override                           { dismiss(); }      // the window was resized under it
    void mouseDown (const juce::MouseEvent&) override           { dismiss(); }      // a click anywhere else
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override {}

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key.isKeyCode (juce::KeyPress::escapeKey))
        {
            dismiss();
            return true;
        }

        return false;
    }

private:
    juce::Component::SafePointer<Dropdown> source;
    Panel panel;
    bool dismissing = false;
};

namespace
{
    // The window keeps the layer alive (in its property set, like HoverFade): it goes away with the window, never leaks.
    class Holder final : public juce::ReferenceCountedObject
    {
    public:
        Holder (juce::Component& window, std::unique_ptr<Layer> l) : layer (std::move (l))
        {
            window.addAndMakeVisible (*layer);
            layer->setBounds (window.getLocalBounds());
        }

        ~Holder() override
        {
            if (auto* parent = layer->getParentComponent())
                parent->removeChildComponent (layer.get());
        }

        std::unique_ptr<Layer> layer;
    };

    Holder* holderOf (juce::Component& window)
    {
        return dynamic_cast<Holder*> (window.getProperties()[kHolderKey].getObject());
    }
}

Panel& open (Dropdown& source, AppController& controller, Mix mix)
{
    auto& window = windowContentOf (source);
    close (window);

    auto layer = std::make_unique<Layer> (source, controller, mix);
    auto& panel = layer->getPanel();
    auto* holder = new Holder (window, std::move (layer));      // sizes the layer to the window and puts it in front
    window.getProperties().set (kHolderKey, juce::var (holder));
    holder->layer->place();
    holder->layer->toFront (false);
    return panel;
}

void close (juce::Component& anyComponentInIt)
{
    auto& window = windowContentOf (anyComponentInIt);

    if (holderOf (window) != nullptr)
        window.getProperties().remove (kHolderKey);
}

bool isOpen (juce::Component& anyComponentInIt)
{
    return holderOf (windowContentOf (anyComponentInIt)) != nullptr;
}
}
