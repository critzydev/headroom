// LevelFader: piecewise dB taper, cap, scale, meter and mouse / keyboard handling.
#include "ui/widgets/LevelFader.h"

#include "ui/Format.h"
#include "ui/HeadroomLookAndFeel.h"
#include "ui/Theme.h"

namespace hr
{
namespace
{
    struct TaperPoint { float travel, db; };

    // Console-style taper: 0 dB at 80 % of the travel, +6 dB at the top, generous room around unity, and a long
    // quiet end. Below the first point the fader is at -inf.
    constexpr TaperPoint kTaper[] = {
        { 0.02f, -72.0f }, { 0.14f, -48.0f }, { 0.36f, -24.0f }, { 0.56f, -12.0f },
        { 0.68f, -6.0f },  { 0.80f, 0.0f },   { 1.00f, 6.0f }
    };
    constexpr int kTaperCount = (int) (sizeof (kTaper) / sizeof (kTaper[0]));

    constexpr int kCapWidth = 40, kCapHeight = 20;
    constexpr int kLabelWidth = 24, kTickLength = 5;
    constexpr int kAssemblyWidth = kLabelWidth + 3 + kTickLength + 3 + kCapWidth + 1;      // 76: even, centres cleanly
    constexpr float kMinFiniteDb = -72.0f;
    constexpr int kTrackWidth = 8;                                                          // two 3 px bars + 2 px gap

    const float kScaleDb[] = { 6.0f, 0.0f, -6.0f, -12.0f, -24.0f, -48.0f, kMinusInfDb };
}

LevelFader::LevelFader()
{
    setWantsKeyboardFocus (true);
    setMouseClickGrabsKeyboardFocus (false);
    travel = dbToTravel (db);
}

float LevelFader::travelToDb (float t) noexcept
{
    if (t < kTaper[0].travel)
        return kMinusInfDb;

    for (int i = 1; i < kTaperCount; ++i)
    {
        if (t <= kTaper[i].travel)
        {
            const auto& a = kTaper[i - 1];
            const auto& b = kTaper[i];
            return a.db + (b.db - a.db) * (t - a.travel) / (b.travel - a.travel);
        }
    }

    return kTaper[kTaperCount - 1].db;
}

float LevelFader::dbToTravel (float value) noexcept
{
    if (value <= kMinusInfDb)
        return 0.0f;

    if (value <= kMinFiniteDb)
        return kTaper[0].travel;

    for (int i = 1; i < kTaperCount; ++i)
    {
        if (value <= kTaper[i].db)
        {
            const auto& a = kTaper[i - 1];
            const auto& b = kTaper[i];
            return a.travel + (b.travel - a.travel) * (value - a.db) / (b.db - a.db);
        }
    }

    return 1.0f;
}

void LevelFader::setDb (float newDb)
{
    newDb = newDb <= kMinusInfDb ? kMinusInfDb : juce::jmin (newDb, 6.0f);

    if (dragging || newDb == db)        // while dragging the model is only echoing what we just sent
        return;

    db = newDb;
    travel = dbToTravel (db);
    repaint();
}

void LevelFader::setMuted (bool isMuted)
{
    if (muted != isMuted)
    {
        muted = isMuted;
        repaint();
    }
}

void LevelFader::setReadoutVisible (bool visible)
{
    showReadout = visible;
    readoutHeight = visible ? 20 : 0;
    resized();
    repaint();
}

void LevelFader::resized()
{
    const int x0 = (getWidth() - kAssemblyWidth) / 2;
    labelRight = x0 + kLabelWidth;
    tickLeft = labelRight + 3;
    capLeft = tickLeft + kTickLength + 3;
    capRight = capLeft + kCapWidth;

    travelTop = kCapHeight / 2 + 6;
    travelBottom = getHeight() - readoutHeight - kCapHeight / 2 - 6;
}

float LevelFader::travelToY (float t) const noexcept
{
    return (float) travelBottom - t * (float) (travelBottom - travelTop);
}

float LevelFader::yToTravel (float y) const noexcept
{
    return juce::jlimit (0.0f, 1.0f, ((float) travelBottom - y) / (float) juce::jmax (1, travelBottom - travelTop));
}

juce::Rectangle<int> LevelFader::getCapBounds() const noexcept
{
    const int y = juce::roundToInt (travelToY (travel));
    return { capLeft, y - kCapHeight / 2, kCapWidth, kCapHeight };
}

juce::Rectangle<int> LevelFader::getTrackBounds() const noexcept
{
    const int cx = capLeft + kCapWidth / 2;
    return { cx - kTrackWidth / 2, travelTop - 4, kTrackWidth, travelBottom - travelTop + 8 };
}

bool LevelFader::isInFaderColumn (juce::Point<int> p) const noexcept
{
    return p.x >= capLeft - 4 && p.x <= capRight + 4 && p.y >= 0 && p.y <= travelBottom + kCapHeight;
}

// ---------------------------------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------------------------------

void LevelFader::paint (juce::Graphics& g)
{
    const auto clip = g.getClipBounds();

    if (clip.intersects (juce::Rectangle<int> (0, 0, capLeft - 2, travelBottom + kCapHeight)))
        drawScale (g);

    const auto track = getTrackBounds();
    g.setColour (theme::bg2);
    g.fillRoundedRectangle (track.toFloat(), theme::kMeterRadius);

    drawMeter (g);
    drawCap (g);

    if (showReadout)
    {
        g.setColour (dragging ? theme::text0 : theme::text1);
        g.setFont (theme::mono (11.0f));
        g.drawText (fmt::db (db), juce::Rectangle<int> (0, getHeight() - readoutHeight, capRight, readoutHeight),
                    juce::Justification::centredRight, false);
    }
}

void LevelFader::drawScale (juce::Graphics& g) const
{
    g.setFont (theme::mono (9.0f));

    for (const auto value : kScaleDb)
    {
        const int y = juce::roundToInt (travelToY (dbToTravel (value)));
        const bool unity = value == 0.0f;

        g.setColour (unity ? theme::text2 : theme::lineStrong);
        g.fillRect (tickLeft, y, kTickLength, 1);

        const auto label = value <= kMinusInfDb ? theme::glyph::minus + theme::glyph::infinity
                                                : (value > 0.0f ? "+" + juce::String ((int) value) : fmt::db (value, false, false, 0));

        g.setColour (unity ? theme::text1 : theme::text2);
        g.drawText (label, juce::Rectangle<int> (labelRight - kLabelWidth, y - 6, kLabelWidth, 12), juce::Justification::centredRight, false);
    }
}

void LevelFader::drawMeter (juce::Graphics& g) const
{
    const auto track = getTrackBounds();
    const float bottom = (float) travelBottom;
    const float y12 = travelToY (dbToTravel (-12.0f));
    const float y3 = travelToY (dbToTravel (-3.0f));

    auto drawBar = [&] (float x, const MeterBar& bar)
    {
        if (bar.displayDb > MeterBar::kFloorDb)
        {
            const float top = travelToY (dbToTravel (juce::jmin (bar.displayDb, 6.0f)));

            auto segment = [&] (float from, float to, juce::Colour colour)
            {
                if (to < from)
                {
                    g.setColour (colour);
                    g.fillRect (x, to, 3.0f, from - to);
                }
            };

            segment (bottom, juce::jmax (top, y12), theme::ok);
            if (top < y12)  segment (y12, juce::jmax (top, y3), theme::warn);
            if (top < y3)   segment (y3, top, theme::bad);
        }

        if (bar.peakDb > MeterBar::kFloorDb && bar.peakDb > bar.displayDb + 0.5f)
        {
            const float py = std::round (travelToY (dbToTravel (juce::jmin (bar.peakDb, 6.0f))));
            g.setColour (bar.peakDb >= -3.0f ? theme::bad : (bar.peakDb >= -12.0f ? theme::warn : theme::ok));
            g.fillRect (x, py, 3.0f, 2.0f);
        }
    };

    drawBar ((float) track.getX(), meterL);
    drawBar ((float) track.getX() + 5.0f, meterR);
}

void LevelFader::drawCap (juce::Graphics& g) const
{
    const auto cap = getCapBounds().toFloat();
    auto top = theme::capTop, bottom = theme::capBottom;

    if (muted)          { top = top.darker (0.45f); bottom = bottom.darker (0.45f); }
    else if (hoverCap || dragging)  { top = top.brighter (0.06f); bottom = bottom.brighter (0.06f); }

    g.setGradientFill (juce::ColourGradient (top, 0.0f, cap.getY(), bottom, 0.0f, cap.getBottom(), false));
    g.fillRoundedRectangle (cap, 4.0f);

    g.setColour (juce::Colours::black.withAlpha (0.4f));
    g.drawRoundedRectangle (cap.reduced (0.5f), 3.5f, 1.0f);

    g.setColour (juce::Colours::white.withAlpha (muted ? 0.08f : 0.28f));
    g.fillRect (cap.getX() + 4.0f, cap.getY() + 1.0f, cap.getWidth() - 8.0f, 1.0f);

    g.setColour (juce::Colour (0xff26231f));
    g.fillRect (cap.getX() + 6.0f, cap.getY() + 10.0f, cap.getWidth() - 12.0f, 1.0f);

    if (hasKeyboardFocus (false))
        HeadroomLookAndFeel::drawFocusRing (g, cap.expanded (3.0f), 6.0f);
}

void LevelFader::setLevels (const MeterBar& left, const MeterBar& right)
{
    meterL = left;
    meterR = right;

    auto px = [this] (float value)
    {
        return value <= MeterBar::kFloorDb ? -1 : juce::roundToInt (travelToY (dbToTravel (juce::jmin (value, 6.0f))));
    };

    const int now[4] { px (left.displayDb), px (right.displayDb), px (left.peakDb), px (right.peakDb) };
    bool changed = false;

    for (int i = 0; i < 4; ++i)
    {
        changed = changed || now[i] != lastBarPx[i];
        lastBarPx[i] = now[i];
    }

    if (changed)
        repaint (getTrackBounds().expanded (0, 2));
}

void LevelFader::clearLevels()
{
    setLevels ({}, {});
}

// ---------------------------------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------------------------------

void LevelFader::userSetDb (float newDb)
{
    newDb = newDb <= kMinusInfDb ? kMinusInfDb : juce::jlimit (kMinusInfDb, 6.0f, newDb);

    if (newDb == db)
        return;

    db = newDb;
    travel = dbToTravel (db);
    repaint();

    if (onChange)
        onChange (db);
}

void LevelFader::mouseDown (const juce::MouseEvent& e)
{
    if (! isInFaderColumn (e.getPosition()) || e.mods.isPopupMenu())
        return;

    dragging = true;
    dragFine = e.mods.isShiftDown();

    // Grabbing the cap keeps its offset; clicking the track jumps the cap under the pointer.
    if (! getCapBounds().expanded (2).contains (e.getPosition()) && ! dragFine)
    {
        travel = yToTravel ((float) e.y);
        const auto newDb = travelToDb (travel);
        db = newDb;

        if (onChange)
            onChange (db);
    }

    dragStartTravel = travel;
    dragStartY = (float) e.y;
    repaint();
}

void LevelFader::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging)
        return;

    if (e.mods.isShiftDown() != dragFine)       // re-baseline when Shift is pressed / released mid-drag
    {
        dragFine = e.mods.isShiftDown();
        dragStartTravel = travel;
        dragStartY = (float) e.y;
    }

    const float range = (float) juce::jmax (1, travelBottom - travelTop);
    const float delta = (dragStartY - (float) e.y) / range * (dragFine ? 0.1f : 1.0f);
    const float newTravel = juce::jlimit (0.0f, 1.0f, dragStartTravel + delta);

    float newDb = travelToDb (newTravel);

    // A soft detent at unity, so 0 dB is easy to find by hand. Shift bypasses it.
    if (! dragFine && std::abs (newDb) < 0.5f)
        newDb = 0.0f;

    travel = newDb == 0.0f ? dbToTravel (0.0f) : newTravel;

    if (newDb != db)
    {
        db = newDb;

        if (onChange)
            onChange (db);
    }

    repaint();
}

void LevelFader::mouseUp (const juce::MouseEvent&)
{
    if (dragging)
    {
        dragging = false;
        repaint();
    }
}

void LevelFader::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (isInFaderColumn (e.getPosition()))
    {
        dragging = false;
        userSetDb (0.0f);
    }
}

void LevelFader::mouseMove (const juce::MouseEvent& e)
{
    const bool over = getCapBounds().contains (e.getPosition());

    if (over != hoverCap)
    {
        hoverCap = over;
        repaint (getCapBounds().expanded (4));
    }
}

void LevelFader::mouseExit (const juce::MouseEvent&)
{
    if (hoverCap)
    {
        hoverCap = false;
        repaint();
    }
}

void LevelFader::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    float direction = wheel.deltaY > 0.0f ? 1.0f : (wheel.deltaY < 0.0f ? -1.0f : 0.0f);

    if (wheel.isReversed)
        direction = -direction;

    if (direction == 0.0f)
        return;

    if (wheel.isSmooth)     // trackpads send many tiny deltas: turn them into whole steps
    {
        wheelAccumulator += wheel.deltaY * (wheel.isReversed ? -1.0f : 1.0f);

        if (std::abs (wheelAccumulator) < 0.25f)
            return;

        wheelAccumulator = 0.0f;
    }

    const float step = e.mods.isShiftDown() ? 0.1f : 1.0f;
    float next;

    if (db <= kMinusInfDb)
        next = direction > 0.0f ? kMinFiniteDb : kMinusInfDb;
    else if (db + direction * step < kMinFiniteDb)
        next = kMinusInfDb;
    else
        next = db + direction * step;

    userSetDb (next);
}

bool LevelFader::keyPressed (const juce::KeyPress& key)
{
    const float step = key.getModifiers().isShiftDown() ? 0.1f : 1.0f;
    const int code = key.getKeyCode();

    if (code == juce::KeyPress::upKey || code == juce::KeyPress::rightKey)
    {
        userSetDb (db <= kMinusInfDb ? kMinFiniteDb : db + step);
        return true;
    }

    if (code == juce::KeyPress::downKey || code == juce::KeyPress::leftKey)
    {
        userSetDb (db - step < kMinFiniteDb ? kMinusInfDb : db - step);
        return true;
    }

    if (code == juce::KeyPress::homeKey)    { userSetDb (0.0f);         return true; }
    if (code == juce::KeyPress::endKey)     { userSetDb (kMinusInfDb);  return true; }

    return false;
}
}
