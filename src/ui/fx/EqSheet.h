// EqSheet: the expanded EQ editor. A dimmed overlay over the mixer with the big graph and one strip of controls per band.
#pragma once

#include "Juce.h"
#include "engine/builtin/Equalizer.h"
#include "ui/fx/EqFeed.h"
#include "ui/fx/EqGraph.h"
#include "ui/widgets/Buttons.h"

#include <array>
#include <functional>
#include <memory>

namespace hr
{
class EqBandColumn;

enum class EqValueKind { frequency, gain, q };

// What a person types into a band's number field: "2.4k", "2400 Hz", "-3.5", "+3 dB", "1,2", "Q 0.7". Returns false
// if there is no number in it; the caller clamps the result to the parameter's range.
bool parseEqValue (EqValueKind kind, const juce::String& text, float& value);

// Added as a child of the main window (it follows the window's size), closed with Esc, the x, or a click outside the
// panel. Under the graph: for each of the eight bands a colour chip that switches it on and off, a solo button, the
// type, and frequency / gain / Q in JetBrains Mono, editable by dragging, with the wheel, or by double-clicking and typing.
class EqSheet final : public juce::Component,
                      private juce::Timer
{
public:
    EqSheet (Equalizer& equalizer, EqAnalyzerFeed& analyzerFeed, int initialBand, std::function<void()> onCloseRequested);
    ~EqSheet() override;

    EqGraph& getGraph() noexcept                { return graph; }
    juce::Rectangle<int> getPanelBounds() const noexcept   { return panel; }
    void setDimmed (bool shouldDim)             { graph.setDimmed (shouldDim); }

    // As if the number field of a band had been double-clicked, `text` typed and Return pressed (smoke test).
    bool enterValue (int band, EqValueKind kind, const juce::String& text);

    void paint (juce::Graphics&) override;
    void resized() override;
    void parentSizeChanged() override;
    void mouseDown (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

    static constexpr int kPadding = 20, kHeaderHeight = 52, kStripHeight = 176;

private:
    void timerCallback() override;

    Equalizer& eq;
    EqAnalyzerFeed& feed;
    std::function<void()> onClose;

    IconButton closeButton { "x", 14.0f };
    SegmentedControl sourceControl, rangeControl;
    EqGraph graph;
    std::array<std::unique_ptr<EqBandColumn>, EqGraph::kBands> columns;

    juce::Rectangle<int> panel, headerArea, stripArea;
    int lastSolo = -2;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqSheet)
};
}
