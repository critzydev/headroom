// Focus helper: JUCE asserts when a component that is not on screen asks for the keyboard focus.
#pragma once

#include "Juce.h"

namespace hr
{
// Grabs the keyboard focus only if the component is actually showing (a hidden window, or screenshot mode, has none).
inline void focusIfShowing (juce::Component& c)
{
    if (c.isShowing())
        c.grabKeyboardFocus();
}
}
