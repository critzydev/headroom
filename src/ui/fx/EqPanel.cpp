// EqPanel implementation: the inline graph, the Expand button, and the sheet's lifetime.
#include "ui/fx/EqPanel.h"

#include "ui/Focus.h"
#include "ui/HeadroomLookAndFeel.h"
#include "ui/Icons.h"
#include "ui/MainComponent.h"
#include "ui/Theme.h"
#include "ui/widgets/HoverFade.h"

namespace hr
{
// A small "Expand" pill in the corner of the graph.
class EqPanel::ExpandButton final : public juce::Button
{
public:
    ExpandButton() : juce::Button ("Expand")
    {
        setMouseClickGrabsKeyboardFocus (false);
        setWantsKeyboardFocus (true);
        setTooltip ("Open the full EQ editor");
        HoverFade::attach (*this);
    }

    static constexpr int kWidth = 74, kHeight = 22;

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        const auto bounds = getLocalBounds().toFloat();
        const float hover = down ? 1.0f : HoverFade::get (*this, highlighted);

        g.setColour (theme::bg1.withAlpha (0.88f).interpolatedWith (theme::bg3, hover * 0.8f));
        g.fillRoundedRectangle (bounds, 4.0f);

        if (hasKeyboardFocus (false))
            HeadroomLookAndFeel::drawFocusRing (g, bounds, 4.0f);
        else
        {
            g.setColour (theme::line.interpolatedWith (theme::lineStrong, hover));
            g.drawRoundedRectangle (bounds.reduced (0.5f), 3.5f, 1.0f);
        }

        const auto colour = theme::text1.interpolatedWith (theme::text0, hover);
        Icons::draw (g, "maximize-2", juce::Point<float> (bounds.getX() + 12.0f, bounds.getCentreY()), 12.0f, colour);
        g.setColour (colour);
        g.setFont (theme::sans (11.0f, theme::Sans::Medium));
        g.drawText ("Expand", bounds.withTrimmedLeft (22.0f), juce::Justification::centredLeft, false);
    }
};

EqPanel::EqPanel (Equalizer& equalizer)
    : eq (equalizer), feed (equalizer), graph (equalizer, feed, EqGraph::Mode::compact)
{
    addAndMakeVisible (graph);

    expandButton = std::make_unique<ExpandButton>();
    expandButton->onClick = [this] { openExpanded(); };
    addAndMakeVisible (*expandButton);

    setSize (kWidth, getPreferredHeight());
}

EqPanel::~EqPanel()
{
    closeExpanded (false);
    retired.reset();
}

juce::Button& EqPanel::getExpandButton() noexcept
{
    return *expandButton;
}

void EqPanel::setDimmed (bool shouldDim)
{
    dimmed = shouldDim;
    graph.setDimmed (shouldDim);

    if (sheet != nullptr)
        sheet->setDimmed (shouldDim);
}

void EqPanel::resized()
{
    graph.setBounds (0, 4, kWidth, kGraphHeight);

    // Top left of the plot: the corner a voice EQ has least to say in (nothing is boosted at 20 - 40 Hz), and clear of
    // the labels that appear next to the nodes.
    const auto plot = graph.getPlotBounds();
    expandButton->setBounds (juce::roundToInt ((float) graph.getX() + plot.getX()) + 6,
                             juce::roundToInt ((float) graph.getY() + plot.getY()) + 6, ExpandButton::kWidth, ExpandButton::kHeight);
}

void EqPanel::openExpanded()
{
    if (sheet != nullptr)
        return;

    auto* host = findParentComponentOfClass<MainComponent>();

    if (host == nullptr)
        return;

    juce::Component::SafePointer<EqPanel> safe (this);
    sheet = std::make_unique<EqSheet> (eq, feed, graph.getSelectedBand(), [safe] { if (safe != nullptr) safe->closeExpanded(); });
    sheet->setDimmed (dimmed);

    // The compact graph is under the sheet: it stops working until the sheet closes.
    graph.setSuspended (true);

    host->addAndMakeVisible (*sheet);
    sheet->setBounds (host->getLocalBounds());
    sheet->toFront (true);
    focusIfShowing (*sheet);
}

void EqPanel::closeExpanded (bool restoreFocus)
{
    if (sheet == nullptr)
        return;

    graph.setSuspended (false);
    graph.setRangeDb (EqGraph::getSharedRangeDb());
    graph.setSelectedBand (sheet->getGraph().getSelectedBand());

    if (auto* host = sheet->getParentComponent())
        host->removeChildComponent (sheet.get());

    // Usually called from the sheet's own close button or key handler: let that callback finish before the sheet is
    // destroyed. It is parked here meanwhile (its graph points at our analyzer feed, so it must not outlive us).
    retired = std::move (sheet);

    juce::Component::SafePointer<EqPanel> safe (this);
    juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->retired.reset(); });

    if (restoreFocus && isShowing())
        if (auto* drawer = findParentComponentOfClass<FxDrawer>())
            focusIfShowing (*drawer);
}
}
