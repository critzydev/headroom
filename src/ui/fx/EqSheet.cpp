// EqSheet implementation: overlay, header controls and the eight band columns.
#include "ui/fx/EqSheet.h"

#include "ui/Focus.h"
#include "ui/Format.h"
#include "ui/HeadroomLookAndFeel.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/widgets/HoverFade.h"

namespace hr
{
namespace
{
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

    float plainOf (const juce::RangedAudioParameter* p)
    {
        return p->convertFrom0to1 (p->getValue());
    }

    juce::String trimmed (double value, int decimals)
    {
        auto text = juce::String (value, decimals);

        if (text.contains ("."))
            text = text.trimCharactersAtEnd ("0").trimCharactersAtEnd (".");

        return text;
    }

    // ---- Colour chip: the band's number, filled when it is on, a ring when it is off; a click switches it ------------------
    class BandChip final : public juce::Button
    {
    public:
        BandChip (int bandIndex, juce::Colour c) : juce::Button ("Band " + juce::String (bandIndex + 1)), number (bandIndex + 1), colour (c)
        {
            setMouseClickGrabsKeyboardFocus (false);
            setWantsKeyboardFocus (true);
            setClickingTogglesState (false);
            HoverFade::attach (*this);
        }

        void paintButton (juce::Graphics& g, bool highlighted, bool down) override
        {
            const auto bounds = getLocalBounds().toFloat();
            const bool on = getToggleState();
            const float hover = down ? 1.0f : HoverFade::get (*this, highlighted);
            const auto circle = juce::Rectangle<float> (20.0f, 20.0f).withCentre (bounds.getCentre());

            if (hasKeyboardFocus (false))
                HeadroomLookAndFeel::drawFocusRing (g, bounds, theme::kControlRadius);

            if (on)
            {
                g.setColour (colour.interpolatedWith (juce::Colours::white, 0.12f * hover));
                g.fillEllipse (circle);
                g.setColour (theme::bg0);
            }
            else
            {
                g.setColour (colour.withAlpha (0.45f + 0.4f * hover));
                g.drawEllipse (circle.reduced (0.75f), 1.5f);
                g.setColour (colour.withAlpha (0.6f + 0.3f * hover));
            }

            g.setFont (theme::sans (11.0f, theme::Sans::SemiBold));
            g.drawText (juce::String (number), circle, juce::Justification::centred, false);
        }

    private:
        int number;
        juce::Colour colour;
    };

    // ---- Solo: hear only what this band acts on (the same as Alt + dragging its node) -------------------------------------
    class SoloButton final : public juce::Button
    {
    public:
        SoloButton() : juce::Button ("Solo")
        {
            setMouseClickGrabsKeyboardFocus (false);
            setWantsKeyboardFocus (true);
            setClickingTogglesState (false);
            HoverFade::attach (*this);
        }

        void paintButton (juce::Graphics& g, bool highlighted, bool down) override
        {
            const auto bounds = getLocalBounds().toFloat();
            const bool soloed = getToggleState();
            const float hover = ! isEnabled() ? 0.0f : (down ? 1.0f : HoverFade::get (*this, highlighted));

            g.setColour (soloed ? theme::tint (theme::bg1, theme::accent, 0.16f) : theme::bg2.interpolatedWith (theme::bg3, hover));
            g.fillRoundedRectangle (bounds, theme::kControlRadius);

            if (hasKeyboardFocus (false))
                HeadroomLookAndFeel::drawFocusRing (g, bounds, theme::kControlRadius);
            else if (soloed)
            {
                g.setColour (theme::accent);
                g.drawRoundedRectangle (bounds.reduced (0.5f), theme::kControlRadius - 0.5f, 1.0f);
            }

            g.setColour (! isEnabled() ? theme::text2.withAlpha (0.5f) : (soloed ? theme::accent : theme::text1.interpolatedWith (theme::text0, hover)));
            g.setFont (theme::sans (11.0f, theme::Sans::SemiBold));
            g.drawText ("S", bounds, juce::Justification::centred, false);
        }
    };

    // ---- A small select box: 12 px text, so the longest choice ("High shelf", "12 dB/oct") fits the narrowest column -----------
    class EqSelect final : public juce::Component,
                           public juce::SettableTooltipClient
    {
    public:
        EqSelect()
        {
            setWantsKeyboardFocus (true);
            setMouseClickGrabsKeyboardFocus (false);
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
            HoverFade::attach (*this);
        }

        void setItems (const juce::StringArray& newItems)
        {
            if (items != newItems)
            {
                items = newItems;
                repaint();
            }
        }

        void setSelected (int index)
        {
            if (index != selected)
            {
                selected = index;
                repaint();
            }
        }

        std::function<void (int)> onChange;

        void paint (juce::Graphics& g) override
        {
            const auto bounds = getLocalBounds().toFloat();
            const float hover = isEnabled() ? HoverFade::get (*this, false) : 0.0f;

            g.setColour (theme::bg2.interpolatedWith (theme::bg3, hover));
            g.fillRoundedRectangle (bounds, theme::kControlRadius);

            if (hasKeyboardFocus (false))
            {
                HeadroomLookAndFeel::drawFocusRing (g, bounds, theme::kControlRadius);
            }
            else
            {
                g.setColour (theme::line);
                g.drawRoundedRectangle (bounds.reduced (0.5f), theme::kControlRadius - 0.5f, 1.0f);
            }

            g.setColour (isEnabled() ? theme::text0 : theme::text2);
            g.setFont (theme::sans (12.0f));
            g.drawText (juce::isPositiveAndBelow (selected, items.size()) ? items[selected] : juce::String(),
                        bounds.withTrimmedLeft (7.0f).withTrimmedRight (19.0f), juce::Justification::centredLeft, true);

            Icons::draw (g, "chevron-down", juce::Point<float> (bounds.getRight() - 11.0f, bounds.getCentreY()), 12.0f, theme::text1);
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (! e.mods.isPopupMenu() && getLocalBounds().contains (e.getPosition()))
                showMenu();
        }

        bool keyPressed (const juce::KeyPress& key) override
        {
            const int code = key.getKeyCode();

            if (code == juce::KeyPress::returnKey || code == juce::KeyPress::spaceKey || code == juce::KeyPress::downKey)
            {
                showMenu();
                return true;
            }

            return false;
        }

        void focusGained (FocusChangeType) override     { repaint(); }
        void focusLost (FocusChangeType) override       { repaint(); }

    private:
        void showMenu()
        {
            juce::PopupMenu menu;

            for (int i = 0; i < items.size(); ++i)
                menu.addItem (i + 1, items[i], true, i == selected);

            juce::Component::SafePointer<EqSelect> safe (this);

            menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMinimumWidth (getWidth()).withStandardItemHeight (28),
                                [safe] (int result)
                                {
                                    if (safe != nullptr && result > 0 && safe->onChange)
                                        safe->onChange (result - 1);
                                });
        }

        juce::StringArray items;
        int selected = -1;
    };

    // ---- A number you can drag, scroll, or double-click and type ---------------------------------------------------------
    using Kind = EqValueKind;

    class ValueField final : public juce::Component,
                             public juce::SettableTooltipClient
    {
    public:
        ValueField (juce::RangedAudioParameter& p, Kind k, juce::String labelText)
            : param (p), kind (k), label (std::move (labelText)),
              watcher (std::vector<juce::RangedAudioParameter*> { &p }, [this] { repaint(); })
        {
            setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
            HoverFade::attach (*this);
        }

        void setActive (bool shouldBeActive)
        {
            if (active != shouldBeActive)
            {
                active = shouldBeActive;
                setMouseCursor (active ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
                repaint();
            }
        }

        bool isEditing() const noexcept     { return editor != nullptr; }

        void paint (juce::Graphics& g) override
        {
            const auto bounds = getLocalBounds().toFloat();
            const float hover = active ? HoverFade::get (*this, false) : 0.0f;

            g.setColour (theme::bg2.interpolatedWith (theme::bg3, hover));
            g.fillRoundedRectangle (bounds, 4.0f);

            const bool roomForLabel = getWidth() >= 104;

            if (roomForLabel)
            {
                g.setColour (active ? theme::text2 : theme::text2.withAlpha (0.6f));
                g.setFont (theme::caps (9.0f));
                g.drawText (label.toUpperCase(), bounds.withTrimmedLeft (8.0f), juce::Justification::centredLeft, false);
            }

            if (editor != nullptr)
                return;

            g.setColour (active ? theme::text0 : theme::text2);
            g.setFont (theme::mono (12.0f));
            g.drawText (active ? valueText (! roomForLabel) : theme::glyph::emDash, bounds.withTrimmedRight (8.0f), juce::Justification::centredRight, false);
        }

        void resized() override
        {
            if (editor != nullptr)
                editor->setBounds (getLocalBounds().withTrimmedLeft (getWidth() >= 104 ? 34 : 6));
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (! active || editor != nullptr)
                return;

            dragging = true;
            lastY = e.position.y;
            param.beginChangeGesture();
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (! dragging)
                return;

            const float k = e.mods.isShiftDown() ? 0.0006f : 0.006f;
            const float dy = lastY - e.position.y;
            lastY = e.position.y;
            param.setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, param.getValue() + dy * k));
        }

        void mouseUp (const juce::MouseEvent&) override
        {
            if (dragging)
            {
                param.endChangeGesture();
                dragging = false;
            }
        }

        void mouseDoubleClick (const juce::MouseEvent&) override
        {
            if (active)
                beginEditing();
        }

        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
        {
            if (! active || wheel.deltaY == 0.0f || editor != nullptr)
                return;

            const float direction = (wheel.deltaY > 0.0f ? 1.0f : -1.0f) * (wheel.isReversed ? -1.0f : 1.0f);
            param.beginChangeGesture();
            param.setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, param.getValue() + direction * (e.mods.isShiftDown() ? 0.002f : 0.01f)));
            param.endChangeGesture();
        }

        // What double-click, typing and Return do, in one go.
        void typeValue (const juce::String& text)
        {
            beginEditing();
            finishEditing (true, text);
        }

    private:
        juce::String valueText (bool withName) const
        {
            const float v = plainOf (&param);

            switch (kind)
            {
                case Kind::frequency:   return EqGraph::formatFrequency (v);
                case Kind::gain:        return fmt::db (v, true, true);
                case Kind::q:           return (withName ? juce::String ("Q ") : juce::String()) + juce::String (v, 2);
            }

            return {};
        }

        juce::String editText() const
        {
            const float v = plainOf (&param);

            switch (kind)
            {
                case Kind::frequency:   return v < 100.0f ? trimmed (v, 1) : juce::String (juce::roundToInt (v));
                case Kind::gain:        return trimmed (v, 1);
                case Kind::q:           return trimmed (v, 2);
            }

            return {};
        }

        void beginEditing()
        {
            editor = std::make_unique<juce::TextEditor>();
            auto& ed = *editor;
            ed.setFont (theme::mono (12.0f));
            ed.setJustification (juce::Justification::centredRight);
            ed.setIndents (4, 0);
            ed.setBorder (juce::BorderSize<int> (0));
            ed.setInputRestrictions (10);
            ed.setText (editText(), false);
            ed.selectAll();
            addAndMakeVisible (ed);
            resized();

            juce::Component::SafePointer<ValueField> safe (this);

            ed.onReturnKey = [safe]
            {
                if (safe != nullptr && safe->editor != nullptr)
                    juce::MessageManager::callAsync ([safe, text = safe->editor->getText()] { if (safe != nullptr) safe->finishEditing (true, text); });
            };
            ed.onEscapeKey = [safe]
            {
                juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->finishEditing (false, {}); });
            };
            ed.onFocusLost = [safe]
            {
                if (safe != nullptr && safe->editor != nullptr)
                    juce::MessageManager::callAsync ([safe, text = safe->editor->getText()] { if (safe != nullptr) safe->finishEditing (true, text); });
            };

            focusIfShowing (ed);
            repaint();
        }

        void finishEditing (bool commit, const juce::String& text)
        {
            if (editor == nullptr)
                return;

            float plain = 0.0f;

            if (commit && parseEqValue (kind, text, plain))
            {
                const auto range = param.getNormalisableRange();
                setComplete (&param, juce::jlimit (range.start, range.end, plain));
            }

            std::unique_ptr<juce::TextEditor> dying (std::move (editor));
            removeChildComponent (dying.get());
            repaint();
        }

        juce::RangedAudioParameter& param;
        const Kind kind;
        const juce::String label;
        ParamWatcher watcher;
        std::unique_ptr<juce::TextEditor> editor;
        bool active = true, dragging = false;
        float lastY = 0.0f;
    };
}

bool parseEqValue (EqValueKind kind, const juce::String& input, float& value)
{
    auto text = input.trim().replace (theme::glyph::minus, "-").replace (",", ".").toLowerCase();
    text = text.replace ("khz", "k").replace ("hz", "").replace ("db", "").replace ("q", "").trim();

    if (text.isEmpty() || ! text.containsAnyOf ("0123456789"))
        return false;

    const bool kilo = kind == EqValueKind::frequency && text.endsWithChar ('k');
    value = (float) (text.trimCharactersAtEnd ("k ").getDoubleValue() * (kilo ? 1000.0 : 1.0));
    return true;
}

// ---------------------------------------------------------------------------------------------------
// One band's controls
// ---------------------------------------------------------------------------------------------------
class EqBandColumn final : public juce::Component
{
public:
    EqBandColumn (Equalizer& equalizer, int bandIndex, std::function<void (int)> onSelect)
        : eq (equalizer), band (bandIndex), select (std::move (onSelect)), colour (theme::channelTags[(size_t) bandIndex]),
          chip (bandIndex, colour),
          freqField (*eq.apvts.getParameter (Equalizer::bandParamId (bandIndex, "Freq")), EqValueKind::frequency, "Freq"),
          gainField (*eq.apvts.getParameter (Equalizer::bandParamId (bandIndex, "Gain")), EqValueKind::gain, "Gain"),
          qField (*eq.apvts.getParameter (Equalizer::bandParamId (bandIndex, "Q")), EqValueKind::q, "Q")
    {
        auto find = [this] (const char* what) { return eq.apvts.getParameter (Equalizer::bandParamId (band, what)); };
        onParam = find ("On");
        typeParam = find ("Type");
        slopeParam = find ("Slope");

        chip.setTooltip ("Switch this band on or off");
        chip.onClick = [this] { setComplete (onParam, chip.getToggleState() ? 0.0f : 1.0f); };
        addAndMakeVisible (chip);

        soloButton.setTooltip ("Hear only what this band acts on (or hold Alt while dragging its point)");
        soloButton.onClick = [this] { eq.setSoloBand (eq.getSoloBand() == band ? -1 : band); refreshSolo(); };
        addAndMakeVisible (soloButton);

        juce::StringArray types;

        for (int t = 0; t < Equalizer::kNumTypes; ++t)
            types.add (Equalizer::typeName ((Equalizer::Type) t));

        typeSelect.setItems (types);
        typeSelect.setTooltip ("What kind of filter this band is");
        typeSelect.onChange = [this] (int index)
        {
            const bool wasOn = eq.getBandSettings (band).on;
            setComplete (typeParam, (float) index);

            if (! wasOn)
                setComplete (onParam, 1.0f);
        };
        addAndMakeVisible (typeSelect);

        slopeSelect.setTooltip ("How steeply this filter cuts, in dB per octave");
        slopeSelect.onChange = [this] (int index) { setComplete (slopeParam, (float) index); };
        addChildComponent (slopeSelect);

        freqField.setTooltip ("Frequency. Drag, scroll, or double-click to type.");
        gainField.setTooltip ("Gain. Drag, scroll, or double-click to type.");
        qField.setTooltip ("Width (Q). Higher is narrower. Drag, scroll, or double-click to type.");
        addAndMakeVisible (freqField);
        addAndMakeVisible (gainField);
        addAndMakeVisible (qField);

        watcher = std::make_unique<ParamWatcher> (std::vector<juce::RangedAudioParameter*> { onParam, typeParam, slopeParam }, [this] { refresh(); });
        refresh();
    }

    void setSelected (bool shouldBeSelected)
    {
        if (selected != shouldBeSelected)
        {
            selected = shouldBeSelected;
            repaint();
        }
    }

    void refreshSolo()
    {
        const bool soloed = eq.getSoloBand() == band;

        if (soloButton.getToggleState() != soloed)
            soloButton.setToggleState (soloed, juce::dontSendNotification);
    }

    void refresh()
    {
        const auto s = eq.getBandSettings (band);
        const bool cut = Equalizer::isCut (s.type);

        chip.setToggleState (s.on, juce::dontSendNotification);
        soloButton.setEnabled (s.on);
        typeSelect.setSelected ((int) s.type);

        // The slope choices; the Linkwitz-Riley one only shows up for a band that came from an old state.
        juce::StringArray slopes;

        for (int i = 0; i < (s.slope == dsp::eq::kSlopeLinkwitzRiley24 ? Equalizer::kNumSlopes : 4); ++i)
            slopes.add (Equalizer::slopeName (i));

        slopeSelect.setItems (slopes);
        slopeSelect.setSelected (s.slope);
        slopeSelect.setVisible (cut);
        gainField.setVisible (! cut);

        gainField.setActive (Equalizer::hasGain (s.type));
        qField.setActive (! (cut && s.slope == 0));

        const float alpha = s.on ? 1.0f : 0.55f;
        freqField.setAlpha (alpha);
        gainField.setAlpha (alpha);
        qField.setAlpha (alpha);
        slopeSelect.setAlpha (alpha);
        typeSelect.setAlpha (s.on ? 1.0f : 0.8f);

        refreshSolo();
        repaint();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (getWidth() >= 104 ? 8 : 6, 0).withTrimmedTop (10);

        auto top = area.removeFromTop (24);
        chip.setBounds (top.removeFromLeft (24));
        soloButton.setBounds (top.removeFromRight (24));

        area.removeFromTop (8);
        typeSelect.setBounds (area.removeFromTop (26));
        area.removeFromTop (8);
        freqField.setBounds (area.removeFromTop (24));
        area.removeFromTop (4);

        const auto row = area.removeFromTop (24);
        gainField.setBounds (row);
        slopeSelect.setBounds (row);
        area.removeFromTop (4);
        qField.setBounds (area.removeFromTop (24));
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced (2.0f, 0.0f);

        if (selected)
        {
            g.setColour (theme::bg2.withAlpha (0.55f));
            g.fillRoundedRectangle (bounds, theme::kControlRadius);
        }

        // The band's colour tag along the top, like the channel strips.
        const bool on = eq.getBandSettings (band).on;
        g.setColour (colour.withAlpha (on ? 1.0f : 0.3f));
        g.fillRoundedRectangle (bounds.getX() + 4.0f, bounds.getY(), bounds.getWidth() - 8.0f, 2.0f, 1.0f);
    }

    void mouseDown (const juce::MouseEvent&) override   { select (band); }

    void typeValue (EqValueKind kind, const juce::String& text)
    {
        (kind == EqValueKind::frequency ? freqField : (kind == EqValueKind::gain ? gainField : qField)).typeValue (text);
    }

private:
    Equalizer& eq;
    const int band;
    std::function<void (int)> select;
    const juce::Colour colour;

    juce::RangedAudioParameter *onParam = nullptr, *typeParam = nullptr, *slopeParam = nullptr;
    BandChip chip;
    SoloButton soloButton;
    EqSelect typeSelect, slopeSelect;
    ValueField freqField, gainField, qField;
    std::unique_ptr<ParamWatcher> watcher;
    bool selected = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqBandColumn)
};

// ---------------------------------------------------------------------------------------------------
// The sheet
// ---------------------------------------------------------------------------------------------------

EqSheet::EqSheet (Equalizer& equalizer, EqAnalyzerFeed& analyzerFeed, int initialBand, std::function<void()> onCloseRequested)
    : eq (equalizer), feed (analyzerFeed), onClose (std::move (onCloseRequested)), graph (equalizer, analyzerFeed, EqGraph::Mode::expanded)
{
    setWantsKeyboardFocus (true);
    setInterceptsMouseClicks (true, true);
    setFocusContainerType (FocusContainerType::keyboardFocusContainer);     // Tab stays inside the editor while it is open
    setTitle ("EQ");

    closeButton.setTooltip ("Close (Esc)");
    closeButton.onClick = [this] { if (onClose) onClose(); };
    addAndMakeVisible (closeButton);

    sourceControl.setLabels ({ "Post", "Pre" });
    sourceControl.onChange = [this] (int index)
    {
        graph.setSpectrumSource (index == 0 ? EqAnalyzerFeed::Source::post : EqAnalyzerFeed::Source::pre);
    };
    addAndMakeVisible (sourceControl);

    const juce::String plusMinus = juce::String::charToString ((juce::juce_wchar) 0x00b1);
    rangeControl.setLabels ({ plusMinus + "6", plusMinus + "12", plusMinus + "18", plusMinus + "24" });
    const float ranges[] { 6.0f, 12.0f, 18.0f, 24.0f };
    rangeControl.onChange = [this, ranges] (int index) { graph.setRangeDb (ranges[juce::jlimit (0, 3, index)]); };
    addAndMakeVisible (rangeControl);

    graph.setRangeDb (EqGraph::getSharedRangeDb());
    rangeControl.setSelected (juce::jlimit (0, 3, juce::roundToInt (graph.getRangeDb() / 6.0f) - 1));
    addAndMakeVisible (graph);

    for (int b = 0; b < EqGraph::kBands; ++b)
    {
        columns[(size_t) b] = std::make_unique<EqBandColumn> (equalizer, b, [this] (int band) { graph.setSelectedBand (band); });
        addAndMakeVisible (*columns[(size_t) b]);
    }

    graph.onBandSelected = [this] (int band)
    {
        for (int b = 0; b < EqGraph::kBands; ++b)
            columns[(size_t) b]->setSelected (b == band);
    };

    graph.setSelectedBand (initialBand);
    columns[(size_t) graph.getSelectedBand()]->setSelected (true);
    startTimerHz (10);
}

EqSheet::~EqSheet()
{
    stopTimer();

    if (eq.getSoloBand() >= 0)
        eq.setSoloBand (-1);            // a solo started here does not outlive the sheet
}

bool EqSheet::enterValue (int band, EqValueKind kind, const juce::String& text)
{
    if (! juce::isPositiveAndBelow (band, EqGraph::kBands))
        return false;

    columns[(size_t) band]->typeValue (kind, text);
    return true;
}

void EqSheet::timerCallback()
{
    if (! isShowing())
        return;

    const int solo = eq.getSoloBand();

    if (solo != lastSolo)
    {
        lastSolo = solo;

        for (auto& column : columns)
            column->refreshSolo();
    }
}

void EqSheet::parentSizeChanged()
{
    if (auto* parent = getParentComponent())
        setBounds (parent->getLocalBounds());
}

void EqSheet::resized()
{
    // Roughly 900 x 480 in a small window, growing with it (nearly the whole window when that is small).
    const int w = juce::jlimit (720, 1320, getWidth() - (getWidth() < 1000 ? 32 : 96));
    const int h = juce::jlimit (460, 820, getHeight() - (getHeight() < 640 ? 32 : 112));
    panel = juce::Rectangle<int> (w, h).withCentre (getLocalBounds().getCentre()).constrainedWithin (getLocalBounds().reduced (16));

    auto inner = panel.reduced (kPadding, 0);
    headerArea = inner.removeFromTop (kHeaderHeight);
    stripArea = inner.removeFromBottom (kStripHeight + kPadding).withTrimmedBottom (kPadding);

    // Header: close on the right, then the range and the spectrum choice.
    auto header = headerArea.withTrimmedTop (8);
    closeButton.setBounds (header.removeFromRight (28).withSizeKeepingCentre (28, 28));
    header.removeFromRight (12);
    rangeControl.setBounds (header.removeFromRight (rangeControl.getIdealWidth() + 8).withSizeKeepingCentre (rangeControl.getIdealWidth() + 8, 28));
    header.removeFromRight (56);        // room for its label
    sourceControl.setBounds (header.removeFromRight (sourceControl.getIdealWidth() + 8).withSizeKeepingCentre (sourceControl.getIdealWidth() + 8, 28));

    graph.setBounds (inner.withTrimmedTop (4).withTrimmedBottom (12));

    const int columnWidth = stripArea.getWidth() / EqGraph::kBands;

    for (int b = 0; b < EqGraph::kBands; ++b)
        columns[(size_t) b]->setBounds (stripArea.getX() + b * columnWidth, stripArea.getY(), columnWidth, stripArea.getHeight());
}

void EqSheet::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black.withAlpha (0.62f));

    g.setColour (theme::bg1);
    g.fillRoundedRectangle (panel.toFloat(), theme::kPanelRadius);
    g.setColour (theme::lineStrong);
    g.drawRoundedRectangle (panel.toFloat().reduced (0.5f), theme::kPanelRadius - 0.5f, 1.0f);

    g.setColour (theme::text0);
    g.setFont (theme::sans (15.0f, theme::Sans::SemiBold));
    g.drawText ("EQ", headerArea.withTrimmedTop (8).withHeight (28).withWidth (40), juce::Justification::centredLeft, false);

    // The hint, as much of it as fits.
    {
        const auto font = theme::sans (12.0f);
        const int room = sourceControl.getX() - headerArea.getX() - 130;
        const juce::String hints[] { "Drag a point to shape the sound. Double-click empty space to add a band.", "Drag a point to shape the sound.", {} };

        for (const auto& hint : hints)
        {
            if (hint.isEmpty() || theme::textWidth (font, hint) <= (float) room)
            {
                g.setColour (theme::text2);
                g.setFont (font);
                g.drawText (hint, juce::Rectangle<int> (headerArea.getX() + 40, headerArea.getY() + 8, room, 28), juce::Justification::centredLeft, false);
                break;
            }
        }
    }

    // Labels for the two selectors.
    g.setColour (theme::text2);
    g.setFont (theme::caps (10.0f));
    g.drawText ("SPECTRUM", juce::Rectangle<int> (sourceControl.getX() - 76, sourceControl.getY(), 70, sourceControl.getHeight()), juce::Justification::centredRight, false);
    g.drawText ("RANGE", juce::Rectangle<int> (rangeControl.getX() - 52, rangeControl.getY(), 46, rangeControl.getHeight()), juce::Justification::centredRight, false);

    g.setColour (theme::line);
    g.fillRect (panel.getX() + 1, headerArea.getBottom(), panel.getWidth() - 2, 1);
    g.fillRect (panel.getX() + 1, stripArea.getY() - 8, panel.getWidth() - 2, 1);
}

void EqSheet::mouseDown (const juce::MouseEvent& e)
{
    if (! panel.contains (e.getPosition()) && onClose)
        onClose();
}

bool EqSheet::keyPressed (const juce::KeyPress& key)
{
    if (key.getKeyCode() == juce::KeyPress::escapeKey)
    {
        if (onClose)
            onClose();

        return true;
    }

    return key.getKeyCode() != juce::KeyPress::tabKey;      // modal: shortcuts do not reach the mixer underneath, Tab moves between the controls
}
}
