// EqGraph implementation.
#include "ui/fx/EqGraph.h"

#include "ui/Format.h"
#include "ui/Theme.h"

#include <atomic>
#include <cmath>
#include <iterator>

namespace hr
{
namespace
{
    constexpr float kStepPx = 2.0f;                         // one curve point every 2 px
    constexpr float kSpectrumTopDb = -12.0f;                // the spectrum's window: a soft shape, not a scale to read numbers from
    constexpr float kSpectrumFloorDb = -84.0f;
    constexpr float kWheelStep = 1.10f, kWheelStepFine = 1.03f;

    std::atomic<float> sharedRange { 18.0f };

    void setPlain (juce::RangedAudioParameter* p, float plainValue)
    {
        if (p != nullptr)
            p->setValueNotifyingHost (p->convertTo0to1 (plainValue));
    }

    void setComplete (juce::RangedAudioParameter* p, float plainValue)
    {
        if (p != nullptr)
        {
            p->beginChangeGesture();
            setPlain (p, plainValue);
            p->endChangeGesture();
        }
    }

    float plainOf (juce::RangedAudioParameter* p)
    {
        return p->convertFrom0to1 (p->getValue());
    }

    // "2.40" -> "2.4", "12.0" -> "12".
    juce::String trimmed (double value, int decimals)
    {
        auto text = juce::String (value, decimals);

        if (text.contains ("."))
            text = text.trimCharactersAtEnd ("0").trimCharactersAtEnd (".");

        return text;
    }

    juce::String dbLabel (int db)
    {
        return db == 0 ? juce::String ("0") : (db > 0 ? juce::String ("+") : theme::glyph::minus) + juce::String (std::abs (db));
    }
}

EqGraph::Metrics EqGraph::metricsFor (Mode mode) noexcept
{
    if (mode == Mode::compact)
        return { 30.0f, 8.0f, 8.0f, 18.0f, 5.0f, 9.0f, 13.0f, 1.0f, 2.0f };

    return { 44.0f, 14.0f, 14.0f, 24.0f, 6.5f, 10.0f, 15.0f, 1.25f, 2.25f };
}

float EqGraph::getSharedRangeDb() noexcept          { return sharedRange.load(); }
void EqGraph::setSharedRangeDb (float r) noexcept   { sharedRange.store (r); }

EqGraph::EqGraph (Equalizer& equalizer, EqAnalyzerFeed& analyzerFeed, Mode graphMode)
    : eq (equalizer), feed (analyzerFeed), mode (graphMode), m (metricsFor (graphMode)), rangeDb (getSharedRangeDb())
{
    for (int b = 0; b < kBands; ++b)
    {
        auto& p = params[(size_t) b];
        auto find = [this, b] (const char* what) { return eq.apvts.getParameter (Equalizer::bandParamId (b, what)); };

        p.on = find ("On");
        p.type = find ("Type");
        p.freq = find ("Freq");
        p.gain = find ("Gain");
        p.q = find ("Q");
        p.slope = find ("Slope");

        watchers[(size_t) b] = std::make_unique<ParamWatcher> (std::vector<juce::RangedAudioParameter*> { p.on, p.type, p.freq, p.gain, p.q, p.slope },
                                                               [this, b] { refreshBand (b); repaintPlot(); });
        settings[(size_t) b] = eq.getBandSettings (b);
    }

    setMouseClickGrabsKeyboardFocus (false);
    setOpaque (false);
    feed.addView (*this);
}

EqGraph::~EqGraph()
{
    feed.removeView (*this);

    if (soloing)
        eq.setSoloBand (-1);
}

// ---------------------------------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------------------------------

float EqGraph::xForFreq (float hz) const noexcept
{
    const float t = std::log10 (juce::jlimit (kMinHz, kMaxHz, hz) / kMinHz) / 3.0f;
    return plot.getX() + t * plot.getWidth();
}

float EqGraph::freqForX (float x) const noexcept
{
    const float t = juce::jlimit (0.0f, 1.0f, (x - plot.getX()) / juce::jmax (1.0f, plot.getWidth()));
    return kMinHz * std::pow (10.0f, 3.0f * t);
}

float EqGraph::yForDb (float db) const noexcept
{
    return plot.getCentreY() - juce::jlimit (-rangeDb, rangeDb, db) / rangeDb * (plot.getHeight() * 0.5f);
}

// For the curves: they run off the plot instead of being flattened along its edge (the plot clips them).
float EqGraph::yForDbOffScale (float db) const noexcept
{
    return plot.getCentreY() - juce::jlimit (-4.0f * rangeDb, 4.0f * rangeDb, db) / rangeDb * (plot.getHeight() * 0.5f);
}

float EqGraph::dbForY (float y) const noexcept
{
    return juce::jlimit (-rangeDb, rangeDb, (plot.getCentreY() - y) / (plot.getHeight() * 0.5f) * rangeDb);
}

juce::Colour EqGraph::colourFor (int band) const noexcept
{
    return theme::channelTags[(size_t) juce::jlimit (0, kBands - 1, band)];
}

// A node sits at the band's frequency and gain. Filters without a gain sit on their own curve (a cut at its corner) or on the
// 0 dB line; a band that is off waits on the 0 dB line.
float EqGraph::nodeDb (int band) const noexcept
{
    const auto& s = settings[(size_t) band];

    if (! s.on)
        return 0.0f;

    if (Equalizer::hasGain (s.type))
        return s.gainDb;

    return Equalizer::isCut (s.type) ? cutDb[(size_t) band] : 0.0f;
}

juce::Point<float> EqGraph::nodePoint (int band) const noexcept
{
    return { xForFreq (settings[(size_t) band].freq), yForDb (nodeDb (band)) };
}

juce::Point<float> EqGraph::getNodePosition (int band) const noexcept
{
    return nodePoint (juce::jlimit (0, kBands - 1, band));
}

int EqGraph::bandAt (juce::Point<float> p) const
{
    int best = -1;
    float bestDistance = m.hitRadius;

    for (int i = 0; i < kBands; ++i)
    {
        // A band that is on wins over one that is off underneath it.
        const float d = nodePoint (i).getDistanceFrom (p) + (settings[(size_t) i].on ? 0.0f : 3.0f);

        if (d < bestDistance)
        {
            bestDistance = d;
            best = i;
        }
    }

    return best;
}

// ---------------------------------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------------------------------

void EqGraph::resized()
{
    plot = getLocalBounds().toFloat().withTrimmedLeft (m.left).withTrimmedRight (m.right).withTrimmedTop (m.top).withTrimmedBottom (m.bottom);

    const int points = plot.getWidth() > 1.0f ? (int) std::ceil (plot.getWidth() / kStepPx) + 1 : 2;
    curveFreq.assign ((size_t) points, 0.0f);
    curveX.assign ((size_t) points, 0.0f);
    totalDb.assign ((size_t) points, 0.0f);

    for (int i = 0; i < points; ++i)
    {
        curveX[(size_t) i] = plot.getX() + plot.getWidth() * (float) i / (float) (points - 1);
        curveFreq[(size_t) i] = freqForX (curveX[(size_t) i]);
    }

    for (auto& path : bandPaths)
        path.preallocateSpace (points * 3 + 8);

    totalPath.preallocateSpace (points * 3 + 8);

    const int columns = juce::jmax (1, (int) plot.getWidth());
    spectrumDb.assign ((size_t) columns, dsp::SpectrumAnalyzer::kFloorDb);
    spectrumFill.preallocateSpace (columns * 3 + 16);
    spectrumTop.preallocateSpace (columns * 3 + 16);
    spectrumHasData = false;

    if (feed.isActive() && ! suspended)          // no blank frame while the window is being resized
    {
        feed.getColumns (spectrumSource, spectrumDb.data(), columns, kMinHz, kMaxHz);
        rebuildSpectrumPaths();
    }

    refreshAllBands();
    gridCache = {};
}

void EqGraph::refreshAllBands()
{
    curveRate = eq.getPreparedRate();

    for (int b = 0; b < kBands; ++b)
        refreshBand (b);
}

void EqGraph::refreshBand (int band)
{
    auto& s = settings[(size_t) band];
    s = eq.getBandSettings (band);

    const double rate = eq.getPreparedRate();

    if (rate != curveRate)
    {
        // The sample rate changed: every curve is a little different.
        curveRate = rate;

        for (int b = 0; b < kBands; ++b)
            if (b != band)
                settings[(size_t) b] = eq.getBandSettings (b);
    }

    const int points = (int) curveFreq.size();

    if (points > 0)
    {
        bandDb[(size_t) band].resize ((size_t) points);
        eq.getBandResponseDb (band, curveFreq.data(), bandDb[(size_t) band].data(), points, rate);
    }

    cutDb[(size_t) band] = s.on && Equalizer::isCut (s.type) ? eq.getBandResponseDb (band, s.freq, rate) : 0.0f;

    rebuildBandPath (band);
    rebuildTotalPath();
}

// A band's own curve is drawn only where it does something (more than a tenth of a dB from flat): eight thin lines lying on
// top of the 0 dB line would only make it muddy.
void EqGraph::rebuildBandPath (int band)
{
    auto& path = bandPaths[(size_t) band];
    path.clear();

    if (! settings[(size_t) band].on || bandDb[(size_t) band].size() != curveX.size() || curveX.empty())
        return;

    const auto& db = bandDb[(size_t) band];
    const size_t n = db.size();
    constexpr float kFlatDb = 0.1f;

    auto visible = [&] (size_t i) { return std::abs (db[i]) > kFlatDb; };
    bool drawing = false;

    for (size_t i = 0; i < n; ++i)
    {
        // A point is kept if it or a neighbour departs from flat, so a line starts and ends on the 0 dB line.
        const bool keep = visible (i) || (i > 0 && visible (i - 1)) || (i + 1 < n && visible (i + 1));

        if (! keep)
        {
            drawing = false;
            continue;
        }

        if (drawing)
            path.lineTo (curveX[i], yForDbOffScale (db[i]));
        else
            path.startNewSubPath (curveX[i], yForDbOffScale (db[i]));

        drawing = true;
    }
}

void EqGraph::rebuildTotalPath()
{
    totalPath.clear();

    if (curveX.empty())
        return;

    std::fill (totalDb.begin(), totalDb.end(), 0.0f);

    for (int b = 0; b < kBands; ++b)
        if (settings[(size_t) b].on && bandDb[(size_t) b].size() == totalDb.size())
            for (size_t i = 0; i < totalDb.size(); ++i)
                totalDb[i] += bandDb[(size_t) b][i];

    totalPath.startNewSubPath (curveX[0], yForDbOffScale (totalDb[0]));

    for (size_t i = 1; i < curveX.size(); ++i)
        totalPath.lineTo (curveX[i], yForDbOffScale (totalDb[i]));
}

void EqGraph::rebuildSpectrumPaths()
{
    spectrumFill.clear();
    spectrumTop.clear();

    const int columns = (int) spectrumDb.size();
    const float bottom = plot.getBottom();
    const float height = plot.getHeight();

    bool any = false;

    auto yFor = [&] (float db)
    {
        const float t = juce::jlimit (0.0f, 1.0f, (db - kSpectrumFloorDb) / (kSpectrumTopDb - kSpectrumFloorDb));
        any = any || t > 0.004f;
        return bottom - t * height;
    };

    spectrumFill.startNewSubPath (plot.getX(), bottom);

    for (int i = 0; i < columns; ++i)
    {
        const float x = plot.getX() + (float) i + 0.5f;
        const float y = yFor (spectrumDb[(size_t) i]);

        if (i == 0)
            spectrumTop.startNewSubPath (x, y);
        else
            spectrumTop.lineTo (x, y);

        spectrumFill.lineTo (x, y);
    }

    spectrumFill.lineTo (plot.getX() + (float) columns, bottom);
    spectrumFill.closeSubPath();
    spectrumHasData = any;
}

void EqGraph::spectrumUpdated()
{
    if (suspended || spectrumDb.empty())
        return;

    feed.getColumns (spectrumSource, spectrumDb.data(), (int) spectrumDb.size(), kMinHz, kMaxHz);
    rebuildSpectrumPaths();
    repaintPlot();
}

void EqGraph::repaintPlot()
{
    repaint (plot.getSmallestIntegerContainer().expanded (8, 8).getIntersection (getLocalBounds()));
}

void EqGraph::setSpectrumSource (EqAnalyzerFeed::Source source)
{
    if (source == spectrumSource)
        return;

    spectrumSource = source;
    spectrumUpdated();
}

void EqGraph::setRangeDb (float newRange)
{
    if (newRange == rangeDb)
        return;

    rangeDb = newRange;
    setSharedRangeDb (newRange);
    gridCache = {};

    for (int b = 0; b < kBands; ++b)
        rebuildBandPath (b);

    rebuildTotalPath();
    repaint();
}

void EqGraph::setDimmed (bool shouldDim)
{
    if (dimmed != shouldDim)
    {
        dimmed = shouldDim;
        repaint();
    }
}

void EqGraph::setSuspended (bool shouldSuspend)
{
    suspended = shouldSuspend;
}

void EqGraph::setSelectedBand (int band)
{
    select (juce::jlimit (0, kBands - 1, band));
}

void EqGraph::select (int band)
{
    if (band == selectedBand)
        return;

    selectedBand = band;
    repaint();

    if (onBandSelected)
        onBandSelected (band);
}

// ---------------------------------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------------------------------

juce::String EqGraph::formatFrequency (float hz)
{
    if (hz < 100.0f)    return trimmed (hz, 1) + " Hz";
    if (hz < 1000.0f)   return juce::String (juce::roundToInt (hz)) + " Hz";
    if (hz < 10000.0f)  return trimmed (hz / 1000.0, 2) + " kHz";
    return trimmed (hz / 1000.0, 1) + " kHz";
}

juce::String EqGraph::describeBand (int band, const Equalizer::BandSettings& s)
{
    const auto& dot = theme::glyph::dot;
    const juce::String sep = " " + dot + " ";

    juce::String text = "Band " + juce::String (band + 1) + sep + Equalizer::typeName (s.type) + sep + formatFrequency (s.freq);

    if (Equalizer::hasGain (s.type))
        text << sep << fmt::db (s.gainDb, true, true);

    if (Equalizer::isCut (s.type))
        text << sep << Equalizer::slopeName (s.slope);

    if (! (Equalizer::isCut (s.type) && s.slope == 0))
        text << sep << "Q " << trimmed (s.q, s.q < 10.0f ? 2 : 1);

    if (! s.on)
        text << sep << "off";

    return text;
}

// ---------------------------------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------------------------------

// Background, grid and labels: everything that only changes with the size or the dB range.
void EqGraph::rebuildGridCache (float scale)
{
    const int w = getWidth(), h = getHeight();

    if (w <= 0 || h <= 0)
        return;

    gridCache = juce::Image (juce::Image::ARGB, juce::jmax (1, juce::roundToInt ((float) w * scale)), juce::jmax (1, juce::roundToInt ((float) h * scale)), true);
    juce::Graphics g (gridCache);
    g.addTransform (juce::AffineTransform::scale ((float) gridCache.getWidth() / (float) w, (float) gridCache.getHeight() / (float) h));

    g.setColour (theme::bg2);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), theme::kControlRadius);

    const bool big = mode == Mode::expanded;
    g.setFont (theme::mono (m.labelFont));

    // Frequencies: the labelled lines, thin and quiet; decades slightly stronger.
    static const float compactFreqs[] = { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f };
    static const float bigFreqs[] = { 30.0f, 50.0f, 100.0f, 200.0f, 300.0f, 500.0f, 1000.0f, 2000.0f, 3000.0f, 5000.0f, 10000.0f, 20000.0f };

    auto drawFreqs = [&] (const float* freqs, int count)
    {
        for (int i = 0; i < count; ++i)
        {
            const float f = freqs[i];
            const float x = std::round (xForFreq (f)) + 0.5f;
            const bool decade = f == 100.0f || f == 1000.0f || f == 10000.0f;

            g.setColour (decade ? theme::lineStrong.withAlpha (0.7f) : theme::line);
            g.fillRect (x - 0.5f, plot.getY(), 1.0f, plot.getHeight());

            const auto label = f >= 1000.0f ? juce::String (juce::roundToInt (f / 1000.0f)) + "k" : juce::String (juce::roundToInt (f));
            g.setColour (theme::text2);
            g.drawText (label, juce::Rectangle<float> (x - 16.0f, plot.getBottom() + (big ? 5.0f : 3.0f), 32.0f, 12.0f), juce::Justification::centred, false);
        }
    };

    if (big)
        drawFreqs (bigFreqs, (int) std::size (bigFreqs));
    else
        drawFreqs (compactFreqs, (int) std::size (compactFreqs));

    // Gains.
    const int step = rangeDb <= 6.0f ? 3 : 6;

    for (int db = -(int) rangeDb; db <= (int) rangeDb; db += step)
    {
        if (std::abs (db) == (int) rangeDb && db != 0)
            continue;                                           // the plot's own edge

        const float y = std::round (yForDb ((float) db)) + 0.5f;
        g.setColour (db == 0 ? theme::lineStrong : theme::line);
        g.fillRect (plot.getX(), y - 0.5f, plot.getWidth(), 1.0f);

        const bool labelled = big || db == 0 || (db / step) % 2 == 0;

        if (labelled)
        {
            g.setColour (theme::text2);
            g.drawText (dbLabel (db), juce::Rectangle<float> (0.0f, y - 6.0f, plot.getX() - 5.0f, 12.0f), juce::Justification::centredRight, false);
        }
    }

    cacheScale = scale;
    cacheRange = rangeDb;
    cacheW = w;
    cacheH = h;
}

void EqGraph::paint (juce::Graphics& g)
{
    const float scale = juce::jmax (1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());

    if (gridCache.isNull() || cacheScale != scale || cacheW != getWidth() || cacheH != getHeight() || cacheRange != rangeDb)
        rebuildGridCache (scale);

    if (eq.getPreparedRate() != curveRate)
        refreshAllBands();

    g.drawImage (gridCache, getLocalBounds().toFloat());

    const float alpha = dimmed ? 0.45f : 1.0f;
    const int active = dragBand >= 0 ? dragBand : (hoverBand >= 0 ? hoverBand : selectedBand);

    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (plot.getSmallestIntegerContainer());

        if (spectrumHasData)
        {
            g.setColour (theme::text2.withAlpha (0.26f * alpha));
            g.fillPath (spectrumFill);
            g.setColour (theme::text2.withAlpha (0.55f * alpha));
            g.strokePath (spectrumTop, juce::PathStrokeType (1.0f));
        }

        // The area between the band under the pointer (or the selected one) and 0 dB, so it reads as a shape.
        if (settings[(size_t) active].on && ! bandPaths[(size_t) active].isEmpty() && bandDb[(size_t) active].size() == curveX.size())
        {
            const auto& db = bandDb[(size_t) active];
            bandFill.clear();
            bandFill.startNewSubPath (curveX.front(), yForDb (0.0f));

            for (size_t i = 0; i < curveX.size(); ++i)
                bandFill.lineTo (curveX[i], yForDbOffScale (db[i]));

            bandFill.lineTo (curveX.back(), yForDb (0.0f));
            bandFill.closeSubPath();
            g.setColour (colourFor (active).withAlpha (0.13f * alpha));
            g.fillPath (bandFill);
        }

        for (int b = 0; b < kBands; ++b)
        {
            if (! settings[(size_t) b].on || bandPaths[(size_t) b].isEmpty())
                continue;

            g.setColour (colourFor (b).withAlpha ((b == active ? 0.95f : 0.6f) * alpha));
            g.strokePath (bandPaths[(size_t) b], juce::PathStrokeType (m.bandStroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        if (! totalPath.isEmpty())
        {
            g.setColour (theme::text0.withAlpha (alpha));
            g.strokePath (totalPath, juce::PathStrokeType (m.totalStroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }

    // Nodes: bands that are off first (quiet rings), then the ones that are on, the selected one on top.
    for (int pass = 0; pass < 3; ++pass)
    {
        for (int b = 0; b < kBands; ++b)
        {
            const bool on = settings[(size_t) b].on;

            if ((pass == 0 && on) || (pass == 1 && (! on || b == selectedBand)) || (pass == 2 && (! on || b != selectedBand)))
                continue;

            const auto c = nodePoint (b);
            const auto colour = colourFor (b);
            const float r = m.nodeRadius;
            const bool emphasised = b == hoverBand || b == dragBand;

            if (! on)
            {
                g.setColour (colour.withAlpha ((emphasised ? 0.95f : 0.5f) * alpha));
                g.drawEllipse (juce::Rectangle<float> (r * 2.0f - 1.5f, r * 2.0f - 1.5f).withCentre (c), 1.5f);
                continue;
            }

            if (emphasised || b == selectedBand)
            {
                g.setColour (colour.withAlpha ((emphasised ? 0.30f : 0.20f) * alpha));
                g.fillEllipse (juce::Rectangle<float> ((r + 4.5f) * 2.0f, (r + 4.5f) * 2.0f).withCentre (c));
            }

            g.setColour (theme::bg1.withAlpha (alpha));
            g.fillEllipse (juce::Rectangle<float> ((r + 1.5f) * 2.0f, (r + 1.5f) * 2.0f).withCentre (c));
            g.setColour (colour.withAlpha (alpha));
            g.fillEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (c));
        }
    }

    // Tooltip-like label for the band under the pointer.
    const int shown = dragBand >= 0 ? dragBand : hoverBand;

    if (shown >= 0)
    {
        const auto text = describeBand (shown, settings[(size_t) shown]);
        const auto font = theme::mono (mode == Mode::expanded ? 11.0f : 10.0f);
        const float w = theme::textWidth (font, text) + 26.0f;
        const auto anchor = nodePoint (shown);
        const float above = anchor.y - m.nodeRadius - 20.0f;
        auto box = juce::Rectangle<float> (w, 22.0f).withCentre ({ anchor.x, above >= plot.getY() ? above : anchor.y + m.nodeRadius + 20.0f });
        box = box.constrainedWithin (getLocalBounds().toFloat().reduced (2.0f));

        g.setColour (theme::bg0.withAlpha (0.94f));
        g.fillRoundedRectangle (box, 4.0f);
        g.setColour (theme::lineStrong);
        g.drawRoundedRectangle (box.reduced (0.5f), 3.5f, 1.0f);

        g.setColour (colourFor (shown));
        g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ box.getX() + 11.0f, box.getCentreY() }));

        g.setColour (theme::text0);
        g.setFont (font);
        g.drawText (text, box.withTrimmedLeft (20.0f).withTrimmedRight (6.0f), juce::Justification::centredLeft, false);
    }

    if (soloing && dragBand >= 0)
    {
        g.setFont (theme::sans (11.0f, theme::Sans::Medium));
        g.setColour (theme::text1);
        g.drawText ("Solo", juce::Rectangle<float> (plot.getX() + 10.0f, plot.getY() + 6.0f, 60.0f, 16.0f), juce::Justification::centredLeft, false);
        g.setColour (colourFor (dragBand));
        g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ plot.getX() + 6.0f, plot.getY() + 14.0f }));
    }
}

// ---------------------------------------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------------------------------------

void EqGraph::mouseMove (const juce::MouseEvent& e)
{
    for (int b = 0; b < kBands; ++b)
        settings[(size_t) b] = eq.getBandSettings (b);

    const int hit = bandAt (e.position);

    if (hit != hoverBand)
    {
        hoverBand = hit;
        setMouseCursor (hit >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void EqGraph::mouseExit (const juce::MouseEvent&)
{
    if (hoverBand >= 0 && dragBand < 0)
    {
        hoverBand = -1;
        repaint();
    }
}

void EqGraph::beginEdit (int band)
{
    auto& p = params[(size_t) band];

    for (auto* param : { p.on, p.freq, p.gain })
        if (param != nullptr)
            param->beginChangeGesture();
}

void EqGraph::endEdit()
{
    if (dragBand < 0)
        return;

    auto& p = params[(size_t) dragBand];

    for (auto* param : { p.on, p.freq, p.gain })
        if (param != nullptr)
            param->endChangeGesture();
}

void EqGraph::mouseDown (const juce::MouseEvent& e)
{
    for (int b = 0; b < kBands; ++b)
        settings[(size_t) b] = eq.getBandSettings (b);

    const int hit = bandAt (e.position);

    if (e.mods.isPopupMenu())
    {
        if (hit >= 0)
        {
            select (hit);
            showBandMenu (hit);
        }

        return;
    }

    dragBand = hit;

    if (hit < 0)
        return;

    select (hit);
    beginEdit (hit);
    dragStartedOn = settings[(size_t) hit].on;

    // Grabbing a band that is off switches it on (it keeps the type and frequency it was waiting with).
    if (! dragStartedOn)
    {
        setPlain (params[(size_t) hit].on, 1.0f);
        refreshBand (hit);
    }

    virtualPos = nodePoint (hit);
    lastPointer = e.position;

    if (e.mods.isAltDown())
    {
        soloing = true;
        eq.setSoloBand (hit);
    }

    repaint();
}

void EqGraph::mouseDrag (const juce::MouseEvent& e)
{
    if (dragBand < 0)
        return;

    // Shift: the pointer moves the node a tenth as far (relative, so pressing or releasing Shift mid-drag does not jump).
    const float scale = e.mods.isShiftDown() ? 0.1f : 1.0f;
    virtualPos += (e.position - lastPointer) * scale;
    lastPointer = e.position;
    virtualPos = { juce::jlimit (plot.getX(), plot.getRight(), virtualPos.x), juce::jlimit (plot.getY(), plot.getBottom(), virtualPos.y) };

    auto& p = params[(size_t) dragBand];
    const auto range = p.freq->getNormalisableRange();
    setPlain (p.freq, juce::jlimit (range.start, range.end, freqForX (virtualPos.x)));

    if (Equalizer::hasGain (settings[(size_t) dragBand].type))
    {
        float db = std::round (dbForY (virtualPos.y) * 10.0f) / 10.0f;

        if (std::abs (db) < 0.25f && ! e.mods.isShiftDown())
            db = 0.0f;

        setPlain (p.gain, db);
    }

    refreshBand (dragBand);
    repaintPlot();
}

void EqGraph::mouseUp (const juce::MouseEvent&)
{
    if (dragBand < 0)
        return;

    endEdit();

    if (soloing)
    {
        soloing = false;
        eq.setSoloBand (-1);
    }

    dragBand = -1;
    repaint();
}

void EqGraph::mouseDoubleClick (const juce::MouseEvent& e)
{
    for (int b = 0; b < kBands; ++b)
        settings[(size_t) b] = eq.getBandSettings (b);

    const int hit = bandAt (e.position);

    if (hit >= 0)
    {
        auto& p = params[(size_t) hit];

        if (Equalizer::hasGain (settings[(size_t) hit].type))
            setComplete (p.gain, 0.0f);                                    // back to flat
        else
            setComplete (p.q, p.q->convertFrom0to1 (p.q->getDefaultValue()));

        refreshBand (hit);
        repaintPlot();
        return;
    }

    if (plot.contains (e.position))
        addBandAt (e.position);
}

// Switches on the free band whose waiting place is nearest, and puts it (as a peak) where the person clicked.
void EqGraph::addBandAt (juce::Point<float> position)
{
    const float clickHz = freqForX (position.x);
    int best = -1;
    float bestDistance = 1.0e9f;

    for (int b = 0; b < kBands; ++b)
    {
        if (settings[(size_t) b].on)
            continue;

        const float d = std::abs (std::log (settings[(size_t) b].freq / clickHz));

        if (d < bestDistance)
        {
            bestDistance = d;
            best = b;
        }
    }

    if (best < 0)
        return;                                                 // all eight are in use

    auto& p = params[(size_t) best];
    float db = std::round (dbForY (position.y) * 10.0f) / 10.0f;

    if (std::abs (db) < 0.25f)
        db = 0.0f;

    setComplete (p.type, (float) (int) Equalizer::Type::peak);
    setComplete (p.freq, clickHz);
    setComplete (p.gain, db);
    setComplete (p.q, 1.0f);
    setComplete (p.on, 1.0f);

    refreshBand (best);
    select (best);
    repaint();
}

void EqGraph::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    for (int b = 0; b < kBands; ++b)
        settings[(size_t) b] = eq.getBandSettings (b);

    const int band = bandAt (e.position);

    if (band < 0 || ! settings[(size_t) band].on || wheel.deltaY == 0.0f)
    {
        juce::Component::mouseWheelMove (e, wheel);             // not on a band: let the drawer scroll
        return;
    }

    const float direction = (wheel.deltaY > 0.0f ? 1.0f : -1.0f) * (wheel.isReversed ? -1.0f : 1.0f);
    const float step = e.mods.isShiftDown() ? kWheelStepFine : kWheelStep;
    auto* q = params[(size_t) band].q;
    const auto range = q->getNormalisableRange();

    setComplete (q, juce::jlimit (range.start, range.end, plainOf (q) * (direction > 0.0f ? step : 1.0f / step)));
    refreshBand (band);
    repaintPlot();
}

void EqGraph::showBandMenu (int band)
{
    const auto s = settings[(size_t) band];
    juce::PopupMenu menu;

    for (int t = 0; t < Equalizer::kNumTypes; ++t)
        menu.addItem (100 + t, Equalizer::typeName ((Equalizer::Type) t), true, s.type == (Equalizer::Type) t);

    if (Equalizer::isCut (s.type))
    {
        menu.addSeparator();

        for (int slope = 0; slope < 4; ++slope)
            menu.addItem (200 + slope, Equalizer::slopeName (slope), true, s.slope == slope);

        if (s.slope == dsp::eq::kSlopeLinkwitzRiley24)
            menu.addItem (200 + s.slope, Equalizer::slopeName (s.slope), true, true);
    }

    menu.addSeparator();
    menu.addItem (1, s.on ? "Turn off" : "Turn on");

    juce::Component::SafePointer<EqGraph> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options().withStandardItemHeight (28).withMinimumWidth (160),
                        [safe, band] (int result)
                        {
                            if (safe != nullptr && result > 0)
                                safe->applyMenuResult (band, result);
                        });
}

// The band menu's items: 1 = turn on / off, 100 + n = type n, 200 + n = cut slope n.
void EqGraph::applyMenuResult (int band, int result)
{
    settings[(size_t) band] = eq.getBandSettings (band);
    auto& p = params[(size_t) band];
    const bool wasOn = settings[(size_t) band].on;

    if (result == 1)
    {
        setComplete (p.on, wasOn ? 0.0f : 1.0f);
    }
    else if (result >= 200)
    {
        setComplete (p.slope, (float) (result - 200));
    }
    else if (result >= 100)
    {
        setComplete (p.type, (float) (result - 100));

        if (! wasOn)
            setComplete (p.on, 1.0f);           // choosing a shape for a waiting band means using it
    }

    refreshBand (band);
    repaint();
}
}
