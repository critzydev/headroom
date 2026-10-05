// Knob panels for compressor, gate, de-esser and limiter, plus the factory (which also builds the EQ and Noise Removal panels).
#include "ui/fx/BuiltinPanel.h"

#include "engine/builtin/Compressor.h"
#include "engine/builtin/Equalizer.h"
#include "engine/builtin/NoiseRemoval.h"
#include "ui/Format.h"
#include "ui/Theme.h"
#include "ui/fx/CompPanel.h"
#include "ui/fx/EqPanel.h"
#include "ui/fx/NoisePanel.h"
#include "ui/widgets/Knob.h"
#include "ui/widgets/Meter.h"

namespace hr
{
namespace
{
    struct KnobSpec
    {
        KnobSpec (const char* id, const char* text, const char* tooltip, bool isSigned = false)
            : paramId (id), label (text), tip (tooltip), signedDb (isSigned) {}

        const char* paramId;
        const char* label;
        const char* tip;
        bool signedDb;
    };

    struct MeterSpec
    {
        MeterSpec (const char* text, float range, bool isVoice = false) : label (text), rangeDb (range), voice (isVoice) {}

        const char* label;
        float rangeDb;          // full scale of the bar in dB of reduction
        bool voice;             // 0..1 probability instead of gain reduction
    };

    // A grid of knobs (4 across) with one horizontal meter underneath.
    class KnobPanel final : public BuiltinPanel
    {
    public:
        // `note`, if given, is polled with the meter: whatever it returns (when not empty) replaces the meter's value text.
        KnobPanel (BuiltinProcessor& p, const std::vector<KnobSpec>& specs, std::optional<MeterSpec> meterSpec,
                   std::function<juce::String()> noteProvider = {})
            : processor (p), meterInfo (meterSpec), note (std::move (noteProvider))
        {
            for (const auto& spec : specs)
            {
                if (auto* param = processor.apvts.getParameter (spec.paramId))
                {
                    auto knob = std::make_unique<ParamKnob> (*param, spec.label, spec.signedDb);
                    knob->setTooltip (spec.tip);
                    addAndMakeVisible (*knob);
                    knobs.push_back (std::move (knob));
                }
            }

            setSize (kWidth, getPreferredHeight());
        }

        int getPreferredHeight() const override
        {
            const int rows = ((int) knobs.size() + 3) / 4;
            return 4 + rows * ParamKnob::kHeight + (meterInfo ? 8 + 14 + 6 + 4 : 0) + 8;
        }

        void setDimmed (bool shouldDim) override
        {
            for (auto& knob : knobs)
                knob->setDimmed (shouldDim);

            dimmed = shouldDim;
            repaint();
        }

        void tickMeter (double dt) override
        {
            if (! meterInfo)
                return;

            if (note)
            {
                const auto text = note();

                if (text != noteText)
                {
                    noteText = text;
                    repaint (meterRow);
                }
            }

            const float raw = processor.getMeterValue();
            const float target = meterInfo->voice ? juce::jlimit (0.0f, 1.0f, raw) * 100.0f : juce::jlimit (0.0f, meterInfo->rangeDb, -raw);

            if (meterInfo->voice)
                shown = target > shown ? target : juce::jmax (target, shown - (float) (dt * 250.0));     // quick attack, gentle fall
            else
                shown = juce::jmax (target, shown - (float) (dt * MeterBar::kDecayDbPerSecond));

            const int px = juce::roundToInt (shown * 10.0f);

            if (px != lastPx)
            {
                lastPx = px;
                repaint (meterRow);
            }
        }

        void resized() override
        {
            int i = 0;

            for (auto& knob : knobs)
            {
                knob->setTopLeftPosition ((i % 4) * ParamKnob::kWidth, 4 + (i / 4) * ParamKnob::kHeight);
                ++i;
            }

            const int rows = ((int) knobs.size() + 3) / 4;
            meterRow = juce::Rectangle<int> (0, 4 + rows * ParamKnob::kHeight + 8, kWidth, 20);
        }

        void paint (juce::Graphics& g) override
        {
            if (! meterInfo)
                return;

            const float alpha = dimmed ? 0.5f : 1.0f;
            auto row = meterRow;
            auto text = row.removeFromTop (14);

            g.setFont (theme::sans (11.0f, theme::Sans::Medium));
            g.setColour (theme::text2.withAlpha (alpha));
            g.drawText (meterInfo->label, text, juce::Justification::centredLeft, false);

            if (noteText.isNotEmpty())
            {
                g.setFont (theme::sans (11.0f));
                g.setColour (theme::text2);
                g.drawText (noteText, text, juce::Justification::centredRight, false);
            }
            else
            {
                g.setFont (theme::mono (11.0f));
                g.setColour (theme::text1.withAlpha (alpha));
                g.drawText (meterInfo->voice ? juce::String (juce::roundToInt (shown)) + "%" : fmt::db (-shown),
                            text, juce::Justification::centredRight, false);
            }

            const auto bar = juce::Rectangle<float> ((float) row.getX(), (float) row.getBottom() - 6.0f, (float) row.getWidth(), 6.0f);
            g.setColour (theme::bg2);
            g.fillRoundedRectangle (bar, theme::kMeterRadius);

            const float full = meterInfo->voice ? 100.0f : meterInfo->rangeDb;
            const float fraction = juce::jlimit (0.0f, 1.0f, shown / full);

            if (fraction > 0.004f)
            {
                g.setColour ((meterInfo->voice ? theme::ok : theme::accent).withAlpha (alpha));
                g.fillRoundedRectangle (bar.withWidth (juce::jmax (theme::kMeterRadius * 2.0f, bar.getWidth() * fraction)), theme::kMeterRadius);
            }
        }

    private:
        BuiltinProcessor& processor;
        std::optional<MeterSpec> meterInfo;
        std::function<juce::String()> note;
        juce::String noteText;
        std::vector<std::unique_ptr<ParamKnob>> knobs;
        juce::Rectangle<int> meterRow;
        float shown = 0.0f;
        int lastPx = -1;
        bool dimmed = false;
    };
}

std::unique_ptr<BuiltinPanel> createBuiltinPanel (BuiltinProcessor& processor)
{
    const auto& id = processor.getBuiltinId();

    if (id == "noise")
        if (auto* noise = dynamic_cast<NoiseRemoval*> (&processor))
            return std::make_unique<NoisePanel> (*noise);

    if (id == "comp")
        if (auto* comp = dynamic_cast<Compressor*> (&processor))
            return std::make_unique<CompPanel> (*comp);

    if (id == "gate")
        return std::make_unique<KnobPanel> (processor, std::vector<KnobSpec> {
            { "threshold", "Threshold", "Sound quieter than this counts as silence." },
            { "range",     "Range",     "How far the sound is turned down while the gate is closed." },
            { "attack",    "Attack",    "How quickly the gate opens." },
            { "hold",      "Hold",      "How long the gate stays open after the sound stops." },
            { "release",   "Release",   "How quickly the gate closes." } },
            MeterSpec { "Turned down by", 40.0f });

    if (id == "deess")
        return std::make_unique<KnobPanel> (processor, std::vector<KnobSpec> {
            { "frequency", "Frequency", "Where the harsh S sounds sit in your voice." },
            { "threshold", "Threshold", "S sounds louder than this are softened." },
            { "amount",    "Amount",    "How much the S sounds are softened." } },
            MeterSpec { "Turned down by", 12.0f });

    if (id == "limit")
        return std::make_unique<KnobPanel> (processor, std::vector<KnobSpec> {
            { "ceiling", "Ceiling", "The loudest the sound is allowed to get." },
            { "release", "Release", "How quickly the limiter lets go." } },
            MeterSpec { "Turned down by", 12.0f });

    if (id == "eq")
        if (auto* eq = dynamic_cast<Equalizer*> (&processor))
            return std::make_unique<EqPanel> (*eq);

    return nullptr;
}
}
