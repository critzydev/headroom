// ParamKnob: an arc knob with label and value readout, bound to one effect parameter.
#pragma once

#include "Juce.h"

namespace hr
{
// Drag vertically (Shift = fine), wheel, arrow keys, double-click = default. Talks to the parameter through
// juce::ParameterAttachment, so gestures are reported to hosts correctly and outside changes show up here.
class ParamKnob final : public juce::Component,
                        public juce::SettableTooltipClient
{
public:
    ParamKnob (juce::RangedAudioParameter& parameter, const juce::String& label, bool signedValue = false);

    void setDimmed (bool shouldDim)         { if (dimmed != shouldDim) { dimmed = shouldDim; repaint(); } }

    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override      { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override       { hover = false; repaint(); }
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusGained (FocusChangeType) override             { repaint(); }
    void focusLost (FocusChangeType) override               { repaint(); }

    static constexpr int kWidth = 92, kHeight = 92;

private:
    void nudge (float normalisedDelta);
    juce::Rectangle<float> getDialBounds() const;

    juce::RangedAudioParameter& param;
    juce::ParameterAttachment attachment;
    juce::String labelText;
    bool signedDb;
    float plainValue = 0.0f;
    bool hover = false, dimmed = false, dragging = false, dragFine = false;
    float dragStartNorm = 0.0f, dragStartY = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamKnob)
};
}
