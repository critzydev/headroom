// Sheets: a dimmed overlay with one centred panel. Used by the first-run message and the "remove channel" question.
#pragma once

#include "Juce.h"
#include "backend/BackendTypes.h"

#include <functional>
#include <memory>

namespace hr
{
class SheetHost final : public juce::Component
{
public:
    SheetHost();

    // Shows `content` centred in a panel of that size. Any sheet already open is replaced.
    void show (std::unique_ptr<juce::Component> content, int width, int height);
    void hide();
    bool isOpen() const noexcept                { return content != nullptr; }
    juce::Component* getContent() noexcept      { return content.get(); }

    std::function<void()> onEscape;             // what Esc does (the first-run sheet: same as Done)
    std::function<void()> onEnter;              // what Return does; none for the destructive question

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override   {}
    bool keyPressed (const juce::KeyPress&) override;

private:
    std::unique_ptr<juce::Component> content;
    juce::Rectangle<int> panel;
    int panelWidth = 0, panelHeight = 0;
};

// "You're set up": what Headroom did on the first launch, in three short lines.
class FirstRunSheet final : public juce::Component
{
public:
    FirstRunSheet (const BackendStatus& status, const juce::String& chatChannelName, std::function<void()> onDone);

    static constexpr int kWidth = 520, kHeight = 372;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Line { juce::String title, text; bool good = true; };
    std::vector<Line> lines;
    juce::TextButton doneButton { "Done" };
    juce::Rectangle<int> titleArea;
};

// A yes / no question about something that can't be undone.
class ConfirmSheet final : public juce::Component
{
public:
    ConfirmSheet (const juce::String& title, const juce::String& message, const juce::String& confirmLabel,
                  std::function<void()> onConfirm, std::function<void()> onCancel);

    static constexpr int kWidth = 440, kHeight = 200;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    juce::String heading, body;
    juce::TextButton cancelButton { "Cancel" }, confirmButton;
};
}
