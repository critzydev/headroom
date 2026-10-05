// Vertical layout shared by the channel strips and the master strip, so every fader lines up across the mixer.
#pragma once

#include "Juce.h"

namespace hr
{
struct StripLayout
{
    juce::Rectangle<int> tag, caps, name, line, slot, fader, bottom;

    // `bounds` is the whole strip. `inner` is the horizontal padding.
    static StripLayout compute (juce::Rectangle<int> bounds, int inner = 8)
    {
        StripLayout l;
        auto area = bounds.reduced (inner, 0);

        l.tag = bounds.withHeight (3);
        area.removeFromTop (3 + 8);
        l.caps = area.removeFromTop (20);           // icon on the left, INPUT / MASTER on the right
        area.removeFromTop (4);
        l.name = area.removeFromTop (22);
        area.removeFromTop (2);
        l.line = area.removeFromTop (14);
        area.removeFromTop (10);
        l.slot = area.removeFromTop (28);
        area.removeFromTop (8);

        l.bottom = area.removeFromBottom (32);
        area.removeFromBottom (8);
        l.fader = area;
        return l;
    }
};
}
