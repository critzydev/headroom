// Toast implementation.
#include "ui/Toast.h"

#include "ui/Icons.h"
#include "ui/Theme.h"

namespace hr
{
ToastView::ToastView (const juce::String& message) : text (message) {}

int ToastView::getIdealWidth() const
{
    return 16 + 16 + 10 + (int) std::ceil (theme::textWidth (theme::sans (13.0f), text)) + 20;
}

void ToastView::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg2);
    g.setColour (theme::lineStrong);
    g.drawRect (getLocalBounds(), 1);

    Icons::draw (g, "info", juce::Point<float> (16.0f + 8.0f, (float) getHeight() * 0.5f), 16.0f, theme::text1);

    g.setColour (theme::text0);
    g.setFont (theme::sans (13.0f));
    g.drawText (text, getLocalBounds().withTrimmedLeft (16 + 16 + 10).withTrimmedRight (16), juce::Justification::centredLeft, true);
}

// ---------------------------------------------------------------------------------------------------

namespace
{
    juce::Component::SafePointer<ToastWindow>& current()
    {
        static juce::Component::SafePointer<ToastWindow> toast;
        return toast;
    }
}

void ToastWindow::show (const juce::String& message, int milliseconds)
{
    if (auto* old = current().getComponent())
        delete old;

    current() = new ToastWindow (message, milliseconds);
}

ToastWindow::ToastWindow (const juce::String& message, int milliseconds) : view (message)
{
    const int width = juce::jmin (view.getIdealWidth(), 640);
    addAndMakeVisible (view);
    setSize (width, ToastView::kHeight);
    view.setBounds (getLocalBounds());

    const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    const auto area = display != nullptr ? display->userBounds.toNearestInt() : juce::Rectangle<int> (0, 0, 1280, 720);

    setTopLeftPosition (area.getRight() - width - 24, area.getBottom() - ToastView::kHeight - 24);
    setAlwaysOnTop (true);
    addToDesktop (juce::ComponentPeer::windowIsTemporary | juce::ComponentPeer::windowIgnoresKeyPresses);
    setVisible (true);
    startTimer (milliseconds);
}

void ToastWindow::dismiss()
{
    if (fading)
        return;

    fading = true;
    juce::Desktop::getInstance().getAnimator().fadeOut (this, 200);
    startTimer (300);
}

void ToastWindow::timerCallback()
{
    if (! fading)
    {
        dismiss();
        return;
    }

    stopTimer();
    delete this;
}
}
