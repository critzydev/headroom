// EqPanel: the EQ inside the FX drawer: the live graph with an Expand button that opens the full editor.
#pragma once

#include "Juce.h"
#include "engine/builtin/Equalizer.h"
#include "ui/fx/BuiltinPanel.h"
#include "ui/fx/EqFeed.h"
#include "ui/fx/EqGraph.h"
#include "ui/fx/EqSheet.h"

#include <memory>

namespace hr
{
// The compact view: spectrum (post-EQ), grid, per-band curves, the combined curve and the band nodes; numbers appear
// on hover. "Expand" opens EqSheet over the mixer. The panel owns the analyzer feed both views share.
class EqPanel final : public BuiltinPanel
{
public:
    explicit EqPanel (Equalizer& equalizer);
    ~EqPanel() override;

    int getPreferredHeight() const override     { return 4 + kGraphHeight + 8; }
    void setDimmed (bool shouldDim) override;

    void openExpanded();
    void closeExpanded (bool restoreFocus = true);
    bool isExpandedOpen() const noexcept        { return sheet != nullptr; }

    EqGraph& getGraph() noexcept                { return graph; }
    EqSheet* getSheet() noexcept                { return sheet.get(); }
    EqAnalyzerFeed& getFeed() noexcept          { return feed; }
    juce::Button& getExpandButton() noexcept;

    void resized() override;

    static constexpr int kGraphHeight = 176;

private:
    class ExpandButton;

    Equalizer& eq;
    EqAnalyzerFeed feed;
    EqGraph graph;
    std::unique_ptr<ExpandButton> expandButton;
    std::unique_ptr<EqSheet> sheet, retired;
    bool dimmed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqPanel)
};
}
