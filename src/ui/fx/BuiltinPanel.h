// BuiltinPanel: the inline editor of one built-in effect inside the FX drawer.
#pragma once

#include "Juce.h"
#include "engine/builtin/BuiltinProcessor.h"

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace hr
{
class BuiltinPanel : public juce::Component
{
public:
    // Panels are laid out at this width (the drawer's inner width).
    static constexpr int kWidth = 368;

    ~BuiltinPanel() override = default;

    virtual int getPreferredHeight() const = 0;
    virtual void setDimmed (bool shouldDim) = 0;     // effect switched off: knobs fade

    // 30 fps, message thread, only while the processor is known to be alive.
    virtual void tickMeter (double deltaSeconds) { juce::ignoreUnused (deltaSeconds); }

    // A panel whose height changes by itself (Noise Removal: the Advanced section, the tip) calls this so the drawer
    // lays its rows out again.
    std::function<void()> onHeightChanged;
};

// nullptr for an unknown effect. The panel keeps references into `processor`: the owner destroys it first.
std::unique_ptr<BuiltinPanel> createBuiltinPanel (BuiltinProcessor& processor);

// Repaints / rebuilds a UI piece when any of a set of parameters changes, from any thread.
class ParamWatcher final : private juce::AudioProcessorParameter::Listener,
                           private juce::AsyncUpdater
{
public:
    ParamWatcher (std::vector<juce::RangedAudioParameter*> parameters, std::function<void()> onChange)
        : params (std::move (parameters)), callback (std::move (onChange))
    {
        for (auto* p : params)
            p->addListener (this);
    }

    ~ParamWatcher() override
    {
        cancelPendingUpdate();

        for (auto* p : params)
            p->removeListener (this);
    }

private:
    void parameterValueChanged (int, float) override            { triggerAsyncUpdate(); }
    void parameterGestureChanged (int, bool) override           {}
    void handleAsyncUpdate() override                           { if (callback) callback(); }

    std::vector<juce::RangedAudioParameter*> params;
    std::function<void()> callback;
};
}
