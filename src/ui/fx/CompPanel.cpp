// CompPanel and its parts: transfer curve, scrolling history, gain-reduction meter, Amount knob, Details row.
#include "ui/fx/CompPanel.h"

#include "ui/Format.h"
#include "ui/HeadroomLookAndFeel.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/fx/SlotList.h"
#include "ui/widgets/HoverFade.h"

#include <cmath>

namespace hr
{
namespace
{
    // Images we scroll or cache are made in device pixels. Whole numbers only (the history moves a whole column at a
    // time): a fractional desktop scale such as 125 % draws the next size up, shrunk, which stays sharper than growing 1x.
    int scaleOf (juce::Graphics& g)
    {
        return juce::jmax (1, (int) std::ceil (g.getInternalContext().getPhysicalPixelScaleFactor() - 0.01f));
    }

    juce::Image makeImage (int width, int height)
    {
        return juce::Image (juce::Image::ARGB, juce::jmax (1, width), juce::jmax (1, height), true, juce::SoftwareImageType());
    }

    void setPlain (juce::RangedAudioParameter& p, float plainValue)
    {
        p.setValueNotifyingHost (p.convertTo0to1 (plainValue));
    }

    float roundTo (float value, float step)     { return std::round (value / step) * step; }

    // "-24" with a real minus sign, "0" for zero.
    juce::String axisLabel (int db)
    {
        return db == 0 ? juce::String ("0") : theme::glyph::minus + juce::String (-db);
    }
}

// ---------------------------------------------------------------------------------------------------
// CompCurve
// ---------------------------------------------------------------------------------------------------

CompCurve::CompCurve (Compressor& c) : comp (c)
{
    pThreshold = comp.apvts.getRawParameterValue ("threshold");
    pRatio = comp.apvts.getRawParameterValue ("ratio");
    pKnee = comp.apvts.getRawParameterValue ("knee");
    pMakeup = comp.apvts.getRawParameterValue ("makeup");
    rThreshold = comp.apvts.getParameter ("threshold");
    rRatio = comp.apvts.getParameter ("ratio");

    watcher = std::make_unique<ParamWatcher> (
        std::vector<juce::RangedAudioParameter*> { rThreshold, rRatio, comp.apvts.getParameter ("knee"), comp.apvts.getParameter ("makeup") },
        [this] { staticDirty = true; repaint(); });

    setMouseClickGrabsKeyboardFocus (false);
    setTooltip ("How loud sounds come out. Drag the round handle sideways to move the threshold, "
                "drag the line up or down to change the ratio. Double-click to reset.");
    setSize (kWidth, kHeight);
}

void CompCurve::resized()
{
    const float side = juce::jmin ((float) getWidth() - 28.0f, (float) getHeight() - 20.0f);
    plot = juce::Rectangle<float> (22.0f, 6.0f, side, side);
    staticDirty = true;
}

float CompCurve::xForDb (float db) const noexcept
{
    return plot.getX() + (db - kMinDb) / (kMaxDb - kMinDb) * plot.getWidth();
}

float CompCurve::yForDb (float db) const noexcept
{
    return plot.getBottom() - (db - kMinDb) / (kMaxDb - kMinDb) * plot.getHeight();
}

float CompCurve::curveDb (float inputDb) const noexcept
{
    return Compressor::outputDb (inputDb, threshold(), ratio(), knee(), makeup());
}

juce::Point<float> CompCurve::kneeHandlePosition() const
{
    // The corner of the line: where a hard knee would bend. Held inside the plot so it can always be grabbed.
    return { juce::jlimit (plot.getX(), plot.getRight(), xForDb (threshold())),
             juce::jlimit (plot.getY(), plot.getBottom(), yForDb (threshold() + makeup())) };
}

juce::Point<float> CompCurve::ratioGrabPoint() const
{
    const float thr = threshold();

    for (const float fraction : { 0.6f, 0.4f, 0.8f, 0.25f })
    {
        const float db = thr + (kMaxDb - thr) * fraction;
        const float y = yForDb (curveDb (db));

        if (y > plot.getY() + 4.0f && y < plot.getBottom() - 4.0f)
            return { xForDb (db), y };
    }

    return kneeHandlePosition();
}

CompCurve::Target CompCurve::targetAt (juce::Point<float> p) const
{
    const auto handle = kneeHandlePosition();

    if (p.getDistanceFrom (handle) <= 9.0f)
        return Target::knee;

    if (p.x > handle.x + 6.0f && plot.expanded (2.0f).contains (p))
    {
        const float db = kMinDb + (p.x - plot.getX()) / plot.getWidth() * (kMaxDb - kMinDb);

        if (std::abs (p.y - yForDb (curveDb (db))) <= 6.0f)
            return Target::line;
    }

    return Target::none;
}

void CompCurve::paintStatic (juce::Graphics& g) const
{
    g.setColour (theme::bg2);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), theme::kControlRadius);

    // Grid every 12 dB, on whole pixels; the labels are on the outer edge of the plot.
    g.setFont (theme::mono (9.0f));

    for (int db = -48; db <= -12; db += 12)
    {
        g.setColour (theme::line);
        g.fillRect (std::round (xForDb ((float) db)) - 0.5f, plot.getY(), 1.0f, plot.getHeight());
        g.fillRect (plot.getX(), std::round (yForDb ((float) db)) - 0.5f, plot.getWidth(), 1.0f);
    }

    g.setColour (theme::line);
    g.drawRect (plot, 1.0f);

    g.setColour (theme::text2);

    for (const int db : { -48, -24, 0 })
    {
        g.drawText (axisLabel (db), juce::Rectangle<float> (0.0f, yForDb ((float) db) - 6.0f, plot.getX() - 4.0f, 12.0f),
                    juce::Justification::centredRight, false);
        g.drawText (axisLabel (db), juce::Rectangle<float> (xForDb ((float) db) - 14.0f, plot.getBottom() + 2.0f, 28.0f, 12.0f),
                    juce::Justification::centred, false);
    }

    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (plot.getSmallestIntegerContainer());

        // 1:1, for reference.
        static const float dashes[] { 3.0f, 3.0f };
        g.setColour (theme::lineStrong);
        g.drawDashedLine (juce::Line<float> (xForDb (kMinDb), yForDb (kMinDb), xForDb (kMaxDb), yForDb (kMaxDb)), dashes, 2, 1.0f);

        const int steps = 240;
        juce::Path line, fill;

        for (int i = 0; i <= steps; ++i)
        {
            const float db = kMinDb + (kMaxDb - kMinDb) * (float) i / (float) steps;
            const float x = xForDb (db), y = yForDb (curveDb (db));

            if (i == 0)
            {
                line.startNewSubPath (x, y);
                fill.startNewSubPath (x, yForDb (db + makeup()));
            }
            else
            {
                line.lineTo (x, y);
                fill.lineTo (x, yForDb (db + makeup()));
            }
        }

        // The amber wedge is what the compressor takes away: between the line and where it would run without it.
        for (int i = steps; i >= 0; --i)
        {
            const float db = kMinDb + (kMaxDb - kMinDb) * (float) i / (float) steps;
            fill.lineTo (xForDb (db), yForDb (curveDb (db)));
        }

        fill.closeSubPath();

        g.setColour (theme::accent.withAlpha (0.18f));
        g.fillPath (fill);

        g.setColour (theme::text0);
        g.strokePath (line, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

void CompCurve::rebuildStatic (int scale)
{
    staticImage = makeImage (getWidth() * scale, getHeight() * scale);
    juce::Graphics ig (staticImage);
    ig.addTransform (juce::AffineTransform::scale ((float) scale));
    paintStatic (ig);
    staticScale = scale;
    drawn = { threshold(), ratio(), knee(), makeup() };
    staticDirty = false;
}

void CompCurve::paint (juce::Graphics& g)
{
    const int scale = scaleOf (g);

    // Compared with what the image was drawn from, so it is right even before the parameter watcher has run.
    if (staticDirty || staticScale != scale || ! staticImage.isValid()
        || drawn != std::array<float, 4> { threshold(), ratio(), knee(), makeup() })
        rebuildStatic (scale);

    const float alpha = dimmed ? 0.45f : 1.0f;
    g.setOpacity (alpha);
    g.drawImage (staticImage, getLocalBounds().toFloat());

    // Knee handle.
    const auto handle = kneeHandlePosition();
    const bool grabbed = dragging == Target::knee, hot = grabbed || hover == Target::knee;
    const auto ring = juce::Rectangle<float> (10.0f, 10.0f).withCentre (handle);

    g.setColour ((grabbed ? theme::text0 : (hot ? theme::bg3 : theme::bg1)).withMultipliedAlpha (alpha));
    g.fillEllipse (ring);
    g.setColour ((grabbed ? theme::bg1 : (hot ? theme::text0 : theme::text1)).withMultipliedAlpha (alpha));
    g.drawEllipse (ring.reduced (0.75f), 1.5f);

    // The dot on the curve at the current input level: amber while it is turning the sound down.
    if (dotVisible)
    {
        const juce::Point<float> at (xForDb (dotDb), juce::jlimit (plot.getY(), plot.getBottom(), yForDb (curveDb (dotDb))));
        g.setColour ((dotBright ? theme::accent : theme::text1).withMultipliedAlpha (alpha));
        g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (at));
    }

    // What the pointer is on.
    const auto shown = dragging != Target::none ? dragging : hover;

    if (shown != Target::none)
    {
        const bool onKnee = shown == Target::knee;
        const auto text = onKnee ? juce::String ("Threshold ") + fmt::db (threshold(), true, false, 0)
                                 : juce::String ("Ratio ") + juce::String (ratio(), 1) + ":1";
        const auto font = theme::mono (10.0f);
        const auto anchor = onKnee ? handle : ratioGrabPoint();
        auto box = juce::Rectangle<float> (theme::textWidth (font, text) + 14.0f, 20.0f).withCentre ({ anchor.x, anchor.y - 20.0f });
        box = box.constrainedWithin (getLocalBounds().toFloat().reduced (3.0f));

        g.setOpacity (1.0f);
        g.setColour (theme::bg0);
        g.fillRoundedRectangle (box, 4.0f);
        g.setColour (theme::lineStrong);
        g.drawRoundedRectangle (box.reduced (0.5f), 3.5f, 1.0f);
        g.setColour (theme::text0);
        g.setFont (font);
        g.drawText (text, box, juce::Justification::centred, false);
    }
}

juce::Rectangle<int> CompCurve::dotBounds() const
{
    const juce::Point<float> at (xForDb (dotDb), juce::jlimit (plot.getY(), plot.getBottom(), yForDb (curveDb (dotDb))));
    return juce::Rectangle<float> (12.0f, 12.0f).withCentre (at).getSmallestIntegerContainer();
}

void CompCurve::setDot (float inputDb)
{
    const bool visible = inputDb > kMinDb + 0.5f;
    const float newDb = visible ? juce::jmin (inputDb, kMaxDb) : kMinDb;
    const bool bright = visible && Compressor::reductionDb (newDb, threshold(), ratio(), knee()) > 0.1f;
    const bool moved = visible != dotVisible || bright != dotBright || (visible && std::abs (xForDb (newDb) - xForDb (dotDb)) >= 0.3f);

    if (! moved)
        return;

    const auto before = dotBounds();
    dotDb = newDb;
    dotVisible = visible;
    dotBright = bright;
    repaint (before.getUnion (dotBounds()));
}

void CompCurve::mouseMove (const juce::MouseEvent& e)
{
    const auto target = targetAt (e.position);

    if (target != hover)
    {
        hover = target;
        setMouseCursor (target == Target::knee ? juce::MouseCursor::LeftRightResizeCursor
                                               : (target == Target::line ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor));
        repaint();
    }
}

void CompCurve::mouseExit (const juce::MouseEvent&)
{
    if (hover != Target::none && dragging == Target::none)
    {
        hover = Target::none;
        repaint();
    }
}

void CompCurve::beginDrag (Target t, const juce::MouseEvent& e)
{
    dragging = t;
    dragFine = e.mods.isShiftDown();
    dragBaseMouse = e.position;
    dragBaseValue = t == Target::knee ? threshold() : rRatio->getValue();
    (t == Target::knee ? rThreshold : rRatio)->beginChangeGesture();
}

void CompCurve::endDrag()
{
    if (dragging == Target::none)
        return;

    (dragging == Target::knee ? rThreshold : rRatio)->endChangeGesture();
    dragging = Target::none;
}

void CompCurve::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        return;

    const auto target = targetAt (e.position);

    if (target == Target::none)
        return;

    beginDrag (target, e);
    repaint();
}

void CompCurve::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging == Target::none)
        return;

    // Shift mid-drag re-bases the drag, so the value does not jump.
    if (e.mods.isShiftDown() != dragFine)
    {
        dragFine = e.mods.isShiftDown();
        dragBaseMouse = e.position;
        dragBaseValue = dragging == Target::knee ? threshold() : rRatio->getValue();
    }

    if (dragging == Target::knee)
    {
        // Sideways only: the handle follows the pointer (a fifth of the way with Shift).
        const float dxDb = (e.position.x - dragBaseMouse.x) / plot.getWidth() * (kMaxDb - kMinDb) * (dragFine ? 0.2f : 1.0f);
        setPlain (*rThreshold, roundTo (juce::jlimit (kMinDb, kMaxDb, dragBaseValue + dxDb), 0.1f));
    }
    else
    {
        // Up or down only, through the parameter's own log-ish skew so 2:1 to 6:1 gets most of the travel. Down = firmer.
        const float norm = juce::jlimit (0.0f, 1.0f, dragBaseValue + (e.position.y - dragBaseMouse.y) / (dragFine ? 700.0f : 140.0f));
        const auto range = rRatio->getNormalisableRange();
        setPlain (*rRatio, juce::jlimit (range.start, range.end, roundTo (rRatio->convertFrom0to1 (norm), 0.1f)));
    }
}

void CompCurve::mouseUp (const juce::MouseEvent&)
{
    if (dragging == Target::none)
        return;

    endDrag();
    repaint();
}

void CompCurve::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto target = targetAt (e.position);

    if (target == Target::none)
        return;

    auto& p = target == Target::knee ? *rThreshold : *rRatio;
    p.beginChangeGesture();
    p.setValueNotifyingHost (p.getDefaultValue());
    p.endChangeGesture();
}

// ---------------------------------------------------------------------------------------------------
// CompHistory
// ---------------------------------------------------------------------------------------------------

CompHistory::CompHistory (Compressor& c) : comp (c)
{
    pThreshold = comp.apvts.getRawParameterValue ("threshold");
    watcher = std::make_unique<ParamWatcher> (std::vector<juce::RangedAudioParameter*> { comp.apvts.getParameter ("threshold") },
                                              [this] { repaint(); });
}

void CompHistory::resized()
{
    const int columns = juce::jmax (1, getWidth() - 28 - 6);
    plot = juce::Rectangle<float> (28.0f, 6.0f, (float) columns, (float) juce::jmax (1, getHeight() - 12));

    if (columns != numColumns)
    {
        numColumns = columns;
        ring.assign ((size_t) numColumns, HistoryColumn {});
        head = 0;
    }

    imageScale = 0;                 // redrawn at the next paint
    background = scroller = juce::Image();
}

void CompHistory::reset()
{
    ring.assign ((size_t) numColumns, HistoryColumn {});
    head = 0;
    newest = {};

    if (scroller.isValid())
        scroller.clear (scroller.getBounds());

    repaint();
}

const HistoryColumn& CompHistory::columnAt (int fromLeft) const noexcept
{
    return ring[(size_t) ((head + fromLeft) % numColumns)];
}

float CompHistory::yForColumnDb (float db) const noexcept
{
    return juce::jlimit (0.0f, plot.getHeight(), -db / -kFloorDb * plot.getHeight());
}

void CompHistory::drawColumn (juce::Graphics& g, int x, const HistoryColumn& c, const HistoryColumn* previous) const
{
    const float h = plot.getHeight(), fx = (float) x;

    // Input: a low-alpha area from the bottom.
    const float inTop = yForColumnDb (c.inDb);

    if (h - inTop >= 0.5f)
    {
        g.setColour (theme::text2.withAlpha (0.34f));
        g.fillRect (fx, inTop, 1.0f, h - inTop);

        // A thin cap so the input's peaks stay readable through the amber.
        g.setColour (theme::text1.withAlpha (0.5f));
        g.fillRect (fx, inTop, 1.0f, 1.0f);
    }

    // Gain reduction: an amber area hanging from the top, brighter along its lower edge.
    const float grPx = juce::jlimit (0.0f, h, -c.grDb / kGrFullScaleDb * h);

    if (grPx >= 0.4f)
    {
        g.setColour (theme::accent.withAlpha (0.30f));
        g.fillRect (fx, 0.0f, 1.0f, grPx);
        g.setColour (theme::accent.withAlpha (0.95f));
        g.fillRect (fx, juce::jmax (0.0f, grPx - 1.5f), 1.0f, juce::jmin (1.5f, grPx));
    }

    // Output: a lighter line, joined to its neighbour so a fast change is a vertical stroke and not a gap.
    if (c.outDb > kFloorDb)
    {
        const float y = yForColumnDb (c.outDb);
        float top = y, bottom = y;

        if (previous != nullptr && previous->outDb > kFloorDb)
        {
            const float yPrev = yForColumnDb (previous->outDb);
            top = juce::jmin (y, yPrev);
            bottom = juce::jmax (y, yPrev);
        }

        g.setColour (theme::text0.withAlpha (0.9f));
        g.fillRect (fx, top - 0.75f, 1.0f, bottom - top + 1.5f);
    }
}

void CompHistory::rebuildScroller()
{
    scroller = makeImage (numColumns * imageScale, juce::roundToInt (plot.getHeight()) * imageScale);
    juce::Graphics ig (scroller);
    ig.addTransform (juce::AffineTransform::scale ((float) imageScale));

    for (int x = 0; x < numColumns; ++x)
        drawColumn (ig, x, columnAt (x), x > 0 ? &columnAt (x - 1) : nullptr);
}

void CompHistory::ensureImages (int scale)
{
    if (imageScale == scale && background.isValid() && scroller.isValid())
        return;

    imageScale = scale;

    // The frame: rounded panel, a hairline every 12 dB, and the labels.
    background = makeImage (getWidth() * scale, getHeight() * scale);
    {
        juce::Graphics g (background);
        g.addTransform (juce::AffineTransform::scale ((float) scale));
        g.setColour (theme::bg2);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), theme::kControlRadius);

        g.setFont (theme::mono (9.0f));

        for (int db = 0; db >= -60; db -= 12)
        {
            const float y = plot.getY() + std::round (yForColumnDb ((float) db));

            if (db > -60 && db < 0)
            {
                g.setColour (theme::line);
                g.fillRect (plot.getX(), y - 0.5f, plot.getWidth(), 1.0f);
            }

            if (db == 0 || db == -24 || db == -48)
            {
                g.setColour (theme::text2);
                g.drawText (axisLabel (db), juce::Rectangle<float> (0.0f, y - 6.0f, plot.getX() - 4.0f, 12.0f),
                            juce::Justification::centredRight, false);
            }
        }
    }

    rebuildScroller();
}

void CompHistory::pushColumns (const HistoryColumn* columns, int count)
{
    if (count <= 0 || numColumns <= 0)
        return;

    if (count > numColumns)
    {
        columns += count - numColumns;
        count = numColumns;
    }

    for (int i = 0; i < count; ++i)
    {
        ring[(size_t) head] = columns[i];
        head = (head + 1) % numColumns;
    }

    newest = columns[count - 1];

    if (scroller.isValid() && imageScale > 0)
    {
        const int s = imageScale, heightPx = scroller.getHeight();

        scroller.moveImageSection (0, 0, count * s, 0, (numColumns - count) * s, heightPx);
        scroller.clear ({ (numColumns - count) * s, 0, count * s, heightPx });

        juce::Graphics ig (scroller);
        ig.addTransform (juce::AffineTransform::scale ((float) s));

        for (int j = 0; j < count; ++j)
        {
            const int x = numColumns - count + j;
            drawColumn (ig, x, columnAt (x), x > 0 ? &columnAt (x - 1) : nullptr);
        }
    }

    repaint (plot.getSmallestIntegerContainer());
}

void CompHistory::paint (juce::Graphics& g)
{
    ensureImages (scaleOf (g));

    const float alpha = dimmed ? 0.45f : 1.0f;
    g.setOpacity (alpha);
    g.drawImage (background, getLocalBounds().toFloat());
    g.drawImage (scroller, plot);

    // Threshold: a dashed hairline across the whole width.
    const float y = plot.getY() + std::round (yForColumnDb (pThreshold->load())) + 0.5f;
    static const float dashes[] { 3.0f, 3.0f };
    g.setColour (theme::text1.withAlpha (0.6f * alpha));
    g.drawDashedLine (juce::Line<float> (plot.getX(), y, plot.getRight(), y), dashes, 2, 1.0f);
}

// ---------------------------------------------------------------------------------------------------
// GrMeter
// ---------------------------------------------------------------------------------------------------

GrMeter::GrMeter()
{
    setTooltip ("How much your voice is being turned down right now. The white mark holds the most for a moment.");
}

void GrMeter::reset()
{
    shown = held = holdSeconds = 0.0f;
    lastShownPx = lastHeldPx = lastHeldTenths = -1;
    repaint();
}

void GrMeter::push (float reductionDb, double dt)
{
    reductionDb = juce::jlimit (0.0f, kRangeDb + 6.0f, reductionDb);
    shown = juce::jmax (reductionDb, shown - (float) dt * 60.0f);

    // 1.5 s peak hold, then it falls at 20 dB/s (the same ballistics as the level meters).
    if (reductionDb >= held)
    {
        held = reductionDb;
        holdSeconds = 0.0f;
    }
    else
    {
        holdSeconds += (float) dt;

        if (holdSeconds > 1.5f)
            held = juce::jmax (shown, held - (float) dt * 20.0f);
    }

    const float barHeight = (float) (getHeight() - kReadoutHeight - 2 * kBarInset);
    const int shownPx = juce::roundToInt (juce::jmin (shown, kRangeDb) / kRangeDb * barHeight);
    const int heldPx = juce::roundToInt (juce::jmin (held, kRangeDb) / kRangeDb * barHeight);
    const int heldTenths = juce::roundToInt (held * 10.0f);

    if (shownPx != lastShownPx || heldPx != lastHeldPx || heldTenths != lastHeldTenths)
    {
        lastShownPx = shownPx;
        lastHeldPx = heldPx;
        lastHeldTenths = heldTenths;
        repaint();
    }
}

void GrMeter::paint (juce::Graphics& g)
{
    const float alpha = dimmed ? 0.45f : 1.0f;
    const int panelHeight = getHeight() - kReadoutHeight;

    g.setColour (theme::bg2);
    g.fillRoundedRectangle (juce::Rectangle<float> ((float) getWidth(), (float) panelHeight), theme::kControlRadius);

    const auto bar = juce::Rectangle<float> (10.0f, (float) kBarInset, 10.0f, (float) (panelHeight - 2 * kBarInset));
    g.setColour (theme::bg1);
    g.fillRoundedRectangle (bar, theme::kMeterRadius);

    // Scale: a tick and a label every 6 dB.
    g.setFont (theme::mono (9.0f));

    for (int db = 6; db <= 24; db += 6)
    {
        const float y = std::round (bar.getY() + (float) db / kRangeDb * bar.getHeight());
        g.setColour (theme::lineStrong);
        g.fillRect (bar.getRight() + 3.0f, y - 0.5f, 4.0f, 1.0f);
        g.setColour (theme::text2);
        g.drawText (juce::String (db), juce::Rectangle<float> (bar.getRight() + 9.0f, y - 6.0f, 20.0f, 12.0f), juce::Justification::centredLeft, false);
    }

    // Amber from the top down, like the history.
    const float fillHeight = juce::jmin (shown, kRangeDb) / kRangeDb * bar.getHeight();

    if (fillHeight > 0.5f)
    {
        g.setColour (theme::accent.withAlpha (alpha));
        g.fillRoundedRectangle (bar.withHeight (juce::jmax (fillHeight, theme::kMeterRadius * 2.0f)), theme::kMeterRadius);
    }

    if (held > 0.2f)
    {
        const float y = bar.getY() + juce::jmin (held, kRangeDb) / kRangeDb * bar.getHeight();
        g.setColour (theme::text0.withAlpha (alpha));
        g.fillRect (bar.getX() - 2.0f, juce::jmin (y, bar.getBottom() - 1.5f) - 0.75f, bar.getWidth() + 4.0f, 1.5f);
    }

    // The held peak as a number.
    g.setFont (theme::mono (11.0f));
    g.setColour ((held > 0.1f ? theme::accent : theme::text2).withMultipliedAlpha (alpha));
    g.drawText (fmt::db (-held), juce::Rectangle<int> (0, panelHeight + 2, getWidth(), kReadoutHeight - 2), juce::Justification::centred, false);
}

// ---------------------------------------------------------------------------------------------------
// AmountKnob
// ---------------------------------------------------------------------------------------------------

namespace
{
    constexpr float kStartAngle = juce::MathConstants<float>::pi * 1.25f;
    constexpr float kEndAngle = juce::MathConstants<float>::pi * 2.75f;
    constexpr float kDialDiameter = 88.0f;
}

AmountKnob::AmountKnob (Compressor& c)
    : comp (c),
      param (*c.apvts.getParameter ("amount")),
      attachment (param, [this] (float v) { plainValue = v; repaint(); })
{
    watcher = std::make_unique<ParamWatcher> (std::vector<juce::RangedAudioParameter*> { comp.apvts.getParameter ("amountCustom") },
                                              [this] { repaint(); });
    setWantsKeyboardFocus (true);
    setMouseClickGrabsKeyboardFocus (false);
    setTooltip ("One knob for the whole compressor: turn it up to even out your voice more. "
                "Double-click for the standard setting.");
    setSize (kWidth, kHeight);
    attachment.sendInitialUpdate();
}

juce::Point<float> AmountKnob::getDialCentre() const
{
    return { (float) getWidth() * 0.5f, 4.0f + kDialDiameter * 0.5f };
}

juce::String AmountKnob::getReadout() const
{
    return comp.isAmountCustom() ? juce::String ("Custom") : juce::String (juce::roundToInt (plainValue)) + "%";
}

void AmountKnob::paint (juce::Graphics& g)
{
    const auto centre = getDialCentre();
    const auto dial = juce::Rectangle<float> (kDialDiameter, kDialDiameter).withCentre (centre);
    const float norm = param.convertTo0to1 (plainValue);
    const float angle = kStartAngle + norm * (kEndAngle - kStartAngle);
    const float alpha = dimmed ? 0.4f : 1.0f;
    const bool custom = comp.isAmountCustom();

    // Track and value arc. A custom setting is not "an amount", so its arc goes quiet.
    const float arcRadius = kDialDiameter * 0.5f - 2.0f;
    juce::Path track, value;
    track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, kStartAngle, kEndAngle, true);
    value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, kStartAngle, angle, true);

    g.setColour (theme::line.withAlpha (alpha));
    g.strokePath (track, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    if (norm > 0.004f)
    {
        g.setColour ((custom ? theme::text2 : theme::accent).withAlpha (alpha));
        g.strokePath (value, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Body, pointer on the rim, the reading in the middle.
    const auto body = dial.reduced (11.0f);
    g.setColour ((hover || dragging ? theme::bg3 : theme::bg2).withAlpha (alpha));
    g.fillEllipse (body);
    g.setColour (theme::lineStrong.withAlpha (alpha));
    g.drawEllipse (body.reduced (0.5f), 1.0f);

    const float bodyRadius = body.getWidth() * 0.5f;
    const auto dir = juce::Point<float> (std::sin (angle), -std::cos (angle));
    g.setColour ((custom ? theme::text1 : theme::text0).withAlpha (alpha));
    g.drawLine (juce::Line<float> (centre + dir * (bodyRadius - 10.0f), centre + dir * (bodyRadius - 3.5f)), 2.0f);

    if (hasKeyboardFocus (false))
    {
        g.setColour (theme::accent);
        g.drawEllipse (dial.expanded (3.0f), 1.0f);
    }

    const auto readout = juce::Rectangle<float> (0.0f, 0.0f, bodyRadius * 1.5f, 18.0f).withCentre (centre);
    g.setColour ((custom ? theme::text1 : theme::text0).withAlpha (alpha));
    g.setFont (custom ? theme::sans (11.0f, theme::Sans::Medium) : theme::mono (13.0f, theme::Mono::Medium));
    g.drawText (getReadout(), readout, juce::Justification::centred, false);

    g.setColour ((dragging ? theme::text0 : theme::text1).withAlpha (dimmed ? 0.55f : 1.0f));
    g.setFont (theme::sans (12.0f, theme::Sans::Medium));
    g.drawText ("Amount", juce::Rectangle<float> (0.0f, dial.getBottom() + 8.0f, (float) getWidth(), 16.0f), juce::Justification::centred, true);

    g.setColour (theme::text2.withAlpha (dimmed ? 0.55f : 1.0f));
    g.setFont (theme::sans (11.0f));
    g.drawText (getCaption(), juce::Rectangle<float> (0.0f, dial.getBottom() + 25.0f, (float) getWidth(), 14.0f), juce::Justification::centred, true);
}

juce::String AmountKnob::getCaption() const
{
    if (comp.isAmountCustom())
        return "Double-click to reset";

    if (plainValue < 0.5f)      return "Off";
    if (plainValue <= 20.0f)    return "Light";
    if (plainValue <= 40.0f)    return "Gentle";
    if (plainValue <= 60.0f)    return "Balanced";
    if (plainValue <= 80.0f)    return "Strong";

    return "Heavy";
}

void AmountKnob::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        return;

    dragging = true;
    dragFine = e.mods.isShiftDown();
    dragStartNorm = param.getValue();
    dragStartY = (float) e.y;
    attachment.beginGesture();
    repaint();
}

void AmountKnob::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging)
        return;

    if (e.mods.isShiftDown() != dragFine)
    {
        dragFine = e.mods.isShiftDown();
        dragStartNorm = param.getValue();
        dragStartY = (float) e.y;
    }

    const float pixelsForFullRange = dragFine ? 850.0f : 170.0f;
    const float norm = juce::jlimit (0.0f, 1.0f, dragStartNorm + (dragStartY - (float) e.y) / pixelsForFullRange);
    attachment.setValueAsPartOfGesture (param.convertFrom0to1 (norm));
}

void AmountKnob::mouseUp (const juce::MouseEvent&)
{
    if (dragging)
    {
        dragging = false;
        attachment.endGesture();
        repaint();
    }
}

void AmountKnob::mouseDoubleClick (const juce::MouseEvent&)
{
    // Back to the standard setting - also when Amount already says 50 but the details were changed.
    param.beginChangeGesture();
    comp.applyAmount (Compressor::kDefaultAmount);
    param.endChangeGesture();
}

void AmountKnob::nudge (float normalisedDelta)
{
    const float norm = juce::jlimit (0.0f, 1.0f, param.getValue() + normalisedDelta);
    attachment.setValueAsCompleteGesture (param.convertFrom0to1 (norm));
}

void AmountKnob::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    // Like every knob in a scrolling list: only Shift / Ctrl + wheel turns it.
    if (! e.mods.isShiftDown() && ! e.mods.isCtrlDown())
    {
        juce::Component::mouseWheelMove (e, wheel);
        return;
    }

    const float direction = (wheel.deltaY > 0.0f ? 1.0f : (wheel.deltaY < 0.0f ? -1.0f : 0.0f)) * (wheel.isReversed ? -1.0f : 1.0f);

    if (direction != 0.0f)
        nudge (direction * (e.mods.isCtrlDown() ? 0.01f : 0.05f));
}

bool AmountKnob::keyPressed (const juce::KeyPress& key)
{
    const float step = key.getModifiers().isShiftDown() ? 0.01f : 0.05f;
    const int code = key.getKeyCode();

    if (code == juce::KeyPress::upKey || code == juce::KeyPress::rightKey)      { nudge (step);  return true; }
    if (code == juce::KeyPress::downKey || code == juce::KeyPress::leftKey)     { nudge (-step); return true; }

    return false;
}

// ---------------------------------------------------------------------------------------------------
// DetailsToggle
// ---------------------------------------------------------------------------------------------------

DetailsToggle::DetailsToggle() : juce::Button ("Details")
{
    setClickingTogglesState (true);
    setMouseClickGrabsKeyboardFocus (false);
    setWantsKeyboardFocus (true);
    setTooltip ("Show the individual settings");
    HoverFade::attach (*this);
}

void DetailsToggle::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto bounds = getLocalBounds().toFloat();
    const float hover = down ? 1.0f : HoverFade::get (*this, highlighted);

    g.setColour (theme::line);
    g.fillRect (0.0f, 0.0f, bounds.getWidth(), 1.0f);

    if (hasKeyboardFocus (false))
        HeadroomLookAndFeel::drawFocusRing (g, bounds.reduced (0.0f, 2.0f), theme::kControlRadius);

    g.setColour (theme::text1.interpolatedWith (theme::text0, hover));
    g.setFont (theme::sans (12.0f, theme::Sans::Medium));
    g.drawText ("Details", juce::Rectangle<float> (4.0f, 1.0f, bounds.getWidth() - 40.0f, bounds.getHeight() - 1.0f),
                juce::Justification::centredLeft, false);

    Icons::draw (g, getToggleState() ? "chevron-down" : "chevron-right",
                 juce::Point<float> (bounds.getRight() - 14.0f, bounds.getCentreY() + 0.5f), 16.0f,
                 theme::text1.interpolatedWith (theme::text0, hover));
}

// ---------------------------------------------------------------------------------------------------
// CompPanel
// ---------------------------------------------------------------------------------------------------

namespace
{
    struct DetailSpec
    {
        const char* paramId;
        const char* label;
        const char* tip;
        bool isSigned;
    };

    constexpr DetailSpec kDetails[] {
        { "threshold", "Threshold", "Sounds louder than this are turned down.", false },
        { "ratio",     "Ratio",     "How firmly loud sounds are turned down.", false },
        { "attack",    "Attack",    "How quickly it reacts to a loud sound.", false },
        { "release",   "Release",   "How quickly it lets go afterwards.", false },
        { "knee",      "Knee",      "Softens the point where turning down starts.", false },
        { "makeup",    "Makeup",    "Brings the overall level back up.", true },
    };

    constexpr int kMeterColumnWidth = 52;
    constexpr int kCurveGap = 12;
    constexpr int kTop = 4;
}

CompPanel::CompPanel (Compressor& compressor)
    : comp (compressor), history (compressor), curve (compressor), amountKnob (compressor)
{
    history.setTooltip ("The last 5 seconds: what comes in (grey), what goes out (line) and how much it is turned down (amber). "
                       "The dashed line is the threshold.");
    addAndMakeVisible (history);
    addAndMakeVisible (grMeter);
    addAndMakeVisible (curve);
    addAndMakeVisible (amountKnob);

    detailsToggle.onClick = [this] { setDetailsOpen (detailsToggle.getToggleState()); };
    addAndMakeVisible (detailsToggle);

    for (const auto& spec : kDetails)
    {
        if (auto* p = comp.apvts.getParameter (spec.paramId))
        {
            auto knob = std::make_unique<ParamKnob> (*p, spec.label, spec.isSigned);
            knob->setTooltip (spec.tip);
            addChildComponent (*knob);
            knobs.push_back (std::move (knob));
        }
    }

    pending.reserve (2048);
    setSize (kWidth, getPreferredHeight());
    startTimer (500);                   // catches "the window went away" while the shared 30 fps clock is idle
}

CompPanel::~CompPanel()
{
    stopTimer();
    comp.displayActive.store (false, std::memory_order_relaxed);
}

int CompPanel::heightWithDetails (bool open) const noexcept
{
    const int base = kTop + kHistoryHeight + GrMeter::kReadoutHeight + kGap + kRowHeight + 4 + kDetailsRow + 4;
    return base + (open ? 2 * ParamKnob::kHeight : 0);
}

int CompPanel::getPreferredHeight() const
{
    return heightWithDetails (detailsOpen);
}

void CompPanel::setDetailsOpen (bool shouldOpen)
{
    if (shouldOpen == detailsOpen)
        return;

    detailsOpen = shouldOpen;
    detailsToggle.setToggleState (shouldOpen, juce::dontSendNotification);
    detailsToggle.setTooltip (shouldOpen ? "Hide the individual settings" : "Show the individual settings");

    for (auto& knob : knobs)
        knob->setVisible (shouldOpen);

    setSize (kWidth, getPreferredHeight());

    // The drawer's row has to grow or shrink with us.
    if (auto* row = findParentComponentOfClass<SlotRow>())
        if (row->onLayoutChanged)
            row->onLayoutChanged();

    repaint();
}

void CompPanel::setDimmed (bool shouldDim)
{
    dimmed = shouldDim;
    history.setDimmed (shouldDim);
    grMeter.setDimmed (shouldDim);
    curve.setDimmed (shouldDim);
    amountKnob.setDimmed (shouldDim);

    for (auto& knob : knobs)
        knob->setDimmed (shouldDim);

    repaint();
}

void CompPanel::resized()
{
    const int historyWidth = kWidth - kMeterColumnWidth - 8;
    history.setBounds (0, kTop, historyWidth, kHistoryHeight);
    grMeter.setBounds (kWidth - kMeterColumnWidth, kTop, kMeterColumnWidth, kHistoryHeight + GrMeter::kReadoutHeight);

    const int rowY = kTop + kHistoryHeight + GrMeter::kReadoutHeight + kGap;
    curve.setBounds (0, rowY, CompCurve::kWidth, CompCurve::kHeight);

    const int cellX = CompCurve::kWidth + kCurveGap;
    amountKnob.setBounds (cellX + (kWidth - cellX - AmountKnob::kWidth) / 2, rowY + (kRowHeight - AmountKnob::kHeight) / 2,
                          AmountKnob::kWidth, AmountKnob::kHeight);

    const int detailsY = rowY + kRowHeight + 4;
    detailsToggle.setBounds (0, detailsY, kWidth, kDetailsRow);

    int i = 0;

    for (auto& knob : knobs)
    {
        knob->setTopLeftPosition ((i % 4) * ParamKnob::kWidth, detailsY + kDetailsRow + (i / 4) * ParamKnob::kHeight);
        ++i;
    }
}

void CompPanel::paint (juce::Graphics& g)
{
    // Legend under the history: what the three shapes are.
    const float alpha = dimmed ? 0.5f : 1.0f;
    const float y = (float) (kTop + kHistoryHeight + 4);
    const auto font = theme::sans (11.0f);
    float x = 28.0f;

    g.setFont (font);

    auto item = [&] (const juce::String& text, auto drawSwatch)
    {
        drawSwatch (x, y + 7.0f);
        x += 16.0f;
        g.setColour (theme::text2.withMultipliedAlpha (alpha));
        g.drawText (text, juce::Rectangle<float> (x, y, 80.0f, 14.0f), juce::Justification::centredLeft, false);
        x += theme::textWidth (font, text) + 14.0f;
    };

    item ("In", [&] (float sx, float sy)
    {
        g.setColour (theme::text2.withAlpha (0.55f * alpha));
        g.fillRoundedRectangle (sx, sy - 4.0f, 12.0f, 8.0f, 2.0f);
    });
    item ("Out", [&] (float sx, float sy)
    {
        g.setColour (theme::text0.withAlpha (0.9f * alpha));
        g.fillRect (sx, sy - 0.75f, 12.0f, 1.5f);
    });
    item ("Turned down", [&] (float sx, float sy)
    {
        g.setColour (theme::accent.withAlpha (0.5f * alpha));
        g.fillRoundedRectangle (sx, sy - 4.0f, 12.0f, 8.0f, 2.0f);
    });
}

// ---------------------------------------------------------------------------------------------------
// Live data
// ---------------------------------------------------------------------------------------------------

bool CompPanel::isOnScreen() const
{
    if (getWidth() <= 0 || getHeight() <= 0)
        return false;

    // Every component below the root has to be visible and the panel has to reach the visible part of each parent (it
    // may be scrolled out of the drawer). The root itself only counts when it lives in a real window: screenshot mode
    // has none, and a hidden or minimised window has nothing to show.
    auto area = getLocalBounds();
    const juce::Component* child = this;

    while (auto* parent = child->getParentComponent())
    {
        if (! child->isVisible())
            return false;

        area = parent->getLocalArea (child, area).getIntersection (parent->getLocalBounds());

        if (area.isEmpty())
            return false;

        child = parent;
    }

    if (auto* peer = child->getPeer())
        return child->isVisible() && ! peer->isMinimised();

    return child == this ? isVisible() : true;
}

void CompPanel::updateActive()
{
    const bool show = isOnScreen();

    if (show == active)
        return;

    active = show;

    if (show)
    {
        comp.clearDisplay();                    // nothing old: history starts when it is looked at
        pending.clear();
        history.reset();
        grMeter.reset();
        columnCarry = 0.0f;
        secondsSinceRecord = 10.0f;
        lastColumn = {};
        dotDb = -100.0f;
    }
    else
    {
        curve.setDot (-100.0f);
    }

    comp.displayActive.store (show, std::memory_order_relaxed);
}

void CompPanel::tickMeter (double dt)
{
    updateActive();

    if (! active)
        return;

    // 1. What the audio thread wrote since the last frame.
    float grThisFrame = 0.0f;
    bool gotRecords = false;
    Compressor::DisplayRecord buffer[128];

    for (;;)
    {
        const int n = comp.drainDisplay (buffer, 128);

        if (n <= 0)
            break;

        gotRecords = true;
        pending.insert (pending.end(), buffer, buffer + n);

        for (int i = 0; i < n; ++i)
            grThisFrame = juce::jmax (grThisFrame, -buffer[i].gainReductionDb);

        if (n < 128)
            break;
    }

    if (pending.size() > 2048)
        pending.erase (pending.begin(), pending.end() - 2048);

    secondsSinceRecord = gotRecords ? 0.0f : secondsSinceRecord + (float) dt;

    // 2. The clock decides how many columns to add: (columns per second) x dt, carrying the remainder, so the picture
    //    scrolls at a steady pace whatever the rhythm of the audio callbacks.
    columnCarry += (float) dt * history.getColumnsPerSecond();
    const int wanted = (int) columnCarry;
    columnCarry -= (float) wanted;
    const int numCols = juce::jmin (wanted, history.getNumColumns());

    if (numCols > 0)
    {
        scratch.resize ((size_t) numCols);
        const int records = (int) pending.size();

        for (int j = 0; j < numCols; ++j)
        {
            HistoryColumn column;

            if (records > 0)
            {
                int from = j * records / numCols, to = (j + 1) * records / numCols;

                if (to <= from)
                {
                    from = juce::jmin (from, records - 1);
                    to = from + 1;
                }

                for (int k = from; k < to; ++k)
                {
                    column.inDb = juce::jmax (column.inDb, pending[(size_t) k].inputPeakDb);
                    column.outDb = juce::jmax (column.outDb, pending[(size_t) k].outputPeakDb);
                    column.grDb = juce::jmin (column.grDb, pending[(size_t) k].gainReductionDb);
                }
            }
            else if (secondsSinceRecord < 0.25f)
            {
                column = lastColumn;            // a late callback is not silence
            }

            scratch[(size_t) j] = column;
        }

        lastColumn = scratch.back();
        pending.clear();
        history.pushColumns (scratch.data(), numCols);
    }

    // 3. The meter and the dot.
    if (gotRecords)                             lastGr = grThisFrame;
    else if (secondsSinceRecord >= 0.25f)       lastGr = 0.0f;

    grMeter.push (lastGr, dt);

    const float target = comp.getDetectorLevelDb();
    dotDb += (target - dotDb) * (1.0f - std::exp (-(float) dt / (target > dotDb ? 0.03f : 0.25f)));
    curve.setDot (dotDb);
}
}
