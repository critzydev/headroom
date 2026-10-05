// Dropdown: a quiet select box (optional prefix and colour dot) that opens a themed PopupMenu.
#pragma once

#include "Juce.h"

#include <functional>
#include <vector>

namespace hr
{
class Dropdown final : public juce::Component,
                       public juce::SettableTooltipClient
{
public:
    struct Item
    {
        juce::String key;               // stable value the owner cares about (device id, channel id, ...)
        juce::String text;              // what the user reads
        juce::Colour dot;               // shown before the text when hasDot
        bool hasDot = false;
        bool enabled = true;
        bool separatorBefore = false;
        juce::String shortText;         // shown in the closed box instead of `text` when set
    };

    Dropdown();

    void setItems (std::vector<Item> newItems);
    void setSelectedKey (const juce::String& key);          // model -> UI, no callback
    void setSelectedKeyOr (const juce::String& key, const juce::String& fallbackKey);   // the fallback if no item has `key`
    const juce::String& getSelectedKey() const noexcept     { return selectedKey; }
    juce::String getSelectedText() const;                   // what the closed box says (without the prefix)
    void setPrefix (const juce::String& text)               { prefix = text; repaint(); }
    void setPlaceholder (const juce::String& text)          { placeholder = text; repaint(); }
    int getIdealWidth() const;

    std::function<void (const juce::String&)> onChange;

    // Set to show your own menu instead of the item list (the mic preset menu has two-line items).
    std::function<void (Dropdown&)> onOpenRequested;

    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override      { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override       { hover = false; repaint(); }
    void mouseUp (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusGained (FocusChangeType) override             { repaint(); }
    void focusLost (FocusChangeType) override              { repaint(); }

    // A small filled circle as a menu image, shared with the strips' colour menu.
    static std::unique_ptr<juce::Drawable> makeDot (juce::Colour colour);

private:
    void showMenu();
    const Item* findSelected() const;

    std::vector<Item> items;
    juce::String selectedKey, prefix, placeholder;
    bool hover = false;
};
}
