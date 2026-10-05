// Single include for the JUCE modules Headroom uses (no juce_audio_devices / audio_utils / opengl: no ALSA, JACK or GL dependencies).
#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

namespace hr
{
// GCC (LP64) finds `juce::String << unsigned int` ambiguous while MSVC accepts it, so it only ever broke on CI.
// Deleting the exact match makes it a compile error everywhere in our namespace: wrap the value in juce::String().
juce::String& operator<< (juce::String&, unsigned int) = delete;
}
