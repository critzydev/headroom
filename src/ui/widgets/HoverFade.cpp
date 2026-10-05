// HoverFade implementation.
#include "ui/widgets/HoverFade.h"

namespace hr
{
namespace
{
    const juce::Identifier kFadeProperty { "hrHoverFade" };
    const juce::Identifier kAmountProperty { "hrHoverAmount" };

    float easeOut (float t)     { const float u = 1.0f - t; return 1.0f - u * u * u; }
}

HoverFade::HoverFade (juce::Component& component) : owner (&component)
{
    component.addMouseListener (this, true);       // nested children too: a row stays hovered while the pointer is on its buttons
}

HoverFade::~HoverFade()
{
    stopTimer();

    if (owner != nullptr)
        owner->removeMouseListener (this);
}

void HoverFade::attach (juce::Component& component)
{
    if (component.getProperties().contains (kFadeProperty))
        return;

    component.getProperties().set (kFadeProperty, juce::var (new HoverFade (component)));
}

float HoverFade::get (const juce::Component& component, bool fallback)
{
    if (! component.getProperties().contains (kFadeProperty))
        return fallback ? 1.0f : 0.0f;

    return (float) (double) component.getProperties()[kAmountProperty];
}

void HoverFade::refreshTarget()
{
    if (owner != nullptr)
        setTarget (owner->isMouseOver (true) ? 1.0f : 0.0f);
}

void HoverFade::setTarget (float newTarget)
{
    target = newTarget;

    if (! isTimerRunning())
    {
        lastMs = juce::Time::getMillisecondCounterHiRes();
        startTimerHz (60);
    }
}

void HoverFade::timerCallback()
{
    if (owner == nullptr)
    {
        stopTimer();
        return;
    }

    const auto now = juce::Time::getMillisecondCounterHiRes();
    const float step = (float) ((now - lastMs) / kMilliseconds);
    lastMs = now;

    value = value < target ? juce::jmin (target, value + step) : juce::jmax (target, value - step);
    owner->getProperties().set (kAmountProperty, (double) easeOut (value));
    owner->repaint();

    if (value == target)
        stopTimer();
}
}
