// UiScale implementation.
#include "ui/UiScale.h"

#include <cmath>

namespace hr::UiScale
{
float snap (double scale)
{
    if (! std::isfinite (scale))
        return kMinScale;

    const double snapped = std::round (scale * 4.0) / 4.0;
    return (float) juce::jlimit ((double) kMinScale, (double) kMaxScale, snapped);
}

std::optional<float> fromXftDpi (const juce::String& dpiText)
{
    const auto text = dpiText.trim();

    if (text.isEmpty() || ! text.containsOnly ("0123456789."))
        return std::nullopt;

    const double dpi = text.getDoubleValue();
    return dpi >= 24.0 && dpi <= 1000.0 ? std::optional<float> (snap (dpi / 96.0)) : std::nullopt;
}

std::optional<float> fromKwinrc (const juce::String& kwinrcText)
{
    bool inSection = false;

    for (auto line : juce::StringArray::fromLines (kwinrcText))
    {
        line = line.trim();

        if (line.startsWithChar ('['))
        {
            inSection = line.equalsIgnoreCase ("[Xwayland]");
            continue;
        }

        if (inSection && line.startsWithIgnoreCase ("Scale"))
        {
            const auto rest = line.substring (5).trimStart();

            if (! rest.startsWithChar ('='))
                continue;

            const auto value = rest.substring (1).trim();

            if (value.isNotEmpty() && value.containsOnly ("0123456789.") && value.getDoubleValue() > 0.0)
                return snap (value.getDoubleValue());
        }
    }

    return std::nullopt;
}

std::optional<float> fromGdkScale (const juce::String& gdkScaleText)
{
    const auto text = gdkScaleText.trim();

    if (text.isEmpty() || ! text.containsOnly ("0123456789.") || text.getDoubleValue() <= 0.0)
        return std::nullopt;

    return snap (text.getDoubleValue());
}

namespace
{
    // Xft.dpi as the X server (Xwayland included) has it in RESOURCE_MANAGER. libX11 is opened at run time so the
    // binary has no link-time X dependency.
    std::optional<float> readXftDpi()
    {
        juce::DynamicLibrary x11;

        if (! x11.open ("libX11.so.6"))
            return std::nullopt;

        using OpenFn = void* (*) (const char*);
        using GetDefaultFn = char* (*) (void*, const char*, const char*);
        using CloseFn = int (*) (void*);

        auto openDisplay = reinterpret_cast<OpenFn> (x11.getFunction ("XOpenDisplay"));
        auto getDefault = reinterpret_cast<GetDefaultFn> (x11.getFunction ("XGetDefault"));
        auto closeDisplay = reinterpret_cast<CloseFn> (x11.getFunction ("XCloseDisplay"));

        if (openDisplay == nullptr || getDefault == nullptr || closeDisplay == nullptr)
            return std::nullopt;

        void* display = openDisplay (nullptr);

        if (display == nullptr)
            return std::nullopt;

        const char* raw = getDefault (display, "Xft", "dpi");
        const juce::String text = raw != nullptr ? juce::String (raw) : juce::String();      // copied before the display goes
        closeDisplay (display);

        return fromXftDpi (text);
    }

    std::optional<float> readKwinrc()
    {
        const auto configHome = juce::SystemStats::getEnvironmentVariable ("XDG_CONFIG_HOME", {});
        const auto base = configHome.isNotEmpty() ? juce::File (configHome)
                                                  : juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile (".config");
        const auto file = base.getChildFile ("kwinrc");

        return file.existsAsFile() ? fromKwinrc (file.loadFileAsString()) : std::nullopt;
    }

   #if JUCE_LINUX
    constexpr bool kLookForDesktopScale = true;
   #else
    constexpr bool kLookForDesktopScale = false;        // Windows / macOS: JUCE follows the OS setting already
   #endif

    float detectNow()
    {
        if constexpr (kLookForDesktopScale)
        {
            if (auto dpi = readXftDpi())
                return *dpi;

            if (auto kwin = readKwinrc())
                return *kwin;

            if (auto gdk = fromGdkScale (juce::SystemStats::getEnvironmentVariable ("GDK_SCALE", {})))
                return *gdk;
        }

        return 1.0f;
    }
}

float detectSystemScale()
{
    static const float detected = detectNow();
    return detected;
}

bool isAutomatic (const juce::var& setting)
{
    return setting.isVoid() || (double) setting <= 0.01;
}

float effectiveScale (const juce::var& setting, float detected, float platformScale)
{
    if (! isAutomatic (setting))
        return (float) juce::jlimit ((double) kMinScale, (double) kMaxScale, (double) setting);

    return snap ((double) detected / juce::jmax (1.0, (double) platformScale));
}

float effectiveScale (const juce::var& setting)
{
    // Display::scale already includes the global factor we set ourselves: take that out, or a second call would
    // divide our own choice away again.
    float platform = 1.0f;
    auto& desktop = juce::Desktop::getInstance();

    if (auto* display = desktop.getDisplays().getPrimaryDisplay())
        platform = (float) (display->scale / juce::jmax (0.1f, desktop.getGlobalScaleFactor()));

    return effectiveScale (setting, detectSystemScale(), platform);
}
}
