// Dropdown: paint and popup handling.
#include "ui/widgets/Dropdown.h"

#include "ui/HeadroomLookAndFeel.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/widgets/HoverFade.h"

namespace hr
{
Dropdown::Dropdown()
{
    setWantsKeyboardFocus (true);
    setMouseClickGrabsKeyboardFocus (false);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    HoverFade::attach (*this);
}

void Dropdown::setItems (std::vector<Item> newItems)
{
    items = std::move (newItems);
    repaint();
}

void Dropdown::setSelectedKey (const juce::String& key)
{
    if (key != selectedKey)
    {
        selectedKey = key;
        repaint();
    }
}

void Dropdown::setSelectedKeyOr (const juce::String& key, const juce::String& fallbackKey)
{
    for (auto& item : items)
        if (item.key == key)
        {
            setSelectedKey (key);
            return;
        }

    setSelectedKey (fallbackKey);
}

const Dropdown::Item* Dropdown::findSelected() const
{
    for (auto& item : items)
        if (item.key == selectedKey)
            return &item;

    return nullptr;
}

juce::String Dropdown::getSelectedText() const
{
    const auto* selected = findSelected();
    return selected == nullptr ? placeholder : (selected->shortText.isNotEmpty() ? selected->shortText : selected->text);
}

int Dropdown::getIdealWidth() const
{
    const auto font = theme::sans (13.0f);
    float widest = 0.0f;

    if (const auto* selected = findSelected())
        widest = theme::textWidth (font, selected->shortText.isNotEmpty() ? selected->shortText : selected->text) + (selected->hasDot ? 16.0f : 0.0f);

    const float prefixW = prefix.isNotEmpty() ? theme::textWidth (font, prefix) : 0.0f;
    return (int) std::ceil (10.0f + prefixW + widest + 8.0f + 24.0f);
}

std::unique_ptr<juce::Drawable> Dropdown::makeDot (juce::Colour colour)
{
    auto dot = std::make_unique<juce::DrawablePath>();
    juce::Path p;
    p.addEllipse (0.0f, 0.0f, 8.0f, 8.0f);
    dot->setPath (p);
    dot->setFill (juce::FillType (colour));
    return dot;
}

void Dropdown::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const bool enabled = isEnabled();

    g.setColour (theme::bg2.interpolatedWith (theme::bg3, enabled ? HoverFade::get (*this, hover) : 0.0f));
    g.fillRoundedRectangle (bounds, theme::kControlRadius);

    if (hasKeyboardFocus (false))
    {
        HeadroomLookAndFeel::drawFocusRing (g, bounds, theme::kControlRadius);
    }
    else
    {
        g.setColour (theme::line);
        g.drawRoundedRectangle (bounds.reduced (0.5f), theme::kControlRadius - 0.5f, 1.0f);
    }

    const auto font = theme::sans (13.0f);
    float x = bounds.getX() + 10.0f;
    const float right = bounds.getRight() - 28.0f;

    g.setFont (font);

    if (prefix.isNotEmpty())
    {
        const float w = theme::textWidth (font, prefix);
        g.setColour (theme::text2);
        g.drawText (prefix, juce::Rectangle<float> (x, bounds.getY(), w + 2.0f, bounds.getHeight()), juce::Justification::centredLeft, false);
        x += w;
    }

    const auto* selected = findSelected();

    if (selected != nullptr && selected->hasDot)
    {
        g.setColour (selected->dot);
        g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ x + 4.0f, bounds.getCentreY() }));
        x += 16.0f;
    }

    g.setColour (! enabled ? theme::text2 : (selected != nullptr ? theme::text0 : theme::text1));
    g.drawText (selected != nullptr ? (selected->shortText.isNotEmpty() ? selected->shortText : selected->text) : placeholder,
                juce::Rectangle<float> (x, bounds.getY(), juce::jmax (0.0f, right - x), bounds.getHeight()),
                juce::Justification::centredLeft, true);

    Icons::draw (g, "chevron-down", juce::Point<float> (bounds.getRight() - 16.0f, bounds.getCentreY()), 14.0f,
                 enabled ? theme::text1 : theme::text2);
}

void Dropdown::mouseUp (const juce::MouseEvent& e)
{
    if (isEnabled() && ! e.mods.isPopupMenu() && getLocalBounds().contains (e.getPosition()))
        showMenu();
}

bool Dropdown::keyPressed (const juce::KeyPress& key)
{
    const int code = key.getKeyCode();

    if (code == juce::KeyPress::returnKey || code == juce::KeyPress::spaceKey || code == juce::KeyPress::downKey)
    {
        showMenu();
        return true;
    }

    return false;
}

void Dropdown::showMenu()
{
    if (onOpenRequested)
    {
        onOpenRequested (*this);
        return;
    }

    juce::PopupMenu menu;

    for (size_t i = 0; i < items.size(); ++i)
    {
        const auto& item = items[i];

        if (item.separatorBefore)
            menu.addSeparator();

        juce::PopupMenu::Item entry (item.text);
        entry.setID ((int) i + 1).setEnabled (item.enabled).setTicked (item.key == selectedKey);

        if (item.hasDot)
            entry.setImage (makeDot (item.dot));

        menu.addItem (std::move (entry));
    }

    juce::Component::SafePointer<Dropdown> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMinimumWidth (getWidth()).withStandardItemHeight (28),
                        [safe] (int result)
                        {
                            if (safe == nullptr || result <= 0 || result > (int) safe->items.size())
                                return;

                            const auto key = safe->items[(size_t) result - 1].key;
                            safe->setSelectedKey (key);

                            if (safe->onChange)
                                safe->onChange (key);
                        });
}
}
