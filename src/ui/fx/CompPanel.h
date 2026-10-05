// CompPanel: the compressor's inline panel - level history with the turned-down amber, gain-reduction meter,
// transfer curve with draggable knee and ratio, the big Amount knob, and the six detailed knobs under "Details".
#pragma once

#include "Juce.h"
#include "engine/builtin/Compressor.h"
#include "ui/fx/BuiltinPanel.h"
#include "ui/widgets/Buttons.h"
#include "ui/widgets/Knob.h"

#include <array>
#include <memory>
#include <vector>

namespace hr
{
// One column of the history: what came in, what went out and how far the gain was pulled down, over ~15 ms.
struct HistoryColumn
{
    float inDb = -100.0f, outDb = -100.0f, grDb = 0.0f;        // grDb <= 0
};

// ---------------------------------------------------------------------------------------------------
// The transfer curve. -60...0 dB on both axes, drawn with Compressor::outputDb, so what you see is what the DSP does.
// Drag the round knee handle sideways = threshold; drag the line above it up or down = ratio (Shift = fine);
// double-click either one = back to its default. The dot rides the curve at the current input level.
// ---------------------------------------------------------------------------------------------------
class CompCurve final : public juce::Component,
                        public juce::SettableTooltipClient
{
public:
    explicit CompCurve (Compressor& comp);

    void setDimmed (bool shouldDim)                 { dimmed = shouldDim; repaint(); }
    void setDot (float inputDb);                    // smoothed detector level; <= -60 hides the dot

    juce::Point<float> kneeHandlePosition() const;  // centre of the round handle, in this component
    juce::Point<float> ratioGrabPoint() const;      // a point on the line above the knee where a ratio drag can start
    juce::Rectangle<float> getPlotBounds() const noexcept   { return plot; }
    bool isDotVisible() const noexcept             { return dotVisible; }
    bool isDotBright() const noexcept              { return dotBright; }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    static constexpr int kWidth = 168, kHeight = 160;
    static constexpr float kMinDb = -60.0f, kMaxDb = 0.0f;

private:
    enum class Target { none, knee, line };

    float xForDb (float db) const noexcept;
    float yForDb (float db) const noexcept;
    float threshold() const noexcept                { return pThreshold->load(); }
    float ratio() const noexcept                    { return pRatio->load(); }
    float knee() const noexcept                     { return pKnee->load(); }
    float makeup() const noexcept                   { return pMakeup->load(); }
    float curveDb (float inputDb) const noexcept;
    Target targetAt (juce::Point<float> p) const;
    juce::Rectangle<int> dotBounds() const;
    void rebuildStatic (int scale);
    void paintStatic (juce::Graphics&) const;
    void beginDrag (Target t, const juce::MouseEvent&);
    void endDrag();

    Compressor& comp;
    std::atomic<float> *pThreshold, *pRatio, *pKnee, *pMakeup;
    juce::RangedAudioParameter *rThreshold, *rRatio;
    std::unique_ptr<ParamWatcher> watcher;

    juce::Rectangle<float> plot;
    juce::Image staticImage;
    int staticScale = 0;
    std::array<float, 4> drawn {};                  // threshold, ratio, knee, makeup the cached image was drawn with
    bool staticDirty = true, dimmed = false;

    float dotDb = -100.0f;
    bool dotVisible = false, dotBright = false;

    Target hover = Target::none, dragging = Target::none;
    bool dragFine = false;
    float dragBaseValue = 0.0f;                     // threshold (dB) or normalised ratio when the drag (re)started
    juce::Point<float> dragBaseMouse;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CompCurve)
};

// ---------------------------------------------------------------------------------------------------
// About 5 s of history, scrolling left. Input as a filled low-alpha area, output as a lighter line, the gain reduction
// as an amber area hanging from the top edge (24 dB = the full height, like the meter beside it), the threshold as a
// dashed hairline. A ring of per-pixel columns is the truth; a cached image holds what was drawn and is shifted, so a
// frame costs a memmove plus a column or two instead of a path with hundreds of points.
// ---------------------------------------------------------------------------------------------------
class CompHistory final : public juce::Component,
                          public juce::SettableTooltipClient
{
public:
    explicit CompHistory (Compressor& comp);

    // New columns, oldest first. Repaints.
    void pushColumns (const HistoryColumn* columns, int count);
    void reset();
    void setDimmed (bool shouldDim)                 { dimmed = shouldDim; repaint(); }

    int getNumColumns() const noexcept              { return numColumns; }
    static constexpr float kSeconds = 5.0f;
    float getColumnsPerSecond() const noexcept      { return (float) numColumns / kSeconds; }

    // The column that was pushed most recently (test hook).
    const HistoryColumn& getNewestColumn() const noexcept   { return newest; }

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr float kFloorDb = -60.0f, kGrFullScaleDb = 24.0f;

private:
    float yForColumnDb (float db) const noexcept;
    void ensureImages (int scale);
    void drawColumn (juce::Graphics&, int x, const HistoryColumn&, const HistoryColumn* previous) const;
    void rebuildScroller();
    const HistoryColumn& columnAt (int fromLeft) const noexcept;

    Compressor& comp;
    std::atomic<float>* pThreshold;
    std::unique_ptr<ParamWatcher> watcher;

    juce::Rectangle<float> plot;
    int numColumns = 0, head = 0, imageScale = 0;
    std::vector<HistoryColumn> ring;
    HistoryColumn newest;
    juce::Image background, scroller;
    bool dimmed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CompHistory)
};

// ---------------------------------------------------------------------------------------------------
// Vertical gain-reduction bar, 0...-24 dB, 1.5 s peak hold, readout of the held peak in mono.
// ---------------------------------------------------------------------------------------------------
class GrMeter final : public juce::Component,
                      public juce::SettableTooltipClient
{
public:
    GrMeter();

    // `reductionDb` is the largest reduction since the last call as a positive number.
    void push (float reductionDb, double dt);
    void reset();
    void setDimmed (bool shouldDim)                 { dimmed = shouldDim; repaint(); }

    float getShownDb() const noexcept               { return shown; }
    float getHeldDb() const noexcept                { return held; }

    void paint (juce::Graphics&) override;

    static constexpr float kRangeDb = 24.0f;
    static constexpr int kBarInset = 6, kReadoutHeight = 18;

private:
    float shown = 0.0f, held = 0.0f, holdSeconds = 0.0f;
    int lastShownPx = -1, lastHeldPx = -1, lastHeldTenths = -1;
    bool dimmed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GrMeter)
};

// ---------------------------------------------------------------------------------------------------
// The Amount knob: a big dial for the `amount` parameter. Reads "Custom" once a detailed setting was edited.
// ---------------------------------------------------------------------------------------------------
class AmountKnob final : public juce::Component,
                         public juce::SettableTooltipClient
{
public:
    explicit AmountKnob (Compressor& comp);

    void setDimmed (bool shouldDim)                 { if (dimmed != shouldDim) { dimmed = shouldDim; repaint(); } }

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

    juce::Point<float> getDialCentre() const;
    juce::String getReadout() const;                        // "50%" or "Custom"
    juce::String getCaption() const;                        // a word for the amount, or how to get back from Custom

    static constexpr int kWidth = 120, kHeight = 140;

private:
    void nudge (float normalisedDelta);

    Compressor& comp;
    juce::RangedAudioParameter& param;
    juce::ParameterAttachment attachment;
    std::unique_ptr<ParamWatcher> watcher;
    float plainValue = 0.0f;
    bool hover = false, dimmed = false, dragging = false, dragFine = false;
    float dragStartNorm = 0.0f, dragStartY = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AmountKnob)
};

// "Details" row: a hairline, the word, and a chevron.
class DetailsToggle final : public juce::Button
{
public:
    DetailsToggle();
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;
};

// ---------------------------------------------------------------------------------------------------
class CompPanel final : public BuiltinPanel,
                        private juce::Timer
{
public:
    explicit CompPanel (Compressor& compressor);
    ~CompPanel() override;

    int getPreferredHeight() const override;
    void setDimmed (bool shouldDim) override;
    void tickMeter (double deltaSeconds) override;

    void setDetailsOpen (bool shouldOpen);
    bool isDetailsOpen() const noexcept             { return detailsOpen; }

    CompCurve& getCurve() noexcept                  { return curve; }
    CompHistory& getHistory() noexcept              { return history; }
    GrMeter& getGrMeter() noexcept                  { return grMeter; }
    AmountKnob& getAmountKnob() noexcept            { return amountKnob; }
    DetailsToggle& getDetailsToggle() noexcept      { return detailsToggle; }
    bool isTapActive() const noexcept               { return active; }

    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override               { updateActive(); }
    void parentHierarchyChanged() override          { updateActive(); }

    static constexpr int kHistoryHeight = 100, kGap = 8, kRowHeight = 160, kDetailsRow = 28;

private:
    void timerCallback() override                   { updateActive(); }
    void updateActive();
    bool isOnScreen() const;
    int heightWithDetails (bool open) const noexcept;

    Compressor& comp;
    CompHistory history;
    GrMeter grMeter;
    CompCurve curve;
    AmountKnob amountKnob;
    DetailsToggle detailsToggle;
    std::vector<std::unique_ptr<ParamKnob>> knobs;

    bool detailsOpen = false, dimmed = false, active = false;

    // Tap consumption: records wait here until the clock has produced enough columns for them.
    std::vector<Compressor::DisplayRecord> pending;
    std::vector<HistoryColumn> scratch;
    HistoryColumn lastColumn;
    float columnCarry = 0.0f, secondsSinceRecord = 10.0f, dotDb = -100.0f, lastGr = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CompPanel)
};
}
