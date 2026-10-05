// HoverFade: a 150 ms ease-out fade for hover states. Attach once, read the amount when painting.
#pragma once

#include "Juce.h"

namespace hr
{
// No subclassing and no timer while nothing is changing: the fade object lives in the component's property set, listens
// to its mouse enter / exit, runs a 60 Hz timer for the 150 ms a transition takes, and repaints the component.
class HoverFade final : public juce::ReferenceCountedObject,
                        private juce::MouseListener,
                        private juce::Timer
{
public:
    static constexpr double kMilliseconds = 150.0;

    // Idempotent. The component keeps the fade alive; it goes away with the component.
    static void attach (juce::Component& component);

    // 0 = idle, 1 = fully hovered (eased). Components that were never attached report `fallback` (their own flag).
    static float get (const juce::Component& component, bool fallback = false);

private:
    explicit HoverFade (juce::Component& component);
    ~HoverFade() override;

    void mouseEnter (const juce::MouseEvent&) override      { refreshTarget(); }
    void mouseExit (const juce::MouseEvent&) override       { refreshTarget(); }
    void refreshTarget();
    void timerCallback() override;
    void setTarget (float newTarget);

    juce::WeakReference<juce::Component> owner;
    float value = 0.0f, target = 0.0f;
    double lastMs = 0.0;
};
}
