// BuiltinProcessor: shared bus layout, state (de)serialisation and parameter factory helpers.
#include "engine/builtin/BuiltinProcessor.h"

namespace hr
{
BuiltinProcessor::BuiltinProcessor (const juce::String& builtinId, const juce::String& displayName, Layout layout)
    : juce::AudioProcessor (BusesProperties()
                                .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", std::move (layout)),
      id (builtinId),
      name (displayName)
{
}

bool BuiltinProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();

    return (out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono())
           && layouts.getMainInputChannelSet() == out;
}

void BuiltinProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void BuiltinProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

std::unique_ptr<juce::AudioParameterFloat> BuiltinProcessor::makeFloat (const juce::String& paramId, const juce::String& paramName,
                                                                        float min, float max, float defaultValue,
                                                                        const juce::String& unit, float centre, float interval)
{
    juce::NormalisableRange<float> range (min, max, interval);

    if (centre > min && centre < max)
        range.setSkewForCentre (centre);

    return std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { paramId, 1 }, paramName, range, defaultValue,
                                                        juce::AudioParameterFloatAttributes().withLabel (unit));
}

std::unique_ptr<juce::AudioParameterBool> BuiltinProcessor::makeBool (const juce::String& paramId, const juce::String& paramName,
                                                                      bool defaultValue)
{
    return std::make_unique<juce::AudioParameterBool> (juce::ParameterID { paramId, 1 }, paramName, defaultValue);
}

std::unique_ptr<juce::AudioParameterChoice> BuiltinProcessor::makeChoice (const juce::String& paramId, const juce::String& paramName,
                                                                          const juce::StringArray& choices, int defaultIndex)
{
    return std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { paramId, 1 }, paramName, choices, defaultIndex);
}
}
