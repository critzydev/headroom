// StatusPill (mic state in the top bar) and BannerRow (one line of status at the top of the window).
#pragma once

#include "Juce.h"
#include "ui/widgets/Buttons.h"

#include <functional>

namespace hr
{
// Dot + short text in a hairline pill. Clicking it opens Settings > Microphone.
class StatusPill final : public juce::Component,
                         public juce::SettableTooltipClient
{
public:
    StatusPill();

    void set (juce::Colour dotColour, const juce::String& text);
    int getIdealWidth() const;

    std::function<void()> onClick;

    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override      { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override       { hover = false; repaint(); }
    void mouseUp (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusGained (FocusChangeType) override             { repaint(); }
    void focusLost (FocusChangeType) override               { repaint(); }

private:
    juce::Colour dot;
    juce::String label;
    bool hover = false;
};

struct BannerSpec
{
    enum class Severity { info, warning, error };

    juce::String id;
    Severity severity = Severity::info;
    juce::String text;
    juce::String actionLabel;               // empty = no button
    std::function<void()> action;
    std::function<void()> onDismiss;        // set = show the close button

    bool sameContentAs (const BannerSpec& o) const
    {
        return id == o.id && severity == o.severity && text == o.text && actionLabel == o.actionLabel
               && (onDismiss != nullptr) == (o.onDismiss != nullptr);
    }
};

// 36 px row tinted by severity at 12 %: icon, one sentence, an optional text button, an optional close.
class BannerRow final : public juce::Component
{
public:
    explicit BannerRow (BannerSpec spec);

    const BannerSpec& getSpec() const noexcept      { return banner; }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    juce::Colour severityColour() const;

    BannerSpec banner;
    juce::TextButton actionButton;
    IconButton closeButton { "x", 14.0f };
    juce::Rectangle<int> textArea;
};
}
