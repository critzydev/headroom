// PluginWindow: a native-titled window hosting a plugin's own editor (or the generic one).
#pragma once

#include "Juce.h"

#include <functional>
#include <optional>

namespace hr
{
class PluginWindow final : public juce::DocumentWindow
{
public:
    // onCloseRequested is called from the close button; the owner deletes the window (delete it BEFORE the processor).
    // rememberedBounds is where this slot's window last was: used only if it is still on a connected display, and
    // only its position unless the editor is resizable.
    PluginWindow (juce::AudioProcessor& processor, const juce::String& title, std::function<void()> onCloseRequested,
                  std::optional<juce::Rectangle<int>> rememberedBounds = std::nullopt);
    ~PluginWindow() override;

    void closeButtonPressed() override;

    // Plugin editors do their own scaling; like AudioPluginHost we don't let JUCE's global scale apply on top.
    float getDesktopScaleFactor() const override        { return 1.0f; }

    // True when the plugin has no editor of its own (most LADSPA / LV2) and JUCE's parameter list is shown instead.
    bool isShowingGenericEditor() const noexcept        { return genericEditor; }

    // Enough of `bounds` (its title bar strip) lies on a connected display for the user to grab the window.
    static bool isPositionUsable (const juce::Rectangle<int>& bounds);

private:
    std::function<void()> closeCallback;
    bool genericEditor = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindow)
};
}
