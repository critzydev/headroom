// LevelFader: the channel / master fader with dB scale, stereo meter behind the cap and a dB readout.
#pragma once

#include "Juce.h"
#include "ui/widgets/Meter.h"

#include <functional>

namespace hr
{
// Range -inf ... +6 dB, 0 dB at 80 % of the travel. Drag (Shift = fine), double-click = 0 dB, wheel = +-1 dB,
// arrow keys when focused. The meter is drawn in the track, behind the cap, so the eye finds level and setting together.
class LevelFader final : public juce::Component,
                         public juce::SettableTooltipClient
{
public:
    LevelFader();

    // Model -> UI. Never calls onChange.
    void setDb (float db);
    float getDb() const noexcept                    { return db; }
    void setMuted (bool isMuted);
    void setReadoutVisible (bool visible);

    // Called at 30 fps with the current display values; repaints only the track column, and only if a bar moved.
    void setLevels (const MeterBar& left, const MeterBar& right);
    void clearLevels();

    // UI -> model: the new gain in dB (kMinusInfDb for -inf). Fires on every drag step.
    std::function<void (float)> onChange;

    // dB <-> fader travel (0..1). Exposed so the scale and meter share one mapping.
    static float travelToDb (float travel) noexcept;
    static float dbToTravel (float db) noexcept;

    juce::Rectangle<int> getCapBounds() const noexcept;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusGained (FocusChangeType) override     { repaint(); }
    void focusLost (FocusChangeType) override       { repaint(); }

private:
    float travelToY (float travel) const noexcept;
    float yToTravel (float y) const noexcept;
    juce::Rectangle<int> getTrackBounds() const noexcept;
    bool isInFaderColumn (juce::Point<int>) const noexcept;
    void userSetDb (float newDb);
    void drawScale (juce::Graphics&) const;
    void drawMeter (juce::Graphics&) const;
    void drawCap (juce::Graphics&) const;

    float db = 0.0f;
    float travel = 0.0f;
    bool muted = false, showReadout = true, hoverCap = false, dragging = false;
    MeterBar meterL, meterR;
    int lastBarPx[4] { -1, -1, -1, -1 };

    // geometry (resized)
    int labelRight = 0, tickLeft = 0, capLeft = 0, capRight = 0, travelTop = 0, travelBottom = 0, readoutHeight = 20;

    // drag state
    float dragStartTravel = 0.0f, dragStartY = 0.0f;
    bool dragFine = false;
    float wheelAccumulator = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LevelFader)
};
}
