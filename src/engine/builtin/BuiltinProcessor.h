// Base class for the built-in effects: stereo bus, APVTS parameters, XML state, no native editor.
#pragma once

#include "Juce.h"

#include <memory>

namespace hr
{
class BuiltinProcessor : public juce::AudioProcessor
{
public:
    using Layout = juce::AudioProcessorValueTreeState::ParameterLayout;

    // builtinId is the registry id ("noise", "eq", ...); it is stable and stored in the model.
    BuiltinProcessor (const juce::String& builtinId, const juce::String& displayName, Layout layout);
    ~BuiltinProcessor() override = default;

    const juce::String& getBuiltinId() const noexcept   { return id; }

    // Live value for the panel's meter, safe to call from any thread: gain reduction in dB (<= 0) for
    // comp/gate/deess/limit, voice probability 0..1 for noise, 0 for effects without a meter.
    virtual float getMeterValue() const noexcept        { return 0.0f; }

    // The inline FX-drawer panels (src/ui/fx) attach to this.
    juce::AudioProcessorValueTreeState apvts;

    // ---- juce::AudioProcessor ---------------------------------------------------------------------
    const juce::String getName() const override         { return name; }
    double getTailLengthSeconds() const override        { return 0.0; }
    bool acceptsMidi() const override                   { return false; }
    bool producesMidi() const override                  { return false; }
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override                     { return false; }

    int getNumPrograms() override                       { return 1; }
    int getCurrentProgram() override                    { return 0; }
    void setCurrentProgram (int) override               {}
    const juce::String getProgramName (int) override    { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // ---- Parameter helpers used by the subclasses' createLayout() ----------------------------------
    // Frequencies etc. get a skew so `centre` sits mid-slider (0 = linear).
    static std::unique_ptr<juce::AudioParameterFloat> makeFloat (const juce::String& paramId, const juce::String& paramName,
                                                                 float min, float max, float defaultValue,
                                                                 const juce::String& unit = {}, float centre = 0.0f,
                                                                 float interval = 0.0f);
    static std::unique_ptr<juce::AudioParameterBool> makeBool (const juce::String& paramId, const juce::String& paramName,
                                                               bool defaultValue);
    static std::unique_ptr<juce::AudioParameterChoice> makeChoice (const juce::String& paramId, const juce::String& paramName,
                                                                   const juce::StringArray& choices, int defaultIndex);

private:
    const juce::String id, name;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BuiltinProcessor)
};
}
