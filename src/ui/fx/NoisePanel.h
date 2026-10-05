// NoisePanel: the Noise Removal effect in the FX drawer: mode, live spectrum with the removed part, strength, room check.
#pragma once

#include "Juce.h"
#include "engine/builtin/NoiseRemoval.h"
#include "engine/denoise/SpectrumTap.h"
#include "ui/fx/BuiltinPanel.h"
#include "ui/widgets/Buttons.h"

#include <array>
#include <functional>
#include <memory>
#include <vector>

namespace hr
{
// The maths behind the spectrum, separate from any drawing so it can be tested and timed. Reads the audio thread's tap
// (4096 samples of input and output), does two FFTs with preallocated buffers, and turns each into one dB value per pixel
// column on a log frequency axis (50 Hz - 20 kHz), each averaged over 1/6 octave and smoothed over time.
class SpectrumAnalyzer
{
public:
    static constexpr int kOrder = 12;
    static constexpr int kSize = 1 << kOrder;
    static constexpr float kMinHz = 50.0f, kMaxHz = 20000.0f;
    static constexpr float kRangeDb = 66.0f;                    // what the plot shows below its ceiling

    SpectrumAnalyzer();

    // One value per pixel column of the plot; the sample rate of the audio the tap holds.
    void setLayout (int numColumns, double sampleRate);
    void reset() noexcept;

    // UI thread. False (and nothing changes) until the tap holds enough audio.
    bool update (const denoise::SpectrumTap& tap, double deltaSeconds);

    int getNumColumns() const noexcept                          { return (int) inputDb.size(); }
    const std::vector<float>& getInputDb() const noexcept       { return inputDb; }
    const std::vector<float>& getOutputDb() const noexcept      { return outputDb; }
    float getCeilingDb() const noexcept                         { return ceilingDb; }

    // Frequency at a column's centre (for drawing and tests).
    float frequencyForColumn (int column) const noexcept;

private:
    void analyse (const float* samples, std::vector<float>& targetDb) noexcept;

    juce::dsp::FFT fft { kOrder };
    std::array<float, kSize> window {}, inputSamples {}, outputSamples {};
    std::array<float, 2 * kSize> work {};
    std::array<double, kSize / 2 + 2> prefix {};
    std::vector<int> firstBin, lastBin;
    std::vector<float> binPosition;                 // a column's centre in (fractional) bins: narrow bands interpolate there
    std::vector<float> inputTarget, outputTarget, inputDb, outputDb;
    float ceilingDb = -20.0f;
    double sampleRate = 48000.0;
    bool havePrevious = false;
};

// The plot itself: bg2 panel, three frequency labels, the input as a thin outline, the cleaned output as a soft fill and
// the part in between (what was taken out) shaded amber. Draws cached paths only, so repainting costs well under 1 ms.
class NoiseSpectrumView final : public juce::Component
{
public:
    static constexpr int kHeight = 124;

    NoiseSpectrumView();

    // Number of dB values the analyzer must deliver (plot width in pixels).
    int getNumColumns() const noexcept                          { return juce::jmax (1, (int) plot.getWidth()); }

    void setData (const SpectrumAnalyzer& analyzer);
    void setDimmed (bool shouldDim)                             { if (dimmed != shouldDim) { dimmed = shouldDim; repaint(); } }
    void setActive (bool hasSignal)                             { if (live != hasSignal) { live = hasSignal; repaint(); } }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    float yForDb (float db) const noexcept;
    void rebuildPaths();

    juce::Rectangle<float> plot;
    std::vector<float> input, output;
    float ceiling = -20.0f;
    juce::Path inputLine, outputFill, outputLine, removed;
    bool dimmed = false, live = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NoiseSpectrumView)
};

// A button that is on for as long as the mouse button is down, wherever the pointer goes meanwhile.
class HoldButton final : public juce::Component,
                         public juce::SettableTooltipClient
{
public:
    explicit HoldButton (const juce::String& labelText);

    bool isHeld() const noexcept                                { return held; }
    void setHeld (bool shouldHold, bool notify);

    std::function<void (bool)> onHold;

    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override          { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override           { hover = false; repaint(); }
    void mouseDown (const juce::MouseEvent&) override           { setHeld (true, true); }
    void mouseUp (const juce::MouseEvent&) override             { setHeld (false, true); }
    void visibilityChanged() override                           { if (! isVisible()) setHeld (false, true); }

private:
    juce::String label;
    bool held = false, hover = false;
};

class NoisePanel final : public BuiltinPanel
{
public:
    explicit NoisePanel (NoiseRemoval& processor);
    ~NoisePanel() override;

    int getPreferredHeight() const override                     { return preferredHeight; }
    void setDimmed (bool shouldDim) override;
    void tickMeter (double deltaSeconds) override;

    void resized() override;
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;

    // Tests and screenshots.
    NoiseSpectrumView& getSpectrumView() noexcept               { return spectrum; }
    HoldButton& getHearButton() noexcept                        { return hearButton; }
    SegmentedControl& getModeControl() noexcept                 { return modeControl; }
    bool isAdvancedOpen() const noexcept                        { return advancedOpen; }
    void setAdvancedOpen (bool open);
    void refresh();                                            // the parameters changed (also what the parameter watcher does, asynchronously)

    enum class Verdict { unknown, great, good, noisy };
    Verdict getVerdict() const noexcept                         { return verdict; }

private:
    void layoutAll();
    void syncMode();
    bool isOnScreen() const;
    void updateReadouts (double deltaSeconds);
    juce::String captionText (juce::Colour& colour) const;

    NoiseRemoval& noise;
    SegmentedControl modeControl;
    NoiseSpectrumView spectrum;
    SpectrumAnalyzer analyzer;
    juce::Slider strength, postFilter, threshold, hold;
    std::unique_ptr<juce::SliderParameterAttachment> strengthAttachment, postFilterAttachment, thresholdAttachment, holdAttachment;
    HoldButton hearButton;
    std::unique_ptr<ParamWatcher> watcher;

    // Layout
    juce::Rectangle<int> speakingArea, captionArea, strengthLabelArea, reductionArea, roomArea, tipArea, hintArea, advancedArea,
                         postFilterLabelArea, thresholdLabelArea, holdLabelArea, readoutArea, dividerArea;
    int preferredHeight = 300;
    bool advancedOpen = false, dimmed = false;

    // What is shown
    float shownReduction = 0.0f;
    float shownSnr = 0.0f;
    bool haveSnr = false, speaking = false;
    Verdict verdict = Verdict::unknown;
    juce::String captionCache, reductionText, roomText, readoutText, capText;
    juce::Colour captionColour;
    double lastSampleRate = 0.0;
    double sinceRoomUpdate = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NoisePanel)
};
}
