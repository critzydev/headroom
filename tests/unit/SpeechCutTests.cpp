// "Does the noise removal cut the voice?" on real speech (tests/data, VCTK) in fan, keyboard and room noise.
// Two tests: SpeechCut (category "noise") asserts the numbers; SpeechCutReport (category "noisereport") prints the table
// for any clips and noises you point it at with environment variables, see reportFromEnvironment().
#include "Juce.h"

#include "SpeechCutMetrics.h"
#include "TestHelpers.h"
#include "engine/builtin/NoiseRemoval.h"
#include "engine/denoise/DfnEngine.h"
#include "engine/denoise/DfnRuntime.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>

#ifndef HEADROOM_TEST_DATA_DIR
 #define HEADROOM_TEST_DATA_DIR ""
#endif

namespace hr
{
using namespace test;

namespace
{
    using Mode = NoiseRemoval::Mode;

    struct Lockstep
    {
        Lockstep()  { denoise::DfnEngine::setTestLockstep (true); }
        ~Lockstep() { denoise::DfnEngine::setTestLockstep (false); }
    };

    void setParam (juce::AudioProcessorValueTreeState& apvts, const char* id, float plainValue)
    {
        auto* p = apvts.getParameter (id);
        jassert (p != nullptr);
        p->setValueNotifyingHost (p->convertTo0to1 (plainValue));
    }

    bool dfnPresent()   { return dfn::isAvailable (dfn::Model::fast) && dfn::isAvailable (dfn::Model::best); }

    juce::String environment (const char* name)
    {
        return juce::SystemStats::getEnvironmentVariable (name, {});
    }

    std::vector<juce::String> splitList (const juce::String& s)
    {
        std::vector<juce::String> out;

        for (auto& t : juce::StringArray::fromTokens (s, ",", ""))
            if (t.trim().isNotEmpty())
                out.push_back (t.trim());

        return out;
    }

    // Mono through the effect in 256-sample blocks (both channels carry the signal, like the mic). The output is as long
    // as the input plus the latency: the tail is the effect's last audio. `realTime` paces the blocks by the clock, which
    // is what the worker thread lives with in the program; without it (and with Lockstep) the result does not depend on
    // the machine at all.
    std::vector<float> runThrough (NoiseRemoval& nr, const std::vector<float>& x, int latency, bool realTime, double* cpuShare = nullptr)
    {
        double cpuSum = 0.0;
        int cpuCount = 0;
        std::vector<float> l (x), r (x);
        l.resize (x.size() + (size_t) latency, 0.0f);
        r.resize (l.size(), 0.0f);
        juce::MidiBuffer midi;
        const int block = 256;
        const auto start = juce::Time::getHighResolutionTicks();

        for (int offset = 0; offset < (int) l.size(); offset += block)
        {
            const int n = juce::jmin (block, (int) l.size() - offset);
            float* channels[2] { l.data() + offset, r.data() + offset };
            juce::AudioBuffer<float> buffer (channels, 2, n);
            nr.processBlock (buffer, midi);

            if (realTime)
            {
                if (offset > (int) l.size() / 2)
                {
                    cpuSum += (double) nr.getCpuLoad();
                    ++cpuCount;
                }

                const double due = (double) (offset + n) / kRate;

                while (true)
                {
                    const double now = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - start);

                    if (now >= due)
                        break;

                    if (due - now > 0.002)
                        juce::Thread::sleep (1);
                    else
                        std::this_thread::yield();
                }
            }
        }

        if (cpuShare != nullptr && cpuCount > 0)
            *cpuShare = cpuSum / cpuCount;

        return l;
    }

    struct Run
    {
        cut::Counts counts;
        uint32_t missed = 0;
        int latency = 0;
        double cpuShare = 0.0;          // real-time runs: mean share of one core (audio + network thread) over the second half
        float worstFrameMs = 0.0f;
    };

    Run evaluate (Mode mode, float strengthPercent, const cut::Mixed& mix, bool realTime)
    {
        NoiseRemoval nr;
        setParam (nr.apvts, "mode", (float) (int) mode);
        setParam (nr.apvts, "strength", strengthPercent);
        nr.prepareToPlay (kRate, 512);
        nr.waitUntilReady (30000);

        // Experiments: HEADROOM_DFN_THRESH_FAST / _BEST = "min,erb,df" sets libDF's local-SNR thresholds for that mode.
        if (mode != Mode::light)
        {
            const auto spec = splitList (environment (mode == Mode::fast ? "HEADROOM_DFN_THRESH_FAST" : "HEADROOM_DFN_THRESH_BEST"));

            if (spec.size() == 3)
                if (auto* engine = dynamic_cast<denoise::DfnEngine*> (nr.getActiveEngineForTesting()))
                    engine->setThresholds ({ spec[0].getFloatValue(), spec[1].getFloatValue(), spec[2].getFloatValue() });

            // HEADROOM_DFN_PROT_FAST / _BEST = "speechLsnrDb,limitDb,holdFrames,releaseDbPerFrame,armFrames"
            const auto prot = splitList (environment (mode == Mode::fast ? "HEADROOM_DFN_PROT_FAST" : "HEADROOM_DFN_PROT_BEST"));

            if (prot.size() == 5)
                if (auto* engine = dynamic_cast<denoise::DfnEngine*> (nr.getActiveEngineForTesting()))
                    engine->setProtection ({ prot[0].getFloatValue(), prot[1].getFloatValue(), prot[2].getIntValue(), prot[3].getFloatValue(), prot[4].getIntValue() });
        }

        Run run;
        run.latency = nr.getLatencySamples();
        const auto y = runThrough (nr, mix.noisy, run.latency, realTime, &run.cpuShare);
        run.counts = cut::analyse (mix, y, run.latency);
        run.missed = nr.getMissedFrames();
        run.worstFrameMs = nr.getWorstFrameMs();
        return run;
    }

    juce::String modeName (Mode m)  { return NoiseRemoval::getModeName (m); }

    juce::String line (const cut::Counts& c)
    {
        return juce::String::formatted ("%5d active | cut>6dB %5.2f %% | cut>12dB %5.2f %% | onset>12 %5.2f %% | end>12 %5.2f %% | mean %+5.1f dB worst %+6.1f | pause %5.1f dB gap %5.1f dB | snr gain %5.1f dB",
                                        c.active, c.cut6Percent(), c.cut12Percent(), c.onsetCut12Percent(), c.endingCut12Percent(),
                                        c.meanLevelDb(), c.worstLevel, c.pauseRemovalDb(), c.gapRemovalDb(), c.snrGainDb());
    }

    // The committed clips (tests/data/clips/*.wav), or nothing when they are not there.
    std::vector<std::vector<float>> committedClips()
    {
        std::vector<std::vector<float>> clips;
        const juce::File dir = juce::File (juce::String (HEADROOM_TEST_DATA_DIR)).getChildFile ("speech");

        if (! dir.isDirectory())
            return clips;

        auto files = dir.findChildFiles (juce::File::findFiles, false, "*.wav");
        files.sort();

        for (auto& f : files)
        {
            std::vector<float> samples;

            if (cut::readWav48k (f, samples))
                clips.push_back (std::move (samples));
        }

        return clips;
    }
}

// ---------------------------------------------------------------------------------------------------

class SpeechCutTests final : public juce::UnitTest
{
public:
    SpeechCutTests() : juce::UnitTest ("SpeechCut", "noise") {}

    void runTest() override
    {
        const auto clips = committedClips();

        beginTest ("speech-cut metric: what it counts as cut");

        if (clips.empty())
        {
            logMessage ("skipped: no speech clips under " + juce::String (HEADROOM_TEST_DATA_DIR) + "/speech");
            return;
        }

        testMetric (clips);

        beginTest ("real speech: Fast and Best do not cut the voice");

        if (! dfnPresent())
        {
            logMessage ("skipped: the DeepFilterNet models are not available");
            return;
        }

        testNetworks (clips);
    }

private:
    // The measure itself, on signals we spoil by hand (no network involved).
    void testMetric (const std::vector<std::vector<float>>& clips)
    {
        const auto speech = cut::makeSequence (clips);
        const auto noise = makeRoomNoise ((int) speech.size(), 21);
        const auto mix = cut::mixAtSnr (speech, noise, 10.0);
        const int lag = 2400;

        // An output that is the input, delayed by the latency the analysis is told about: nothing is cut, nothing removed.
        auto delayed = [&] (const std::vector<float>& x)
        {
            std::vector<float> y (x.size() + (size_t) lag, 0.0f);
            std::copy (x.begin(), x.end(), y.begin() + lag);
            return y;
        };

        const auto same = cut::analyse (mix, delayed (mix.noisy), lag);
        expect (same.active > 300, "active frames found: " + juce::String (same.active));
        expectEquals (same.cut6, 0);
        expectEquals (same.cut12, 0);
        expect (std::abs (same.pauseRemovalDb()) < 0.01, "an untouched pause is not noise removal");
        expect (same.pause > 100, "pauses found: " + juce::String (same.pause));

        // The clean speech itself: no cut, and the noise is gone from the pauses.
        const auto clean = cut::analyse (mix, delayed (mix.clean), lag);
        expectEquals (clean.cut6, 0);
        expect (clean.pauseRemovalDb() > 15.0, "the noise is gone from the pauses: " + juce::String (clean.pauseRemovalDb(), 1));
        expect (clean.snrGainDb() > 8.0, "removing all the noise is a big SNR gain: " + juce::String (clean.snrGainDb(), 1));

        // The whole speech 8 dB down: every active frame is cut by more than 6 dB and by less than 12.
        auto quieter = mix.clean;

        for (auto& v : quieter)
            v *= (float) std::pow (10.0, -8.0 / 20.0);

        const auto down = cut::analyse (mix, delayed (quieter), lag);
        expectEquals (down.cut6, down.active);
        expectEquals (down.cut12, 0);

        // The first 40 ms of every stretch of speech muted (a chopped word onset): onset frames are cut, the rest is not.
        auto chopped = mix.clean;
        const auto band = cut::speechBand (mix.clean);
        const auto energy = cut::frameEnergies (band, (int) mix.clean.size() / cut::kFrame);
        const auto active = cut::activeFrames (energy);

        for (size_t k = 8; k + 4 < active.size(); ++k)
        {
            bool quiet = true;

            for (size_t j = 1; j <= 7; ++j)
                quiet = quiet && ! active[k - j];

            if (active[k] && quiet)
                for (size_t j = 0; j < 4; ++j)
                    std::fill_n (chopped.begin() + (long) ((k + j) * cut::kFrame), cut::kFrame, 0.0f);
        }

        const auto onsets = cut::analyse (mix, delayed (chopped), lag);
        expect (onsets.onset >= 12 && onsets.onsetCut12 >= onsets.onset - 2, "muted onsets are counted: " + juce::String (onsets.onsetCut12) + " of " + juce::String (onsets.onset));
        expect (onsets.cut12 <= onsets.onsetCut12 + onsets.endingCut12 + 4, "and only those");

        // A one-frame misalignment shows up (so the latency the effect reports matters): the same clean speech, 10 ms late.
        const auto late = cut::analyse (mix, delayed (mix.clean), lag + cut::kFrame);
        expect (late.cut6 > 0, "10 ms of misalignment cuts frames at the edges of words");
    }

    // The effect itself, in real speech with a fan, typing and room noise at 5-20 dB, Strength 80 % (the default).
    void testNetworks (const std::vector<std::vector<float>>& clips)
    {
        const Lockstep lockstep;
        const auto speech = cut::makeSequence (clips);
        const int n = (int) speech.size();

        std::map<juce::String, std::vector<float>> noises;
        noises["fan"] = makeRoomNoise (n, 11);
        noises["keyboard"] = cut::makeKeyboard (n, 12);
        noises["room"] = cut::makeRoomTone (n, 13);

        struct Case { const char* noise; double snr; };
        const Case cases[] { { "fan", 5.0 }, { "fan", 10.0 }, { "fan", 20.0 }, { "keyboard", 10.0 }, { "room", 10.0 } };

        cut::Counts total[2];

        for (auto mode : { Mode::fast, Mode::best })
        {
            auto& all = total[mode == Mode::fast ? 0 : 1];
            const juce::String who = modeName (mode);

            for (auto& c : cases)
            {
                const auto mix = cut::mixAtSnr (speech, noises[c.noise], c.snr);
                const auto run = evaluate (mode, 80.0f, mix, false);
                logMessage (who + " " + juce::String (c.noise).paddedRight (' ', 8) + juce::String (c.snr, 0).paddedLeft (' ', 3) + " dB | " + line (run.counts));
                expectEquals ((int) run.missed, 0, who + ": no frame was late");
                expectEquals (run.latency, mode == Mode::fast ? 1440 : 2400);
                all += run.counts;

                // The case that was broken: speech over a faint noise. The network's own first stage alone (the second one,
                // deep filtering, skipped because the frame looked clean) took the low frequencies out of the voice.
                if (c.snr >= 20.0)
                    expect (run.counts.cut6Percent() < kMaxCut6AtHighSnr, who + ": speech over a faint noise is left alone (" + juce::String (run.counts.cut6Percent(), 2) + " % cut by more than 6 dB)");
            }

            logMessage (who + " all | " + line (all));
            expect (all.cut6Percent() < kMaxCut6, who + ": fewer than " + juce::String (kMaxCut6, 1) + " % of the speech frames cut by more than 6 dB (" + juce::String (all.cut6Percent(), 2) + ")");
            expect (all.cut12Percent() < kMaxCut12, who + ": fewer than " + juce::String (kMaxCut12, 1) + " % of the speech frames cut by more than 12 dB (" + juce::String (all.cut12Percent(), 2) + ")");
            expect (all.pauseRemovalDb() > kMinPauseRemovalDb, who + ": the pauses are still cleaned (" + juce::String (all.pauseRemovalDb(), 1) + " dB)");
        }

        expect (total[1].cut6Percent() <= total[0].cut6Percent() + kBestVersusFastCut6, "Best cuts no more than Fast does (" + juce::String (total[1].cut6Percent(), 2) + " vs " + juce::String (total[0].cut6Percent(), 2) + " %)");

        // Strength 0 % is the input, delayed: nothing is cut. (This also proves that the latency the effect reports is the real one.)
        for (auto mode : { Mode::fast, Mode::best })
        {
            const auto run = evaluate (mode, 0.0f, cut::mixAtSnr (speech, noises["fan"], 10.0), false);
            expectEquals (run.counts.cut6, 0, modeName (mode) + " at 0 %: nothing is cut");
        }
    }

    // What it was before the fix, on these clips (Strength 80 %): Best cut 14.8 % of the frames by more than 6 dB (15 % in the 20 dB
    // case) and 1.5 % by more than 12 dB, Fast 3.2 % and 1.0 %, with 19.7 / 16.1 dB removed from the pauses. Now: Best 0.4 % and
    // 0.1 %, Fast 0.5 % and 0.1 %, 16.5 / 15.0 dB. The bars sit in between and leave room for another CPU's floating point.
    static constexpr double kMaxCut6 = 2.0;
    static constexpr double kMaxCut6AtHighSnr = 1.0;
    static constexpr double kMaxCut12 = 0.5;
    static constexpr double kMinPauseRemovalDb = 12.0;
    static constexpr double kBestVersusFastCut6 = 0.5;
};

// ---------------------------------------------------------------------------------------------------

class SpeechCutReport final : public juce::UnitTest
{
public:
    SpeechCutReport() : juce::UnitTest ("SpeechCutReport", "noisereport") {}

    void runTest() override
    {
        beginTest ("speech-cut report");

        if (environment ("HEADROOM_CUT_REPORT").isEmpty())
        {
            logMessage ("skipped: set HEADROOM_CUT_REPORT=1 (and HEADROOM_CUT_DIR) to print the table");
            return;
        }

        if (! dfnPresent())
        {
            logMessage ("skipped: the DeepFilterNet models are not available");
            return;
        }

        if (environment ("HEADROOM_CUT_DIAG") == "noise")
            diagnoseNoiseLsnr();
        else if (environment ("HEADROOM_CUT_DIAG").isNotEmpty())
            diagnoseLsnr();
        else
            reportFromEnvironment();
    }

private:
    // Does noise alone look like speech to the network? For each noise at the level it has in a 10 dB mix: how often
    // the local SNR estimate reaches 0 / 5 / 10 dB, and the longest run of such frames.
    void diagnoseNoiseLsnr()
    {
        const int n = (int) kRate * 40;
        std::map<juce::String, std::vector<float>> noises;
        noises["fan"] = makeRoomNoise (n, 11);
        noises["keyboard"] = cut::makeKeyboard (n, 12);
        noises["room"] = cut::makeRoomTone (n, 13);

        const juce::File noiseDir (environment ("HEADROOM_CUT_NOISE_DIR"));

        if (noiseDir.isDirectory())
            for (auto& f : noiseDir.findChildFiles (juce::File::findFiles, false, "*.wav"))
            {
                std::vector<float> samples;

                if (cut::readWav48k (f, samples) && ! samples.empty())
                {
                    samples.resize ((size_t) n);
                    noises[f.getFileNameWithoutExtension()] = std::move (samples);
                }
            }

        for (auto model : { dfn::Model::fast, dfn::Model::best })
            for (auto& entry : noises)
            {
                juce::String error;
                auto loaded = dfn::Models::get (model, error);
                auto state = loaded->createState();
                const double level = std::pow (10.0, -36.0 / 20.0);
                const double gain = level / juce::jmax (1.0e-9, rms (entry.second.data(), n));
                std::vector<float> in (480), out (480);
                const int frames = n / 480;
                int above[3] {}, run[3] {}, longest[3] {};
                const float edges[3] { 0.0f, 5.0f, 10.0f };
                std::vector<int> hist (10, 0);

                for (int k = 0; k < frames; ++k)
                {
                    for (int i = 0; i < 480; ++i)
                        in[(size_t) i] = (float) (gain * (double) entry.second[(size_t) (k * 480 + i)]);

                    const float l = state->process (in.data(), out.data());

                    if (k < 50)
                        continue;

                    for (int e = 0; e < 3; ++e)
                    {
                        if (l >= edges[e]) { ++above[e]; ++run[e]; longest[e] = juce::jmax (longest[e], run[e]); }
                        else run[e] = 0;
                    }
                }

                logMessage (juce::String (model == dfn::Model::fast ? "Fast " : "Best ") + entry.first.paddedRight (' ', 10)
                            + juce::String::formatted (" lsnr>=0: %5.2f %% (longest run %3d)  >=5: %5.2f %% (%3d)  >=10: %5.2f %% (%3d)",
                                                       100.0 * above[0] / (frames - 50), longest[0], 100.0 * above[1] / (frames - 50), longest[1],
                                                       100.0 * above[2] / (frames - 50), longest[2]));
            }
    }

    // Why does a frame get cut? Feeds the noisy sequence straight into the network (no blend, no limit) and tabulates, for
    // the active speech frames, how often the network's own output is more than 12 dB below the clean speech against the
    // local SNR it estimated for that frame (libDF acts on that estimate: below min_db_thresh it mutes the frame, above
    // max_db_df_thresh it skips the second stage, above max_db_erb_thresh it passes the frame untouched).
    void diagnoseLsnr()
    {
        const juce::File dir (environment ("HEADROOM_CUT_DIR"));
        auto files = dir.findChildFiles (juce::File::findFiles, false, "*.wav");
        files.sort();
        std::map<juce::String, std::vector<std::vector<float>>> bySpeaker;

        for (auto& f : files)
        {
            std::vector<float> samples;

            if (cut::readWav48k (f, samples))
                bySpeaker[f.getFileName().upToFirstOccurrenceOf ("_", false, false)].push_back (std::move (samples));
        }

        std::vector<std::vector<float>> sequences;

        for (auto& speaker : bySpeaker)
            if (speaker.second.size() >= 4 && sequences.size() < 10)
                sequences.push_back (cut::makeSequence ({ speaker.second.begin(), speaker.second.begin() + 4 }));

        size_t longest = 0;

        for (auto& s : sequences)
            longest = std::max (longest, s.size());

        const auto noise = makeRoomNoise ((int) longest, 11);
        const double snr = environment ("HEADROOM_CUT_SNRS").isEmpty() ? 10.0 : environment ("HEADROOM_CUT_SNRS").getDoubleValue();

        for (auto model : { dfn::Model::fast, dfn::Model::best })
        {
            juce::String error;
            auto loaded = dfn::Models::get (model, error);

            if (loaded == nullptr)
            {
                logMessage ("no model: " + error);
                continue;
            }

            const int lat = loaded->latency();

            // [bin][0] = active frames, [1] = cut > 12 dB, [2] = cut > 6 dB, per local SNR bin and per lag between the call and the frame
            struct Bin { int n = 0, cut12 = 0, cut6 = 0; };
            const double edges[] { -100.0, -10.0, -5.0, 0.0, 10.0, 20.0, 30.0, 1000.0 };
            constexpr int kBins = 7;
            std::vector<Bin> bins[6];

            for (auto& b : bins)
                b.assign (kBins, {});

            int activeTotal = 0;

            for (auto& seq : sequences)
            {
                const auto mix = cut::mixAtSnr (seq, noise, snr);
                auto state = loaded->createState();
                const auto spec = splitList (environment (model == dfn::Model::fast ? "HEADROOM_DFN_THRESH_FAST" : "HEADROOM_DFN_THRESH_BEST"));

                if (spec.size() == 3)
                    state->setThresholds (spec[0].getFloatValue(), spec[1].getFloatValue(), spec[2].getFloatValue());

                state->setAttenuationLimit (100.0f);
                state->setPostFilter (0.0f);

                const int frames = (int) mix.noisy.size() / 480;
                std::vector<float> wet ((size_t) (frames + 8) * 480, 0.0f);
                std::vector<float> lsnr ((size_t) frames + 8, 0.0f);
                std::vector<float> in (480);

                for (int k = 0; k < frames + 8; ++k)
                {
                    for (int i = 0; i < 480; ++i)
                        in[(size_t) i] = k < frames ? mix.noisy[(size_t) (k * 480 + i)] : 0.0f;

                    lsnr[(size_t) k] = state->process (in.data(), wet.data() + (size_t) k * 480);
                }

                const auto cleanBand = cut::speechBand (mix.clean);
                const auto wetBand = cut::speechBand (wet);
                const auto cleanE = cut::frameEnergies (cleanBand, frames);
                const auto wetE = cut::frameEnergies (wetBand, frames, lat);
                const auto active = cut::activeFrames (cleanE);

                for (int k = 0; k < frames; ++k)
                {
                    if (! active[(size_t) k])
                        continue;

                    ++activeTotal;
                    const double level = 10.0 * std::log10 (juce::jmax (1.0e-30, wetE[(size_t) k]) / cleanE[(size_t) k]);

                    for (int lag = 0; lag < 6; ++lag)
                    {
                        const float l = lsnr[(size_t) juce::jmin (frames + 7, k + lag)];
                        int bin = 0;

                        while (bin < kBins - 1 && l >= edges[bin + 1])
                            ++bin;

                        auto& b = bins[lag][(size_t) bin];
                        ++b.n;
                        if (level < -12.0) ++b.cut12;
                        if (level < -6.0)  ++b.cut6;
                    }
                }
            }

            logMessage (juce::String (model == dfn::Model::fast ? "Fast" : "Best") + ": " + juce::String (activeTotal) + " active frames at " + juce::String (snr, 0) + " dB SNR");

            for (int lag = 0; lag < 6; ++lag)
            {
                juce::String row = "  lsnr taken " + juce::String (lag) + " frames after: ";

                for (int b = 0; b < kBins; ++b)
                    row << juce::String::formatted ("[%5.0f..%5.0f) n=%4d cut6=%4d cut12=%4d  ", edges[b], edges[b + 1], bins[lag][(size_t) b].n, bins[lag][(size_t) b].cut6, bins[lag][(size_t) b].cut12);

                logMessage (row);
            }
        }
    }

    // HEADROOM_CUT_DIR          folder of 48 kHz WAV clips named <speaker>_<number>.wav (default: tests/data/speech)
    // HEADROOM_CUT_NOISE_DIR    folder of 48 kHz WAV noises, used in addition to the built-in fan / keyboard / room
    // HEADROOM_CUT_NOISES       comma list: fan, keyboard, room and the file names (without .wav) in that folder (default: all)
    // HEADROOM_CUT_MODES        fast,best,light      HEADROOM_CUT_SNRS 0,5,10,20      HEADROOM_CUT_STRENGTHS 80,100
    // HEADROOM_CUT_SEQS         sequences to use (default 4), HEADROOM_CUT_PER_SEQ clips in one (default 4)
    // HEADROOM_CUT_REALTIME     1: paced by the clock instead of lockstep (slow, the worker thread is really tested)
    // HEADROOM_CUT_CSV          file to append one row per mode x SNR x strength x noise to
    void reportFromEnvironment()
    {
        juce::File dir (environment ("HEADROOM_CUT_DIR"));

        if (environment ("HEADROOM_CUT_DIR").isEmpty())
            dir = juce::File (juce::String (HEADROOM_TEST_DATA_DIR)).getChildFile ("speech");

        auto files = dir.findChildFiles (juce::File::findFiles, false, "*.wav");
        files.sort();

        std::map<juce::String, std::vector<std::vector<float>>> bySpeaker;

        for (auto& f : files)
        {
            std::vector<float> samples;

            if (cut::readWav48k (f, samples))
                bySpeaker[f.getFileName().upToFirstOccurrenceOf ("_", false, false)].push_back (std::move (samples));
        }

        const int perSeq = juce::jmax (1, environment ("HEADROOM_CUT_PER_SEQ").getIntValue() > 0 ? environment ("HEADROOM_CUT_PER_SEQ").getIntValue() : 4);
        const int seqCount = environment ("HEADROOM_CUT_SEQS").getIntValue() > 0 ? environment ("HEADROOM_CUT_SEQS").getIntValue() : 4;

        std::vector<std::vector<float>> sequences;

        for (int round = 0; (int) sequences.size() < seqCount && round < 20; ++round)
        {
            bool any = false;

            for (auto& speaker : bySpeaker)
            {
                if ((int) sequences.size() >= seqCount)
                    break;

                if ((round + 1) * perSeq > (int) speaker.second.size())
                    continue;

                std::vector<std::vector<float>> group (speaker.second.begin() + round * perSeq, speaker.second.begin() + (round + 1) * perSeq);
                sequences.push_back (cut::makeSequence (group));
                any = true;
            }

            if (! any)
                break;
        }

        expect (! sequences.empty(), "clips found in " + dir.getFullPathName());

        if (sequences.empty())
            return;

        // Noises. They are long enough for the longest sequence (the built-in ones are made per sequence).
        size_t longest = 0;

        for (auto& s : sequences)
            longest = std::max (longest, s.size());

        std::map<juce::String, std::vector<float>> noises;
        noises["fan"] = makeRoomNoise ((int) longest, 11);
        noises["keyboard"] = cut::makeKeyboard ((int) longest, 12);
        noises["room"] = cut::makeRoomTone ((int) longest, 13);

        const juce::File noiseDir (environment ("HEADROOM_CUT_NOISE_DIR"));

        if (noiseDir.isDirectory())
            for (auto& f : noiseDir.findChildFiles (juce::File::findFiles, false, "*.wav"))
            {
                std::vector<float> samples;

                if (cut::readWav48k (f, samples) && ! samples.empty())
                    noises[f.getFileNameWithoutExtension()] = std::move (samples);
            }

        std::vector<juce::String> noiseNames = splitList (environment ("HEADROOM_CUT_NOISES"));

        if (noiseNames.empty())
            for (auto& n : noises)
                noiseNames.push_back (n.first);

        std::vector<Mode> modes;

        for (auto& m : splitList (environment ("HEADROOM_CUT_MODES").isEmpty() ? juce::String ("fast,best,light") : environment ("HEADROOM_CUT_MODES")))
            modes.push_back (m == "fast" ? Mode::fast : m == "best" ? Mode::best : Mode::light);

        std::vector<double> snrs, strengths;

        for (auto& s : splitList (environment ("HEADROOM_CUT_SNRS").isEmpty() ? juce::String ("0,5,10,20") : environment ("HEADROOM_CUT_SNRS")))
            snrs.push_back (s.getDoubleValue());

        for (auto& s : splitList (environment ("HEADROOM_CUT_STRENGTHS").isEmpty() ? juce::String ("80,100") : environment ("HEADROOM_CUT_STRENGTHS")))
            strengths.push_back (s.getDoubleValue());

        const bool realTime = environment ("HEADROOM_CUT_REALTIME").isNotEmpty();
        const juce::File csv (environment ("HEADROOM_CUT_CSV"));

        std::unique_ptr<Lockstep> lockstep;

        if (! realTime)
            lockstep = std::make_unique<Lockstep>();

        double seconds = 0.0;

        for (auto& s : sequences)
            seconds += (double) s.size() / kRate;

        logMessage ("sequences: " + juce::String ((int) sequences.size()) + " (" + juce::String (seconds, 1) + " s of speech and pauses), noises: "
                    + juce::String ((int) noiseNames.size()) + ", " + (realTime ? "real-time pacing" : "lockstep"));

        for (auto mode : modes)
            for (double strength : strengths)
                for (double snr : snrs)
                {
                    cut::Counts all;
                    uint32_t missedAll = 0;

                    for (auto& noiseName : noiseNames)
                    {
                        cut::Counts sum;
                        uint32_t missed = 0;
                        int latency = 0;
                        double cpu = 0.0;
                        float worstMs = 0.0f;

                        for (size_t q = 0; q < sequences.size(); ++q)
                        {
                            const auto& noise = noises[noiseName];
                            const auto mix = cut::mixAtSnr (sequences[q], noise, snr, -26.0, q * 700001 % noise.size());
                            const auto run = evaluate (mode, (float) strength, mix, realTime);
                            sum += run.counts;
                            missed += run.missed;
                            latency = run.latency;
                            cpu += run.cpuShare / (double) sequences.size();
                            worstMs = juce::jmax (worstMs, run.worstFrameMs);
                        }

                        logMessage (modeName (mode) + " " + juce::String ((int) strength) + "% snr " + juce::String (snr, 0).paddedLeft (' ', 2) + " dB "
                                    + noiseName.paddedRight (' ', 9) + " | " + line (sum) + " | missed " + juce::String ((int) missed)
                                    + (realTime ? " | cpu " + juce::String (100.0 * cpu, 1) + " % worst frame " + juce::String (worstMs, 2) + " ms" : juce::String()));
                        all += sum;
                        missedAll += missed;

                        if (csv != juce::File())
                            csv.appendText (modeName (mode) + "," + juce::String ((int) strength) + "," + juce::String (snr, 0) + "," + noiseName + ","
                                            + juce::String (sum.active) + "," + juce::String (sum.cut6) + "," + juce::String (sum.cut12) + ","
                                            + juce::String (sum.onset) + "," + juce::String (sum.onsetCut12) + "," + juce::String (sum.ending) + ","
                                            + juce::String (sum.endingCut12) + "," + juce::String (sum.meanLevelDb(), 2) + "," + juce::String (sum.pauseRemovalDb(), 2)
                                            + "," + juce::String (sum.gapRemovalDb(), 2) + "," + juce::String (sum.snrGainDb(), 2) + "," + juce::String (sum.cut6) + ","
                                            + juce::String ((int) missed) + "," + juce::String (latency) + "\n");
                    }

                    logMessage (">>> " + modeName (mode) + " " + juce::String ((int) strength) + "% snr " + juce::String (snr, 0).paddedLeft (' ', 2) + " dB ALL       | "
                                + line (all) + " | missed " + juce::String ((int) missedAll));
                }
    }
};

static SpeechCutTests speechCutTests;
static SpeechCutReport speechCutReport;
}
