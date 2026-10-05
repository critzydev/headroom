// EqGraph: the EQ plot (live spectrum, per-band curves, combined curve, draggable band nodes), inline and expanded.
#pragma once

#include "Juce.h"
#include "engine/builtin/Equalizer.h"
#include "ui/fx/BuiltinPanel.h"
#include "ui/fx/EqFeed.h"

#include <array>
#include <memory>
#include <vector>

namespace hr
{
// Log frequency axis 20 Hz - 20 kHz, linear dB axis (+-6 ... +-24 dB). Everything drawn comes from the Equalizer: the
// curves from dsp::eq::design (the function the DSP runs), the spectrum from the analyzer feed.
//
// What it costs: the background, grid and labels are painted once into an Image (per size and scale) and blitted; only the
// plot area repaints, at 30 fps while the spectrum is live and only when a band changes otherwise. Curves are Paths with
// one point per 2 px, and a band's curve is recomputed only when one of that band's parameters changes.
//
// Interaction (both sizes): drag a node for frequency (x) and gain (y); Shift = fine; wheel over a node = Q;
// double-click a node = flat (no gain: default Q); double-click empty = switch on the nearest free band there (as a
// peak); right-click a node = type menu; Alt+drag = solo the band while held.
class EqGraph final : public juce::Component
{
public:
    enum class Mode { compact, expanded };

    static constexpr int kBands = Equalizer::kNumBands;
    static constexpr float kMinHz = 20.0f, kMaxHz = 20000.0f;

    EqGraph (Equalizer& equalizer, EqAnalyzerFeed& analyzerFeed, Mode graphMode);
    ~EqGraph() override;

    int getSelectedBand() const noexcept        { return selectedBand; }
    void setSelectedBand (int band);
    std::function<void (int)> onBandSelected;

    void setSpectrumSource (EqAnalyzerFeed::Source source);
    EqAnalyzerFeed::Source getSpectrumSource() const noexcept   { return spectrumSource; }

    // Vertical range of the dB axis, one of 6 / 12 / 18 / 24. Shared by every graph (a person picks it once).
    void setRangeDb (float newRange);
    float getRangeDb() const noexcept           { return rangeDb; }
    static float getSharedRangeDb() noexcept;
    static void setSharedRangeDb (float newRange) noexcept;

    void setDimmed (bool shouldDim);            // the effect is switched off
    void setSuspended (bool shouldSuspend);     // something covers it: no spectrum work
    bool isSpectrumLive (bool ignoreShowing = false) const  { return ! suspended && (ignoreShowing || isShowing()); }
    void spectrumUpdated();                     // from the feed, 30 Hz

    // Geometry, in this component's coordinates (also what the smoke test drives).
    juce::Rectangle<float> getPlotBounds() const noexcept   { return plot; }
    juce::Point<float> getNodePosition (int band) const noexcept;
    float xForFreq (float hz) const noexcept;
    float freqForX (float x) const noexcept;
    float yForDb (float db) const noexcept;
    float yForDbOffScale (float db) const noexcept;
    float dbForY (float y) const noexcept;

    void applyMenuResult (int band, int menuItemId);         // what picking an item of the band menu does (the smoke test calls it)

    static juce::String formatFrequency (float hz);
    static juce::String describeBand (int band, const Equalizer::BandSettings& s);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    // Statistics for the smoke test and the paint-time measurement.
    int getNumCurvePoints() const noexcept      { return (int) curveFreq.size(); }

private:
    struct Metrics
    {
        float left, right, top, bottom, nodeRadius, labelFont, hitRadius, bandStroke, totalStroke;
    };

    struct BandParams
    {
        juce::RangedAudioParameter *on = nullptr, *type = nullptr, *freq = nullptr, *gain = nullptr, *q = nullptr, *slope = nullptr;
    };

    static Metrics metricsFor (Mode m) noexcept;

    float nodeDb (int band) const noexcept;
    juce::Point<float> nodePoint (int band) const noexcept;
    int bandAt (juce::Point<float> p) const;
    juce::Colour colourFor (int band) const noexcept;

    void refreshBand (int band);
    void refreshAllBands();
    void rebuildBandPath (int band);
    void rebuildTotalPath();
    void rebuildSpectrumPaths();
    void rebuildGridCache (float scale);
    void repaintPlot();

    void beginEdit (int band);
    void endEdit();
    void showBandMenu (int band);
    void addBandAt (juce::Point<float> p);
    void select (int band);

    Equalizer& eq;
    EqAnalyzerFeed& feed;
    const Mode mode;
    const Metrics m;

    std::array<BandParams, kBands> params;
    std::array<std::unique_ptr<ParamWatcher>, kBands> watchers;
    std::array<Equalizer::BandSettings, kBands> settings;
    std::array<float, kBands> cutDb {};                     // where a cut's own curve is at its corner (the node's height)

    // Curves: one point every 2 px.
    std::vector<float> curveFreq, curveX;
    std::array<std::vector<float>, kBands> bandDb;
    std::vector<float> totalDb;
    std::array<juce::Path, kBands> bandPaths;
    juce::Path totalPath, bandFill;
    double curveRate = 0.0;

    // Spectrum: one value per pixel column.
    std::vector<float> spectrumDb;
    juce::Path spectrumFill, spectrumTop;
    bool spectrumHasData = false;
    EqAnalyzerFeed::Source spectrumSource = EqAnalyzerFeed::Source::post;

    juce::Rectangle<float> plot;
    juce::Image gridCache;
    float cacheScale = 0.0f, cacheRange = 0.0f;
    int cacheW = 0, cacheH = 0;

    float rangeDb = 18.0f;
    int selectedBand = 0, hoverBand = -1, dragBand = -1;
    bool dimmed = false, suspended = false, soloing = false, dragStartedOn = false;
    juce::Point<float> virtualPos, lastPointer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqGraph)
};
}
