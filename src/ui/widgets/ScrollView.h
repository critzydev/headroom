// ScrollView: a Viewport whose mouse wheel moves a fixed, comfortable distance per notch.
#pragma once

#include "Juce.h"

namespace hr
{
// JUCE's own wheel scaling depends on a "single step" size and on how the OS reports deltas, which made lists crawl on
// some setups. A wheel notch here always moves `stepPixels` (in the direction that fits the bars shown); touchpad
// smooth scrolling still goes through the stock code.
class ScrollView final : public juce::Viewport
{
public:
    explicit ScrollView (bool horizontal = false, int stepPixels = 56) : isHorizontal (horizontal), step (stepPixels) {}

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        const float amount = wheel.deltaY != 0.0f ? wheel.deltaY : wheel.deltaX;

        if (wheel.isSmooth || amount == 0.0f || e.mods.isAltDown() || e.mods.isCtrlDown() || e.mods.isCommandDown())
        {
            juce::Viewport::mouseWheelMove (e, wheel);
            return;
        }

        const int direction = (amount > 0.0f ? 1 : -1) * (wheel.isReversed ? -1 : 1);

        if (isHorizontal)
            setViewPosition (getViewPositionX() - direction * step, getViewPositionY());
        else
            setViewPosition (getViewPositionX(), getViewPositionY() - direction * step);
    }

private:
    bool isHorizontal;
    int step;
};
}
