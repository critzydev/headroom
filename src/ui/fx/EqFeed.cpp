// EqAnalyzerFeed implementation.
#include "ui/fx/EqFeed.h"

#include "ui/fx/EqGraph.h"

#include <algorithm>

namespace hr
{
namespace
{
    constexpr int kBufferFrames = 4096;
    constexpr float kDisplayTiltDbPerOctave = 4.5f;
}

EqAnalyzerFeed::EqAnalyzerFeed (Equalizer& equalizer)
    : eq (equalizer), preBuffer ((size_t) kBufferFrames), postBuffer ((size_t) kBufferFrames)
{
    preAnalyzer.setTilt (kDisplayTiltDbPerOctave);
    postAnalyzer.setTilt (kDisplayTiltDbPerOctave);
    startTimerHz (kIdleHz);
}

EqAnalyzerFeed::~EqAnalyzerFeed()
{
    stopTimer();
    eq.analyzerActive.store (false, std::memory_order_relaxed);
}

void EqAnalyzerFeed::addView (EqGraph& view)
{
    if (std::find (views.begin(), views.end(), &view) == views.end())
        views.push_back (&view);
}

void EqAnalyzerFeed::removeView (EqGraph& view)
{
    views.erase (std::remove (views.begin(), views.end(), &view), views.end());
}

bool EqAnalyzerFeed::anyViewShowing() const
{
    for (auto* view : views)
        if (view->isSpectrumLive (forceLive))
            return true;

    return false;
}

void EqAnalyzerFeed::setActive (bool shouldBeActive)
{
    if (shouldBeActive == active)
        return;

    active = shouldBeActive;

    if (active)
    {
        // Start from "now": whatever is in the FIFO is old, and so is what the analyzers remember.
        eq.getAnalyzerFifo().clear();
        preAnalyzer.reset();
        postAnalyzer.reset();
        lastMs = juce::Time::getMillisecondCounterHiRes();
    }

    eq.analyzerActive.store (active, std::memory_order_relaxed);
    startTimerHz (active ? kActiveHz : kIdleHz);
}

void EqAnalyzerFeed::getColumns (Source source, float* out, int numColumns, float minHz, float maxHz) const
{
    (source == Source::pre ? preAnalyzer : postAnalyzer).getColumns (out, numColumns, minHz, maxHz);
}

void EqAnalyzerFeed::tick()
{
    setActive (anyViewShowing());

    if (! active)
        return;

    const double rate = eq.getPreparedRate();

    if (rate != preparedRate)
    {
        preparedRate = rate;
        preAnalyzer.prepare (rate);
        postAnalyzer.prepare (rate);
        preAnalyzer.setTilt (kDisplayTiltDbPerOctave);
        postAnalyzer.setTilt (kDisplayTiltDbPerOctave);
    }

    const auto now = juce::Time::getMillisecondCounterHiRes();
    const double elapsed = juce::jlimit (0.0, 0.25, (now - lastMs) / 1000.0);
    lastMs = now;

    int total = 0, got;

    while ((got = eq.getAnalyzerFifo().read (preBuffer.data(), postBuffer.data(), kBufferFrames)) > 0)
    {
        preAnalyzer.push (preBuffer.data(), got);
        postAnalyzer.push (postBuffer.data(), got);
        total += got;
    }

    if (total == 0)
    {
        // The audio thread sent nothing (the effect is switched off, or the engine stopped): let the levels fall away.
        std::fill (preBuffer.begin(), preBuffer.end(), 0.0f);
        int left = (int) (elapsed * rate);

        while (left > 0)
        {
            const int n = juce::jmin (left, kBufferFrames);
            preAnalyzer.push (preBuffer.data(), n);
            postAnalyzer.push (preBuffer.data(), n);
            left -= n;
        }
    }

    for (auto* view : views)
        view->spectrumUpdated();
}
}
