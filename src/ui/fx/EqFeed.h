// EqAnalyzerFeed: turns the EQ's audio-thread FIFO into spectrum columns for the views, and only while one is on screen.
#pragma once

#include "Juce.h"
#include "engine/builtin/Equalizer.h"
#include "engine/dsp/SpectrumAnalyzer.h"

#include <vector>

namespace hr
{
class EqGraph;

// One per EQ panel, shared by every view of that EQ (the inline curve and the expanded editor use the same spectra, so
// the FFT runs once). A message-thread timer runs at 30 Hz while at least one registered view is showing; it sets
// `Equalizer::analyzerActive` (so the audio thread only pushes samples while somebody looks), drains the FIFO into a
// pre- and a post-EQ analyzer and tells the views to repaint their spectrum. While nothing is showing the timer only
// polls at 5 Hz to notice that something is, and the audio thread does nothing extra.
class EqAnalyzerFeed final : private juce::Timer
{
public:
    enum class Source { post, pre };

    static constexpr int kActiveHz = 30, kIdleHz = 5;

    explicit EqAnalyzerFeed (Equalizer& equalizer);
    ~EqAnalyzerFeed() override;

    Equalizer& getEqualizer() noexcept              { return eq; }

    void addView (EqGraph& view);
    void removeView (EqGraph& view);

    // One value per horizontal pixel of a log axis (see SpectrumAnalyzer::getColumns).
    void getColumns (Source source, float* out, int numColumns, float minHz, float maxHz) const;

    bool isActive() const noexcept                  { return active; }

    // Screenshot mode has no window on screen, so nothing "is showing": this makes the feed run anyway.
    void setForceLive (bool shouldForce) noexcept   { forceLive = shouldForce; }
    void tick();                                    // one step now (the timer calls it; screenshots and tests drive it by hand)

private:
    void timerCallback() override                   { tick(); }
    bool anyViewShowing() const;
    void setActive (bool shouldBeActive);

    Equalizer& eq;
    std::vector<EqGraph*> views;
    dsp::SpectrumAnalyzer preAnalyzer, postAnalyzer;
    std::vector<float> preBuffer, postBuffer;
    double preparedRate = 0.0, lastMs = 0.0;
    bool active = false, forceLive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqAnalyzerFeed)
};
}
