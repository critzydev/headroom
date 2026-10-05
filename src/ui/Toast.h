// Toast: the one-line "Headroom keeps running in the background" note, shown in its own small window so it is still
// there after the main window has been hidden.
#pragma once

#include "Juce.h"

namespace hr
{
class ToastView final : public juce::Component
{
public:
    explicit ToastView (const juce::String& message);

    int getIdealWidth() const;
    static constexpr int kHeight = 44;

    void paint (juce::Graphics&) override;

private:
    juce::String text;
};

// Floats near the bottom right of the main screen for a few seconds, then fades and deletes itself.
class ToastWindow final : public juce::Component,
                          public juce::DeletedAtShutdown,
                          private juce::Timer
{
public:
    static void show (const juce::String& message, int milliseconds = 7000);

private:
    ToastWindow (const juce::String& message, int milliseconds);
    ~ToastWindow() override = default;

    void timerCallback() override;
    void mouseDown (const juce::MouseEvent&) override   { dismiss(); }
    void dismiss();

    ToastView view;
    bool fading = false;
};
}
