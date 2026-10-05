// UiScale: how big the UI should be on this desktop (HiDPI). JUCE reads XSETTINGS Gdk/WindowScalingFactor,
// which KDE does not publish for Xwayland scaling, so on Linux we look at Xft.dpi ourselves and fall back to kwinrc.
#pragma once

#include "Juce.h"

#include <optional>

namespace hr::UiScale
{
constexpr float kMinScale = 1.0f, kMaxScale = 3.0f;

// Nearest 0.25, clamped to 1.0 ... 3.0.
float snap (double scale);

// Pure parsing helpers (unit tested). Each returns the snapped scale, or nothing if the text is unusable.
std::optional<float> fromXftDpi (const juce::String& dpiText);              // "120" -> 1.25
std::optional<float> fromKwinrc (const juce::String& kwinrcText);           // "[Xwayland]\nScale=1.25" -> 1.25
std::optional<float> fromGdkScale (const juce::String& gdkScaleText);       // "2" -> 2.0

// Linux: Xft.dpi from the X server (libX11 is loaded at run time), else kwinrc [Xwayland] Scale, else GDK_SCALE.
// Everywhere else, and when nothing is found: 1.0 (Windows and macOS scale through JUCE already). Looked up once.
float detectSystemScale();

// SETTINGS uiScale: 0 (or unset) = automatic, otherwise the chosen factor.
bool isAutomatic (const juce::var& setting);

// The factor for Desktop::setGlobalScaleFactor. Automatic = detected / what JUCE already applies for the display
// (so a desktop JUCE does understand is not scaled twice). A chosen size is used as it is.
float effectiveScale (const juce::var& setting, float detected, float platformScale);
float effectiveScale (const juce::var& setting);        // detects, and asks JUCE for the primary display's scale
}
