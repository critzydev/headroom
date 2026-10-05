// NoisePanel implementation: layout, live readouts, the spectrum's maths and drawing, the hold-to-listen button.
#include "ui/fx/NoisePanel.h"

#include "ui/Format.h"
#include "ui/HeadroomLookAndFeel.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

#include <algorithm>
#include <cmath>

namespace hr
{
namespace
{
    constexpr float kOctaveHalfStep = 0.0594630944f;    // 2^(1/12) - 1: 1/6 octave in total, centred
    constexpr float kFloorDb = -120.0f;

    float smoothingFactor (double deltaSeconds, double seconds) noexcept
    {
        return (float) (1.0 - std::exp (-deltaSeconds / seconds));
    }

    juce::String dbText (float value)
    {
        const int rounded = juce::roundToInt (std::abs (value));
        return (value < -0.5f ? theme::glyph::minus : juce::String()) + juce::String (rounded) + " dB";
    }
}

// ---------------------------------------------------------------------------------------------------
// SpectrumAnalyzer
// ---------------------------------------------------------------------------------------------------

SpectrumAnalyzer::SpectrumAnalyzer()
{
    for (int i = 0; i < kSize; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / (float) (kSize - 1));

    setLayout (336, 48000.0);
}

float SpectrumAnalyzer::frequencyForColumn (int column) const noexcept
{
    const float t = ((float) column + 0.5f) / (float) juce::jmax (1, (int) inputDb.size());
    return kMinHz * std::pow (kMaxHz / kMinHz, t);
}

void SpectrumAnalyzer::setLayout (int numColumns, double newSampleRate)
{
    numColumns = juce::jmax (8, numColumns);
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;

    inputDb.assign ((size_t) numColumns, kFloorDb);
    outputDb.assign ((size_t) numColumns, kFloorDb);
    inputTarget.assign ((size_t) numColumns, kFloorDb);
    outputTarget.assign ((size_t) numColumns, kFloorDb);
    firstBin.assign ((size_t) numColumns, 1);
    lastBin.assign ((size_t) numColumns, 1);
    binPosition.assign ((size_t) numColumns, 1.0f);

    const float binHz = (float) sampleRate / (float) kSize;
    const int maxBin = juce::jmin (kSize / 2, (int) (sampleRate * 0.5 / binHz));

    for (int c = 0; c < numColumns; ++c)
    {
        const float f = frequencyForColumn (c);
        firstBin[(size_t) c] = juce::jlimit (1, maxBin, juce::roundToInt (f * (1.0f - kOctaveHalfStep) / binHz));
        lastBin[(size_t) c] = juce::jlimit (firstBin[(size_t) c], maxBin, juce::roundToInt (f * (1.0f + kOctaveHalfStep) / binHz));
        binPosition[(size_t) c] = juce::jlimit (1.0f, (float) (maxBin - 1), f / binHz);
    }

    reset();
}

void SpectrumAnalyzer::reset() noexcept
{
    havePrevious = false;
    ceilingDb = -20.0f;
    std::fill (inputDb.begin(), inputDb.end(), kFloorDb);
    std::fill (outputDb.begin(), outputDb.end(), kFloorDb);
}

// Windowed FFT, power averaged over each column's band (prefix sums), then dB. 0 dB is a full-scale sine.
void SpectrumAnalyzer::analyse (const float* samples, std::vector<float>& targetDb) noexcept
{
    for (int i = 0; i < kSize; ++i)
        work[(size_t) i] = samples[i] * window[(size_t) i];

    std::fill (work.begin() + kSize, work.end(), 0.0f);
    fft.performFrequencyOnlyForwardTransform (work.data(), true);

    prefix[0] = 0.0;

    for (int k = 0; k <= kSize / 2; ++k)
        prefix[(size_t) k + 1] = prefix[(size_t) k] + (double) work[(size_t) k] * (double) work[(size_t) k];

    constexpr double kFullScale = (double) kSize * 0.25;            // a full-scale sine under a Hann window

    for (size_t c = 0; c < targetDb.size(); ++c)
    {
        const int a = firstBin[c], b = lastBin[c];
        double mean;

        if (b - a < 2)
        {
            // Fewer bins than pixels here (the lowest octaves): interpolate between the two nearest instead of stepping.
            const int k = (int) binPosition[c];
            const double t = (double) binPosition[c] - (double) k;
            const double p0 = prefix[(size_t) k + 1] - prefix[(size_t) k], p1 = prefix[(size_t) k + 2] - prefix[(size_t) k + 1];
            mean = p0 + (p1 - p0) * t;
        }
        else
        {
            mean = (prefix[(size_t) b + 1] - prefix[(size_t) a]) / (double) (b - a + 1);
        }

        targetDb[c] = juce::jmax (kFloorDb, 10.0f * (float) std::log10 (mean / (kFullScale * kFullScale) + 1.0e-12));
    }
}

bool SpectrumAnalyzer::update (const denoise::SpectrumTap& tap, double deltaSeconds)
{
    if (! tap.read (inputSamples.data(), outputSamples.data(), kSize))
        return false;

    analyse (inputSamples.data(), inputTarget);
    analyse (outputSamples.data(), outputTarget);

    if (! havePrevious)
    {
        inputDb = inputTarget;
        outputDb = outputTarget;
    }
    else
    {
        // Quick to rise, slow to fall: the picture stays calm and still shows every word.
        const float up = smoothingFactor (deltaSeconds, 0.04), down = smoothingFactor (deltaSeconds, 0.25);

        for (size_t c = 0; c < inputDb.size(); ++c)
        {
            inputDb[c] += (inputTarget[c] - inputDb[c]) * (inputTarget[c] > inputDb[c] ? up : down);
            outputDb[c] += (outputTarget[c] - outputDb[c]) * (outputTarget[c] > outputDb[c] ? up : down);
        }
    }

    // The plot's top follows the loudest part of the input in 6 dB steps: instantly up, slowly down.
    const float peak = *std::max_element (inputTarget.begin(), inputTarget.end());
    const float wanted = juce::jmax (-30.0f, std::ceil ((peak + 4.0f) / 6.0f) * 6.0f);

    if (wanted > ceilingDb || ! havePrevious)
        ceilingDb = wanted;
    else
        ceilingDb += (wanted - ceilingDb) * smoothingFactor (deltaSeconds, 4.0);

    havePrevious = true;
    return true;
}

// ---------------------------------------------------------------------------------------------------
// NoiseSpectrumView
// ---------------------------------------------------------------------------------------------------

NoiseSpectrumView::NoiseSpectrumView()
{
    setOpaque (false);
    setInterceptsMouseClicks (false, false);
    setTitle ("Noise removal spectrum");
}

void NoiseSpectrumView::resized()
{
    plot = getLocalBounds().toFloat().withTrimmedBottom (16.0f);
    rebuildPaths();
}

float NoiseSpectrumView::yForDb (float db) const noexcept
{
    const float t = juce::jlimit (0.0f, 1.0f, (db - (ceiling - SpectrumAnalyzer::kRangeDb)) / SpectrumAnalyzer::kRangeDb);
    return plot.getBottom() - 3.0f - t * (plot.getHeight() - 8.0f);
}

void NoiseSpectrumView::setData (const SpectrumAnalyzer& analyzer)
{
    input = analyzer.getInputDb();
    output = analyzer.getOutputDb();
    ceiling = analyzer.getCeilingDb();
    rebuildPaths();
    repaint();
}

void NoiseSpectrumView::rebuildPaths()
{
    inputLine.clear();
    outputLine.clear();
    outputFill.clear();
    removed.clear();

    const int columns = getNumColumns();

    if (plot.isEmpty() || (int) input.size() != columns || (int) output.size() != columns)
        return;

    const float x0 = plot.getX() + 0.5f * (plot.getWidth() - (float) columns) + 0.5f;

    for (int c = 0; c < columns; ++c)
    {
        const float x = x0 + (float) c;
        const float yi = yForDb (input[(size_t) c]), yo = yForDb (output[(size_t) c]);

        if (c == 0)
        {
            inputLine.startNewSubPath (x, yi);
            outputLine.startNewSubPath (x, yo);
            removed.startNewSubPath (x, yi);
        }
        else
        {
            inputLine.lineTo (x, yi);
            outputLine.lineTo (x, yo);
            removed.lineTo (x, yi);
        }
    }

    // The amber part: from the input curve down to the output curve, where the output is below the input.
    for (int c = columns - 1; c >= 0; --c)
    {
        const float yi = yForDb (input[(size_t) c]), yo = yForDb (output[(size_t) c]);
        removed.lineTo (x0 + (float) c, juce::jmax (yi, yo));
    }

    removed.closeSubPath();

    outputFill = outputLine;
    outputFill.lineTo (x0 + (float) (columns - 1), plot.getBottom());
    outputFill.lineTo (x0, plot.getBottom());
    outputFill.closeSubPath();
}

void NoiseSpectrumView::paint (juce::Graphics& g)
{
    const float alpha = dimmed ? 0.5f : 1.0f;

    g.setColour (theme::bg2);
    g.fillRoundedRectangle (plot, theme::kControlRadius);

    // Frequency grid: three lines, three labels.
    g.setFont (theme::mono (9.0f));
    const float x0 = plot.getX() + 0.5f * (plot.getWidth() - (float) getNumColumns());
    const float span = (float) getNumColumns();

    struct Mark { float hz; const char* text; };

    for (const auto& mark : { Mark { 100.0f, "100 Hz" }, Mark { 1000.0f, "1 kHz" }, Mark { 10000.0f, "10 kHz" } })
    {
        const float x = std::round (x0 + std::log (mark.hz / SpectrumAnalyzer::kMinHz) / std::log (SpectrumAnalyzer::kMaxHz / SpectrumAnalyzer::kMinHz) * span);
        g.setColour (theme::line);
        g.fillRect (x, plot.getY() + 1.0f, 1.0f, plot.getHeight() - 2.0f);
        g.setColour (theme::text2);
        g.drawText (mark.text, juce::Rectangle<float> (x - 30.0f, plot.getBottom() + 2.0f, 60.0f, 12.0f), juce::Justification::centred, false);
    }

    {
        const juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (plot.reduced (1.0f).getSmallestIntegerContainer());

        if (! output.empty())
        {
            g.setColour (theme::text1.withAlpha (0.14f * alpha));
            g.fillPath (outputFill);
            g.setColour (theme::text1.withAlpha (0.45f * alpha));
            g.strokePath (outputLine, juce::PathStrokeType (1.0f));

            g.setColour (theme::accent.withAlpha (0.5f * alpha));
            g.fillPath (removed);

            g.setColour (theme::text1.withAlpha (0.85f * alpha));
            g.strokePath (inputLine, juce::PathStrokeType (1.0f));
        }
    }

    g.setColour (theme::line);
    g.drawRoundedRectangle (plot.reduced (0.5f), theme::kControlRadius - 0.5f, 1.0f);

    // The one legend that matters: what the amber is.
    g.setFont (theme::sans (10.0f, theme::Sans::Medium));
    const auto label = juce::Rectangle<float> (plot.getRight() - 64.0f, plot.getY() + 5.0f, 56.0f, 12.0f);
    g.setColour (theme::accent.withAlpha (0.42f * alpha));
    g.fillRoundedRectangle (juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ label.getX() + 3.0f, label.getCentreY() }), 1.5f);
    g.setColour (theme::accent.withAlpha (0.85f * alpha));
    g.drawText ("Removed", label.withTrimmedLeft (10.0f), juce::Justification::centredLeft, false);

    if (! live)
    {
        g.setColour (theme::text2.withAlpha (alpha));
        g.setFont (theme::sans (11.0f));
        g.drawText ("Say something to see it here", plot.reduced (10.0f), juce::Justification::centred, false);
    }
}

// ---------------------------------------------------------------------------------------------------
// HoldButton
// ---------------------------------------------------------------------------------------------------

HoldButton::HoldButton (const juce::String& labelText) : label (labelText)
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setTitle (labelText);
}

void HoldButton::setHeld (bool shouldHold, bool notify)
{
    if (held == shouldHold)
        return;

    held = shouldHold;
    repaint();

    if (notify && onHold)
        onHold (held);
}

void HoldButton::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();

    g.setColour (held ? theme::tint (theme::bg2, theme::accent, 0.16f) : (hover ? theme::bg3 : theme::bg2));
    g.fillRoundedRectangle (bounds, theme::kControlRadius);
    g.setColour (held ? theme::accent : theme::line);
    g.drawRoundedRectangle (bounds.reduced (0.5f), theme::kControlRadius - 0.5f, 1.0f);

    const auto font = theme::sans (13.0f, theme::Sans::Medium);
    const float textWidth = std::ceil (theme::textWidth (font, label));
    const float total = 16.0f + 8.0f + textWidth;
    const float left = bounds.getCentreX() - total * 0.5f;

    Icons::draw (g, "headphones", juce::Point<float> (left + 8.0f, bounds.getCentreY()), 16.0f, held ? theme::accent : theme::text1);
    g.setColour (theme::text0);
    g.setFont (font);
    g.drawText (label, juce::Rectangle<float> (left + 24.0f, bounds.getY(), textWidth + 2.0f, bounds.getHeight()), juce::Justification::centredLeft, false);
}

// ---------------------------------------------------------------------------------------------------
// NoisePanel
// ---------------------------------------------------------------------------------------------------

namespace
{
    constexpr int kPad = 4;
    const char* const kNoisyTip = "Move the mic closer or turn the back of the mic toward your PC.";
    constexpr int kRowHeight = 18;

    void setUpSlider (juce::Slider& s, juce::Component& parent)
    {
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        s.setMouseClickGrabsKeyboardFocus (false);
        s.setScrollWheelEnabled (false);            // the wheel scrolls the drawer, it must not move a slider under the pointer
        parent.addAndMakeVisible (s);
    }
}

NoisePanel::NoisePanel (NoiseRemoval& processor)
    : noise (processor), hearButton ("Hear what" + theme::glyph::quoteClose + "s removed")
{
    setTitle ("Noise removal");

    modeControl.setLabels ({ "Fast", "Best", "Light" });
    modeControl.onChange = [this] (int index)
    {
        if (auto* param = noise.apvts.getParameter ("mode"))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost (param->convertTo0to1 ((float) index));
            param->endChangeGesture();
        }
    };
    addAndMakeVisible (modeControl);
    addAndMakeVisible (spectrum);

    auto attach = [this] (juce::Slider& slider, const char* id, const char* tip, std::unique_ptr<juce::SliderParameterAttachment>& holder)
    {
        setUpSlider (slider, *this);
        slider.setTooltip (tip);

        if (auto* param = noise.apvts.getParameter (id))
        {
            holder = std::make_unique<juce::SliderParameterAttachment> (*param, slider, nullptr);
            slider.setDoubleClickReturnValue (true, (double) param->convertFrom0to1 (param->getDefaultValue()));
        }

        slider.onValueChange = [this] { repaint(); };
    };

    attach (strength, "strength", "How much of the noise is taken out. Double-click for the usual setting.", strengthAttachment);
    attach (postFilter, "postFilter", "Removes what little hiss is left. Too much makes the voice thin.", postFilterAttachment);
    attach (threshold, "threshold", "How sure Light must be that it hears you before it lets sound through.", thresholdAttachment);
    attach (hold, "hold", "How long Light keeps letting sound through after you stop talking.", holdAttachment);

    hearButton.setTooltip ("Hold to listen to what is being taken out. Nothing goes to Discord while you hold it.");
    hearButton.onHold = [this] (bool on) { noise.requestHearRemoved (on); };
    addAndMakeVisible (hearButton);

    watcher = std::make_unique<ParamWatcher> (std::vector<juce::RangedAudioParameter*> { noise.apvts.getParameter ("mode") },
                                              [this] { refresh(); });

    syncMode();
    layoutAll();
    setSize (kWidth, preferredHeight);
}

NoisePanel::~NoisePanel()
{
    hearButton.setHeld (false, true);           // the drawer closed while it was held
    noise.getSpectrumTap().active.store (false);
    watcher.reset();
}

void NoisePanel::setDimmed (bool shouldDim)
{
    dimmed = shouldDim;
    spectrum.setDimmed (shouldDim);
    setAlpha (shouldDim ? 0.55f : 1.0f);
}

void NoisePanel::syncMode()
{
    modeControl.setSelected ((int) noise.getRequestedMode(), false);
}

void NoisePanel::refresh()
{
    syncMode();
    layoutAll();
    repaint();
}

bool NoisePanel::isOnScreen() const
{
    // Visible all the way up, and (in the real window) not minimised. A top-level component that is not on the desktop
    // at all is taken as showing: that is how the screenshots are made.
    if (! isVisible())
        return false;

    for (const juce::Component* c = getParentComponent(); c != nullptr; c = c->getParentComponent())
    {
        if (c->getParentComponent() == nullptr && c->getPeer() == nullptr)
            break;

        if (! c->isVisible())
            return false;
    }

    if (auto* peer = getTopLevelComponent()->getPeer())
        return ! peer->isMinimised();

    return true;
}

void NoisePanel::setAdvancedOpen (bool open)
{
    if (advancedOpen == open)
        return;

    advancedOpen = open;
    layoutAll();
    repaint();
}

void NoisePanel::mouseUp (const juce::MouseEvent& e)
{
    if (! e.mouseWasDraggedSinceMouseDown() && advancedArea.contains (e.getPosition()))
        setAdvancedOpen (! advancedOpen);
}

void NoisePanel::resized()
{
    layoutAll();
}

// Everything is stacked from the top; the panel is exactly as tall as what is shown (the drawer asks for it).
void NoisePanel::layoutAll()
{
    const int w = getWidth() > 0 ? getWidth() : kWidth;
    int y = kPad;

    modeControl.setBounds (0, y, 192, 28);
    speakingArea = juce::Rectangle<int> (w - 120, y, 120, 28);
    y += 28;
    captionArea = juce::Rectangle<int> (0, y + 3, w, 14);
    y += 17 + 8;

    spectrum.setBounds (0, y, w, NoiseSpectrumView::kHeight);
    y += NoiseSpectrumView::kHeight + 12;

    strengthLabelArea = juce::Rectangle<int> (0, y, w, 16);
    y += 18;
    strength.setBounds (0, y, w, 24);
    y += 26;
    reductionArea = juce::Rectangle<int> (0, y, w, kRowHeight);
    y += kRowHeight + 8;
    dividerArea = juce::Rectangle<int> (0, y, w, 1);
    y += 9;
    roomArea = juce::Rectangle<int> (0, y, w, kRowHeight);
    y += kRowHeight;

    if (verdict == Verdict::noisy)
    {
        // One line if it fits, else two.
        const int lines = theme::textWidth (theme::sans (12.0f), kNoisyTip) <= (float) w ? 1 : 2;
        tipArea = juce::Rectangle<int> (0, y + 2, w, 16 * lines);
        y += 6 + 16 * lines;
    }
    else
    {
        tipArea = {};
    }

    y += 12;
    hearButton.setBounds (0, y, w, 32);
    y += 32 + 6;
    hintArea = juce::Rectangle<int> (0, y, w, 14);
    y += 14 + 8;

    advancedArea = juce::Rectangle<int> (0, y, w, 28);
    y += 28;

    const bool lightRunning = noise.getRequestedMode() == NoiseRemoval::Mode::light;

    for (auto* s : { &postFilter, &threshold, &hold })
        s->setVisible (advancedOpen);

    if (advancedOpen)
    {
        y += 4;
        postFilterLabelArea = juce::Rectangle<int> (0, y, w, 16);
        y += 18;
        postFilter.setBounds (0, y, w, 24);
        y += 30;
        thresholdLabelArea = juce::Rectangle<int> (0, y, w, 16);
        y += 18;
        threshold.setBounds (0, y, w, 24);
        y += 30;
        holdLabelArea = juce::Rectangle<int> (0, y, w, 16);
        y += 18;
        hold.setBounds (0, y, w, 24);
        y += 30;
        readoutArea = juce::Rectangle<int> (0, y, w, 16);
        y += 20;

        postFilter.setEnabled (! lightRunning);
        threshold.setEnabled (lightRunning);
        hold.setEnabled (lightRunning);
    }
    else
    {
        postFilterLabelArea = thresholdLabelArea = holdLabelArea = readoutArea = {};
    }

    y += 8;

    if (y != preferredHeight)
    {
        preferredHeight = y;

        if (onHeightChanged)
            onHeightChanged();
    }
}

juce::String NoisePanel::captionText (juce::Colour& colour) const
{
    const auto mode = noise.getRequestedMode();
    colour = theme::text2;

    switch (noise.getStatus())
    {
        case NoiseRemoval::Status::loading:
            colour = theme::text1;
            return juce::String (NoiseRemoval::getModeName (mode)) + " is getting ready. Light runs until then.";

        case NoiseRemoval::Status::unavailable:
            colour = theme::warn;
            return noise.getStatusNote();

        case NoiseRemoval::Status::ready:
            break;
    }

    const int ms = juce::roundToInt (1000.0 * NoiseRemoval::getNominalLatencySamples (mode) / 48000.0);
    const char* what = mode == NoiseRemoval::Mode::fast ? "lowest delay" : (mode == NoiseRemoval::Mode::best ? "cleanest, recommended" : "lowest CPU");
    return juce::String (ms) + " ms " + theme::glyph::dot + " " + what;
}

void NoisePanel::tickMeter (double dt)
{
    auto& tap = noise.getSpectrumTap();
    const bool showing = isOnScreen();
    tap.active.store (showing, std::memory_order_relaxed);        // nothing is written for a panel that is not on screen

    if (! showing)
        return;

    // The spectrum.
    const double rate = tap.sampleRate.load();

    if (rate != lastSampleRate || analyzer.getNumColumns() != spectrum.getNumColumns())
    {
        lastSampleRate = rate;
        analyzer.setLayout (spectrum.getNumColumns(), rate);
    }

    if (analyzer.update (tap, dt))
    {
        spectrum.setData (analyzer);
        spectrum.setActive (analyzer.getCeilingDb() > -29.0f || noise.getVoiceLevelDb() > -100.0f);
    }

    updateReadouts (dt);
}

void NoisePanel::updateReadouts (double dt)
{
    // Speaking.
    const bool nowSpeaking = noise.isVoiceActive();

    if (nowSpeaking != speaking)
    {
        speaking = nowSpeaking;
        repaint (speakingArea);
    }

    // Caption (mode, or what is going on).
    juce::Colour colour;
    const auto caption = captionText (colour);

    if (caption != captionCache || colour != captionColour)
    {
        captionCache = caption;
        captionColour = colour;
        repaint (captionArea);
    }

    // Noise removed.
    shownReduction += (noise.getReductionDb() - shownReduction) * smoothingFactor (dt, 0.25);
    const auto reduction = dbText (-shownReduction);

    if (reduction != reductionText)
    {
        reductionText = reduction;
        repaint (reductionArea);
    }

    // Your voice against the room.
    const float voice = noise.getVoiceLevelDb(), floorLevel = noise.getNoiseFloorDb();
    const bool known = voice > -110.0f && floorLevel > -110.0f;

    if (known)
    {
        const float snr = voice - floorLevel;
        shownSnr = haveSnr ? shownSnr + (snr - shownSnr) * smoothingFactor (dt, 0.5) : snr;
        haveSnr = true;
    }

    // Great from 30 dB, Good from 20 dB; 1.5 dB of hysteresis so the words do not flicker at the borders.
    auto next = verdict;

    if (haveSnr)
    {
        const float s = shownSnr;
        const float greatAt = verdict == Verdict::great ? 28.5f : 30.0f;
        const float goodAt = (verdict == Verdict::good || verdict == Verdict::great) ? 18.5f : 20.0f;
        next = s >= greatAt ? Verdict::great : (s >= goodAt ? Verdict::good : Verdict::noisy);
    }

    const auto text = haveSnr ? juce::String (juce::roundToInt (shownSnr)) + " dB" : juce::String();

    if (text != roomText || next != verdict)
    {
        const bool relayout = (next == Verdict::noisy) != (verdict == Verdict::noisy);
        roomText = text;
        verdict = next;

        if (relayout)
            layoutAll();

        repaint();
    }

    // Advanced readout.
    if (advancedOpen)
    {
        const double sr = juce::jmax (1.0, noise.getSpectrumTap().sampleRate.load());
        auto readout = "Delay " + juce::String (juce::roundToInt (1000.0 * noise.getLatencySamples() / sr)) + " ms   CPU "
                       + juce::String (juce::roundToInt (100.0f * noise.getCpuLoad())) + " %";

        if (noise.getMissedFrames() > 0)
            readout += "   Late " + juce::String ((int) noise.getMissedFrames());

        if (readout != readoutText)
        {
            readoutText = readout;
            repaint (readoutArea);
        }
    }

    // "Hear what's removed" ended by itself (a timeout in the app): let go of the button.
    if (hearButton.isHeld() && ! noise.isHearingRemoved())
        hearButton.setHeld (false, false);
}

void NoisePanel::paint (juce::Graphics& g)
{
    const auto label = theme::sans (12.0f);

    // Speaking.
    {
        const float dot = 7.0f;
        const auto word = juce::String ("Speaking");
        g.setFont (theme::sans (12.0f, theme::Sans::Medium));
        const float textWidth = std::ceil (theme::textWidth (theme::sans (12.0f, theme::Sans::Medium), word));
        const auto area = speakingArea.toFloat();
        const float right = area.getRight();

        g.setColour (speaking ? theme::ok : theme::text2);
        g.drawText (word, juce::Rectangle<float> (right - textWidth - 1.0f, area.getY(), textWidth + 2.0f, area.getHeight()), juce::Justification::centredRight, false);

        const auto centre = juce::Point<float> (right - textWidth - 12.0f, area.getCentreY());

        if (speaking)
        {
            g.fillEllipse (juce::Rectangle<float> (dot, dot).withCentre (centre));
        }
        else
        {
            g.setColour (theme::text2);
            g.drawEllipse (juce::Rectangle<float> (dot - 1.0f, dot - 1.0f).withCentre (centre), 1.0f);
        }
    }

    // Under the mode control: what it means, or what is going on.
    g.setColour (captionColour);
    g.setFont (theme::sans (11.0f));
    g.drawText (captionCache, captionArea, juce::Justification::centredLeft, true);

    // Strength.
    g.setColour (theme::text1);
    g.setFont (label);
    g.drawText ("How much noise to remove", strengthLabelArea, juce::Justification::centredLeft, false);
    g.setFont (theme::mono (12.0f));
    g.drawText (juce::String (juce::roundToInt (NoiseRemoval::attenuationDbForStrength (noise.apvts.getRawParameterValue ("strength")->load()))) + " dB",
                strengthLabelArea, juce::Justification::centredRight, false);

    // Noise removed.
    g.setColour (theme::text1);
    g.setFont (label);
    g.drawText ("Noise removed", reductionArea, juce::Justification::centredLeft, false);
    g.setColour (shownReduction >= 0.5f ? theme::accent : theme::text2);
    g.setFont (theme::mono (12.0f));
    g.drawText (reductionText, reductionArea, juce::Justification::centredRight, false);

    g.setColour (theme::line);
    g.fillRect (dividerArea);

    // Your voice against the room.
    g.setColour (theme::text1);
    g.setFont (label);
    g.drawText ("Your voice vs room noise", roomArea, juce::Justification::centredLeft, false);

    if (verdict == Verdict::unknown)
    {
        g.setColour (theme::text2);
        g.drawText ("Say a few words to measure", roomArea, juce::Justification::centredRight, false);
    }
    else
    {
        const char* word = verdict == Verdict::great ? "Great" : (verdict == Verdict::good ? "Good" : "Noisy");
        const auto colour = verdict == Verdict::great ? theme::ok : (verdict == Verdict::good ? theme::text0 : theme::warn);
        const auto wordFont = theme::sans (12.0f, theme::Sans::SemiBold);
        const float wordWidth = std::ceil (theme::textWidth (wordFont, word));

        g.setColour (colour);
        g.setFont (wordFont);
        g.drawText (word, juce::Rectangle<int> (roomArea.getRight() - (int) wordWidth - 2, roomArea.getY(), (int) wordWidth + 2, roomArea.getHeight()),
                    juce::Justification::centredRight, false);

        g.setColour (theme::text0);
        g.setFont (theme::mono (12.0f));
        g.drawText (roomText, juce::Rectangle<int> (roomArea.getX(), roomArea.getY(), roomArea.getWidth() - (int) wordWidth - 12, roomArea.getHeight()),
                    juce::Justification::centredRight, false);
    }

    if (! tipArea.isEmpty())
    {
        g.setColour (theme::text1);
        g.setFont (label);
        g.drawFittedText (kNoisyTip, tipArea, juce::Justification::topLeft, 2, 1.0f);
    }

    g.setColour (theme::text2);
    g.setFont (theme::sans (11.0f));
    g.drawText ("Hold to listen. Discord can" + theme::glyph::quoteClose + "t hear you meanwhile.", hintArea, juce::Justification::centredLeft, true);

    // Advanced.
    g.setColour (theme::line);
    g.fillRect (0, advancedArea.getY(), getWidth(), 1);
    g.setColour (theme::text1);
    g.setFont (theme::sans (12.0f, theme::Sans::Medium));
    g.drawText ("Advanced", advancedArea.withTrimmedLeft (0), juce::Justification::centredLeft, false);
    Icons::draw (g, advancedOpen ? "chevron-down" : "chevron-right", juce::Point<float> ((float) getWidth() - 10.0f, (float) advancedArea.getCentreY()), 16.0f, theme::text2);

    if (advancedOpen)
    {
        const bool light = noise.getRequestedMode() == NoiseRemoval::Mode::light;
        auto row = [&] (juce::Rectangle<int> area, const juce::String& name, const juce::String& value, bool enabled)
        {
            g.setColour (enabled ? theme::text1 : theme::text2);
            g.setFont (label);
            g.drawText (name, area, juce::Justification::centredLeft, false);
            g.setFont (theme::mono (12.0f));
            g.drawText (value, area, juce::Justification::centredRight, false);
        };

        row (postFilterLabelArea, light ? juce::String ("Post-filter  (Fast and Best)") : juce::String ("Post-filter"),
             juce::String (juce::roundToInt (postFilter.getValue())) + "%", ! light);
        row (thresholdLabelArea, light ? juce::String ("Voice gate: how sure it must be that you talk") : juce::String ("Voice gate: how sure  (Light only)"),
             juce::String (juce::roundToInt (threshold.getValue())) + "%", light);
        row (holdLabelArea, light ? juce::String ("Voice gate: hold") : juce::String ("Voice gate: hold  (Light only)"),
             juce::String (juce::roundToInt (hold.getValue())) + " ms", light);

        g.setColour (theme::text2);
        g.setFont (theme::mono (11.0f));
        g.drawText (readoutText, readoutArea, juce::Justification::centredLeft, false);
    }
}
}
