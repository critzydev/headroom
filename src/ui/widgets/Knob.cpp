// ParamKnob: 270 degree arc, hardware-style dial body, value text in mono.
#include "ui/widgets/Knob.h"

#include "ui/Format.h"
#include "ui/HeadroomLookAndFeel.h"
#include "ui/Theme.h"

namespace hr
{
namespace
{
    constexpr float kStartAngle = juce::MathConstants<float>::pi * 1.25f;
    constexpr float kEndAngle = juce::MathConstants<float>::pi * 2.75f;
    constexpr float kDialSize = 44.0f;
}

ParamKnob::ParamKnob (juce::RangedAudioParameter& parameter, const juce::String& label, bool signedValue)
    : param (parameter),
      attachment (parameter, [this] (float v) { plainValue = v; repaint(); }),
      labelText (label),
      signedDb (signedValue)
{
    setWantsKeyboardFocus (true);
    setMouseClickGrabsKeyboardFocus (false);
    setSize (kWidth, kHeight);
    attachment.sendInitialUpdate();
}

juce::Rectangle<float> ParamKnob::getDialBounds() const
{
    return juce::Rectangle<float> (kDialSize, kDialSize).withCentre ({ (float) getWidth() * 0.5f, 4.0f + kDialSize * 0.5f });
}

void ParamKnob::paint (juce::Graphics& g)
{
    const auto dial = getDialBounds();
    const auto centre = dial.getCentre();
    const float norm = param.convertTo0to1 (plainValue);
    const float angle = kStartAngle + norm * (kEndAngle - kStartAngle);
    const float alpha = dimmed ? 0.4f : 1.0f;

    // Track and value arc, 3 px strokes on the outer ring.
    const float arcRadius = kDialSize * 0.5f - 2.0f;
    const auto range = param.getNormalisableRange();
    const bool bipolar = range.start < 0.0f && range.end > 0.0f;           // gains: the arc grows from 0 dB
    const float anchorNorm = bipolar ? param.convertTo0to1 (0.0f) : 0.0f;
    const float anchorAngle = kStartAngle + anchorNorm * (kEndAngle - kStartAngle);

    juce::Path track, value;
    track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, kStartAngle, kEndAngle, true);
    value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, juce::jmin (anchorAngle, angle), juce::jmax (anchorAngle, angle), true);

    g.setColour (theme::line.withAlpha (alpha));
    g.strokePath (track, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    if (std::abs (norm - anchorNorm) > 0.004f)
    {
        g.setColour (theme::accent.withAlpha (alpha));
        g.strokePath (value, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Body and pointer.
    const auto body = dial.reduced (8.0f);
    g.setColour ((hover || dragging ? theme::bg3 : theme::bg2).withAlpha (alpha));
    g.fillEllipse (body);
    g.setColour (theme::lineStrong.withAlpha (alpha));
    g.drawEllipse (body.reduced (0.5f), 1.0f);

    const float r0 = 3.0f, r1 = body.getWidth() * 0.5f - 3.0f;
    const auto dir = juce::Point<float> (std::sin (angle), -std::cos (angle));
    g.setColour (theme::text0.withAlpha (alpha));
    g.drawLine (juce::Line<float> (centre + dir * r0, centre + dir * r1), 1.5f);

    if (hasKeyboardFocus (false))
    {
        g.setColour (theme::accent);
        g.drawEllipse (dial.expanded (3.0f), 1.0f);
    }

    // Label and value.
    g.setColour ((dragging ? theme::text0 : theme::text1).withAlpha (dimmed ? 0.55f : 1.0f));
    g.setFont (theme::sans (11.0f, theme::Sans::Medium));
    g.drawText (labelText, juce::Rectangle<float> (0.0f, dial.getBottom() + 8.0f, (float) getWidth(), 14.0f), juce::Justification::centred, true);

    g.setColour ((theme::text0).withAlpha (dimmed ? 0.55f : 1.0f));
    g.setFont (theme::mono (11.0f));
    g.drawText (fmt::param (param.getLabel(), plainValue, signedDb), juce::Rectangle<float> (0.0f, dial.getBottom() + 24.0f, (float) getWidth(), 14.0f),
                juce::Justification::centred, false);
}

void ParamKnob::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        return;

    dragging = true;
    dragFine = e.mods.isShiftDown();
    dragStartNorm = param.getValue();
    dragStartY = (float) e.y;
    attachment.beginGesture();
    repaint();
}

void ParamKnob::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging)
        return;

    if (e.mods.isShiftDown() != dragFine)
    {
        dragFine = e.mods.isShiftDown();
        dragStartNorm = param.getValue();
        dragStartY = (float) e.y;
    }

    const float pixelsForFullRange = dragFine ? 900.0f : 180.0f;
    const float norm = juce::jlimit (0.0f, 1.0f, dragStartNorm + (dragStartY - (float) e.y) / pixelsForFullRange);
    attachment.setValueAsPartOfGesture (param.convertFrom0to1 (norm));
}

void ParamKnob::mouseUp (const juce::MouseEvent&)
{
    if (dragging)
    {
        dragging = false;
        attachment.endGesture();
        repaint();
    }
}

void ParamKnob::mouseDoubleClick (const juce::MouseEvent&)
{
    attachment.setValueAsCompleteGesture (param.convertFrom0to1 (param.getDefaultValue()));
}

void ParamKnob::nudge (float normalisedDelta)
{
    const float norm = juce::jlimit (0.0f, 1.0f, param.getValue() + normalisedDelta);
    attachment.setValueAsCompleteGesture (param.convertFrom0to1 (norm));
}

void ParamKnob::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    // A knob in a scrolling list must not grab the wheel: only Shift / Ctrl + wheel turns it.
    if (! e.mods.isShiftDown() && ! e.mods.isCtrlDown())
    {
        juce::Component::mouseWheelMove (e, wheel);
        return;
    }

    const float direction = (wheel.deltaY > 0.0f ? 1.0f : (wheel.deltaY < 0.0f ? -1.0f : 0.0f)) * (wheel.isReversed ? -1.0f : 1.0f);

    if (direction != 0.0f)
        nudge (direction * (e.mods.isCtrlDown() ? 0.004f : 0.02f));
}

bool ParamKnob::keyPressed (const juce::KeyPress& key)
{
    const float step = key.getModifiers().isShiftDown() ? 0.004f : 0.02f;
    const int code = key.getKeyCode();

    if (code == juce::KeyPress::upKey || code == juce::KeyPress::rightKey)      { nudge (step);  return true; }
    if (code == juce::KeyPress::downKey || code == juce::KeyPress::leftKey)     { nudge (-step); return true; }

    return false;
}
}
