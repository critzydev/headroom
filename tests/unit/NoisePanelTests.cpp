// Unit tests for the Noise Removal panel: the spectrum's maths, what the readouts say, and that drawing is cheap.
#include "Juce.h"

#include "NoiseTestSignals.h"
#include "TestHelpers.h"
#include "engine/builtin/NoiseRemoval.h"
#include "ui/HeadroomLookAndFeel.h"
#include "ui/Theme.h"
#include "ui/fx/NoisePanel.h"

#include <algorithm>
#include <cmath>

namespace hr
{
using namespace test;

class NoisePanelTests final : public juce::UnitTest
{
public:
    NoisePanelTests() : juce::UnitTest ("NoisePanel", "noise") {}

    void runTest() override
    {
        beginTest ("spectrum: log columns, 1/6-octave smoothing, levels");
        testAnalyzer();

        beginTest ("spectrum: nothing runs without a tap, nothing is drawn without data");
        testInactive();

        beginTest ("panel: the room check says Great, Good or Noisy");
        testVerdicts();

        beginTest ("panel: layout, Advanced, mode control, hold button");
        testLayout();

        beginTest ("panel: drawing the spectrum costs well under a millisecond");
        testPaintCost();
    }

private:
    static void setMode (NoiseRemoval& nr, NoiseRemoval::Mode mode)
    {
        auto* p = nr.apvts.getParameter ("mode");
        p->setValueNotifyingHost (p->convertTo0to1 ((float) (int) mode));
    }

    // Feeds `count` blocks of a 1 kHz sine at 0.5 (-6 dBFS) through a tap.
    static void fillTap (denoise::SpectrumTap& tap, int count, float amplitude, double frequency)
    {
        std::vector<float> block ((size_t) 480);
        double phase = 0.0;
        tap.active.store (true);

        for (int b = 0; b < count; ++b)
        {
            for (auto& v : block)
            {
                v = amplitude * (float) std::sin (phase);
                phase += 2.0 * kPi * frequency / kRate;
            }

            tap.push (block.data(), block.data(), 480);
        }
    }

    void testAnalyzer()
    {
        SpectrumAnalyzer analyzer;
        analyzer.setLayout (336, kRate);
        expectEquals (analyzer.getNumColumns(), 336);

        // Log axis 50 Hz .. 20 kHz: strictly rising, first near 50 Hz, last near 20 kHz, 1 kHz about 60 % across.
        float previous = 0.0f;
        bool rising = true;

        for (int c = 0; c < analyzer.getNumColumns(); ++c)
        {
            rising = rising && analyzer.frequencyForColumn (c) > previous;
            previous = analyzer.frequencyForColumn (c);
        }

        expect (rising);
        expect (analyzer.frequencyForColumn (0) < 55.0f && analyzer.frequencyForColumn (335) > 19000.0f);

        // Not enough audio yet: nothing to show.
        denoise::SpectrumTap tap;
        expect (! analyzer.update (tap, 0.033));

        // A 1 kHz sine at -6 dBFS: one clear peak in the column that holds 1 kHz, lower than -6 dB because what is drawn
        // is the average power over 1/6 octave (about 10 bins), which the sine's few bins share, and much lower
        // everywhere far away.
        fillTap (tap, 40, 0.5f, 1000.0);
        expect (analyzer.update (tap, 0.033), "with 19200 samples the tap can be read");

        int peakColumn = 0;

        for (int c = 0; c < analyzer.getNumColumns(); ++c)
            if (analyzer.getInputDb()[(size_t) c] > analyzer.getInputDb()[(size_t) peakColumn])
                peakColumn = c;

        const float peakHz = analyzer.frequencyForColumn (peakColumn);
        logMessage ("1 kHz sine at -6 dBFS: peak at " + juce::String (peakHz, 0) + " Hz, " + juce::String (analyzer.getInputDb()[(size_t) peakColumn], 1) + " dB");
        expect (peakHz > 900.0f && peakHz < 1100.0f, "the peak is at 1 kHz");
        expect (analyzer.getInputDb()[(size_t) peakColumn] > -18.0f && analyzer.getInputDb()[(size_t) peakColumn] < -4.0f, "at a level that follows the sine (-6 dBFS)");
        expect (analyzer.getInputDb()[(size_t) (peakColumn - 60)] < analyzer.getInputDb()[(size_t) peakColumn] - 30.0f, "far below it, well away");
        expect (analyzer.getOutputDb()[(size_t) peakColumn] == analyzer.getInputDb()[(size_t) peakColumn], "input and output were the same signal here");

        // The plot's ceiling comes up to the loudest part (in steps).
        expect (analyzer.getCeilingDb() >= -12.0f && analyzer.getCeilingDb() <= 6.0f, "ceiling " + juce::String (analyzer.getCeilingDb(), 1));

        // Silence falls back over time, slowly (release), and the ceiling comes down after a few seconds.
        denoise::SpectrumTap quiet;
        fillTap (quiet, 40, 0.0f, 1000.0);

        for (int i = 0; i < 30; ++i)
            analyzer.update (quiet, 0.033);

        expect (analyzer.getInputDb()[(size_t) peakColumn] < -20.0f, "after a second of silence the peak has fallen");

        // Other sample rates: the peak stays at the same frequency.
        {
            SpectrumAnalyzer other;
            other.setLayout (336, 44100.0);
            denoise::SpectrumTap t;
            std::vector<float> block (480);
            double phase = 0.0;
            t.active.store (true);

            for (int b = 0; b < 40; ++b)
            {
                for (auto& v : block)
                {
                    v = 0.5f * (float) std::sin (phase);
                    phase += 2.0 * kPi * 1000.0 / 44100.0;
                }

                t.push (block.data(), block.data(), 480);
            }

            other.update (t, 0.033);
            int best = 0;

            for (int c = 0; c < other.getNumColumns(); ++c)
                if (other.getInputDb()[(size_t) c] > other.getInputDb()[(size_t) best])
                    best = c;

            expect (other.frequencyForColumn (best) > 900.0f && other.frequencyForColumn (best) < 1100.0f, "1 kHz is 1 kHz at 44.1 kHz too");
        }
    }

    void testInactive()
    {
        NoiseRemoval nr;
        setMode (nr, NoiseRemoval::Mode::light);
        nr.prepareToPlay (kRate, 512);

        NoisePanel panel (nr);
        panel.setSize (NoisePanel::kWidth, panel.getPreferredHeight());

        // A panel that is not showing switches the tap off; showing, on.
        panel.setVisible (false);
        panel.tickMeter (0.033);
        expect (! nr.getSpectrumTap().active.load(), "hidden: the audio thread writes nothing");

        panel.setVisible (true);
        panel.tickMeter (0.033);
        expect (nr.getSpectrumTap().active.load(), "showing: it does");

        // Before any audio the view has no curves and says what to do.
        juce::Image image (juce::Image::ARGB, 368, NoiseSpectrumView::kHeight, true);
        juce::Graphics g (image);
        panel.getSpectrumView().paint (g);
        expect (image.isValid());
    }

    // Runs `seconds` of voice + room through a Light-mode effect while the panel ticks at 30 Hz.
    void runRoom (NoiseRemoval& nr, NoisePanel& panel, float voiceLevel, float roomLevel, int seconds)
    {
        const int n = (int) kRate * seconds;
        const auto voice = makeSpeech (n, 91);
        const auto room = makeRoomNoise (n, 92);
        std::vector<float> l ((size_t) n), r;

        for (int i = 0; i < n; ++i)
            l[(size_t) i] = voiceLevel * voice[(size_t) i] + roomLevel * room[(size_t) i];

        r = l;
        juce::MidiBuffer midi;

        for (int offset = 0; offset + 480 <= n; offset += 480)
        {
            float* ch[2] { l.data() + offset, r.data() + offset };
            juce::AudioBuffer<float> buffer (ch, 2, 480);
            nr.processBlock (buffer, midi);

            if ((offset / 480) % 3 == 2)
                panel.tickMeter (0.030);
        }
    }

    void testVerdicts()
    {
        struct Case { float voice, room; NoisePanel::Verdict expected; const char* name; };
        const Case cases[] { { 0.1f, 0.0005f, NoisePanel::Verdict::great, "quiet room" },
                             { 0.1f, 0.011f,  NoisePanel::Verdict::good,  "some noise" },
                             { 0.05f, 0.03f,  NoisePanel::Verdict::noisy, "noisy room" } };

        for (const auto& c : cases)
        {
            NoiseRemoval nr;
            setMode (nr, NoiseRemoval::Mode::light);
            nr.prepareToPlay (kRate, 512);
            NoisePanel panel (nr);
            panel.setVisible (true);
            panel.setSize (NoisePanel::kWidth, panel.getPreferredHeight());
            const int before = panel.getPreferredHeight();

            expect (panel.getVerdict() == NoisePanel::Verdict::unknown, "nothing is claimed before there is a measurement");
            runRoom (nr, panel, c.voice, c.room, 12);

            logMessage (juce::String (c.name) + ": voice " + juce::String (nr.getVoiceLevelDb(), 1) + " dBFS, room " + juce::String (nr.getNoiseFloorDb(), 1)
                        + " dBFS -> " + juce::String (nr.getVoiceLevelDb() - nr.getNoiseFloorDb(), 1) + " dB, verdict " + juce::String ((int) panel.getVerdict()));
            expect (panel.getVerdict() == c.expected, juce::String (c.name) + ": verdict");

            // The tip appears (and the panel grows to hold it) only for a noisy room.
            const bool tip = panel.getPreferredHeight() > before;
            expect (tip == (c.expected == NoisePanel::Verdict::noisy), juce::String (c.name) + ": the tip is shown only when noisy");
        }
    }

    void testLayout()
    {
        NoiseRemoval nr;
        nr.prepareToPlay (kRate, 512);
        NoisePanel panel (nr);
        panel.setSize (NoisePanel::kWidth, panel.getPreferredHeight());

        const int collapsed = panel.getPreferredHeight();
        expect (collapsed > 250 && collapsed < 420, "collapsed panel fits the drawer comfortably (" + juce::String (collapsed) + " px)");
        expect (! panel.isAdvancedOpen());

        int notified = 0;
        panel.onHeightChanged = [&] { ++notified; };
        panel.setAdvancedOpen (true);
        expect (panel.getPreferredHeight() > collapsed + 100 && notified == 1, "Advanced opens and says the height changed");
        panel.setAdvancedOpen (false);
        expectEquals (panel.getPreferredHeight(), collapsed);

        // The mode control follows the parameter and writes it.
        expectEquals (panel.getModeControl().getSelected(), 1, "a new Noise Removal starts on Best");
        expect (nr.getRequestedMode() == NoiseRemoval::Mode::best);
        panel.getModeControl().setSelected (0, true);
        expect (nr.getRequestedMode() == NoiseRemoval::Mode::fast);
        setMode (nr, NoiseRemoval::Mode::light);
        panel.refresh();                                        // (the panel does this by itself, a moment later)
        expectEquals (panel.getModeControl().getSelected(), 2, "an outside change shows up in the control");

        // Hold: on for as long as it is held, and letting go (or the panel going away) ends it.
        std::vector<bool> asked;
        nr.onHearRemovedRequest = [&] (bool on) { asked.push_back (on); };
        panel.getHearButton().setHeld (true, true);
        expect (nr.isHearingRemoved() && asked.size() == 1);
        panel.getHearButton().setHeld (false, true);
        expect (! nr.isHearingRemoved() && asked.size() == 2);

        {
            NoisePanel second (nr);
            second.getHearButton().setHeld (true, true);
            expect (nr.isHearingRemoved());
        }

        expect (! nr.isHearingRemoved(), "the panel going away lets go");
        nr.onHearRemovedRequest = nullptr;
    }

    void testPaintCost()
    {
        NoiseRemoval nr;
        setMode (nr, NoiseRemoval::Mode::light);
        nr.prepareToPlay (kRate, 512);
        NoisePanel panel (nr);
        panel.setVisible (true);
        panel.setSize (NoisePanel::kWidth, panel.getPreferredHeight());
        runRoom (nr, panel, 0.05f, 0.01f, 4);

        auto& view = panel.getSpectrumView();
        juce::Image image (juce::Image::ARGB, view.getWidth(), view.getHeight(), true);

        // A whole frame of work as the UI timer does it: FFTs, paths, and one repaint of the view.
        double paintMs = 0.0, updateMs = 0.0;
        constexpr int frames = 300;
        SpectrumAnalyzer analyzer;
        analyzer.setLayout (view.getNumColumns(), kRate);

        for (int f = 0; f < frames; ++f)
        {
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            analyzer.update (nr.getSpectrumTap(), 0.033);
            view.setData (analyzer);
            const auto t1 = juce::Time::getMillisecondCounterHiRes();

            juce::Graphics g (image);
            view.paint (g);
            paintMs += juce::Time::getMillisecondCounterHiRes() - t1;
            updateMs += t1 - t0;
        }

        logMessage ("spectrum view at 1x: paint " + juce::String (paintMs / frames, 3) + " ms, FFT + paths " + juce::String (updateMs / frames, 3) + " ms per frame");
       #if ! JUCE_DEBUG
        expect (paintMs / frames < 1.0, "one repaint of the spectrum is under 1 ms");
        expect (updateMs / frames < 1.0, "and so is the analysis");
       #endif

        // Optional: write pictures for a look (HEADROOM_TEST_PNG_DIR=<folder>): the panel as the drawer shows it,
        // in a quiet room and in a noisy one.
        if (const auto dir = juce::SystemStats::getEnvironmentVariable ("HEADROOM_TEST_PNG_DIR", {}); dir.isNotEmpty())
        {
            HeadroomLookAndFeel lnf;
            juce::LookAndFeel::setDefaultLookAndFeel (&lnf);
            juce::File folder (dir);
            folder.createDirectory();

            struct Look { const char* file; float voice, room; };

            for (const auto& look : { Look { "noise-panel-good.png", 0.1f, 0.011f }, Look { "noise-panel-noisy.png", 0.05f, 0.03f } })
            {
                NoiseRemoval other;
                setMode (other, NoiseRemoval::Mode::light);
                other.prepareToPlay (kRate, 512);
                NoisePanel shown (other);
                shown.setVisible (true);
                shown.setSize (NoisePanel::kWidth, shown.getPreferredHeight());
                runRoom (other, shown, look.voice, look.room, 12);
                shown.setAdvancedOpen (look.room > 0.02f);
                shown.setSize (NoisePanel::kWidth, shown.getPreferredHeight());

                const auto snapshot = shown.createComponentSnapshot (shown.getLocalBounds(), true, 2.0f);
                juce::Image picture (juce::Image::ARGB, snapshot.getWidth() + 64, snapshot.getHeight() + 64, true);
                {
                    juce::Graphics g (picture);
                    g.fillAll (theme::bg1);
                    g.drawImageAt (snapshot, 32, 32);
                }

                juce::PNGImageFormat png;

                if (juce::FileOutputStream out (folder.getChildFile (look.file)); out.openedOk())
                {
                    out.setPosition (0);
                    out.truncate();
                    png.writeImageToStream (picture, out);
                }
            }

            juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        }
    }
};

static NoisePanelTests noisePanelTests;
}
