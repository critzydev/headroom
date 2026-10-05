// The demo backend feeds the screenshots and the --demo window: its signals must look like a real mix, not a clipping one.
#include "Juce.h"

#include "app/AppController.h"
#include "engine/RtUtils.h"

#include <algorithm>
#include <map>

namespace hr
{
class DemoLevelTests final : public juce::UnitTest
{
public:
    DemoLevelTests() : juce::UnitTest ("Demo backend levels", "headroom") {}

    void runTest() override
    {
        beginTest ("synthetic signals peak between about -18 and -3 dBFS and never clip");

        AppController::Options options;
        options.demo = true;
        options.persist = false;

        AppController controller (options);
        controller.start();
        auto& engine = controller.getEngine();

        // An input channel on a device the demo has talks like the others; one on a device it has not stays silent.
        const auto webcam = controller.addInputChannel ("alsa_input.usb-Generic_USB_Webcam_DEMO-02.analog-stereo", "Webcam");
        const auto absent = controller.addInputChannel ("alsa_input.usb-Not_Plugged_In-00.analog-stereo", "Absent");

        struct Peaks { float input = 0.0f, monitor = 0.0f, stream = 0.0f; };
        std::map<juce::String, Peaks> peaks;
        float masterMonitor = 0.0f, masterStream = 0.0f;

        const auto ids = juce::StringArray { "mic", "system", "game", "music", "chat", webcam, absent };
        const auto start = juce::Time::getMillisecondCounterHiRes();

        // The speech-like sources have gaps of up to a second, so listen for long enough to hear them talk.
        while (juce::Time::getMillisecondCounterHiRes() - start < 6000.0)
        {
            juce::Thread::sleep (8);

            for (const auto& id : ids)
            {
                const auto levels = engine.readChannelLevels (controller.engineIndexFor (id));
                auto& p = peaks[id];
                p.input = juce::jmax (p.input, levels.input.l, levels.input.r);
                p.monitor = juce::jmax (p.monitor, levels.monitor.l, levels.monitor.r);
                p.stream = juce::jmax (p.stream, levels.stream.l, levels.stream.r);
            }

            const auto m = engine.readMasterLevels (Mix::Monitor);
            const auto s = engine.readMasterLevels (Mix::Stream);
            masterMonitor = juce::jmax (masterMonitor, m.l, m.r);
            masterStream = juce::jmax (masterStream, s.l, s.r);
        }

        for (const auto& id : ids)
        {
            const auto& p = peaks[id];

            if (id == absent)
            {
                expect (p.input == 0.0f && p.monitor == 0.0f && p.stream == 0.0f, "an input channel without its device has no sound");
                continue;
            }

            logMessage (id + ": input " + juce::String (linearToDb (p.input), 1) + " dB, monitor " + juce::String (linearToDb (p.monitor), 1)
                        + " dB, stream " + juce::String (linearToDb (p.stream), 1) + " dB");

            // What arrives at a channel (an app, or the mic): a believable level, quiet to loud, never hot.
            expect (linearToDb (p.input) > -24.0f, id + " should be audible");
            expect (linearToDb (p.input) < -3.0f, id + " should stay clear of clipping (" + juce::String (linearToDb (p.input), 1) + " dB)");
        }

        logMessage ("master: monitor " + juce::String (linearToDb (masterMonitor), 1) + " dB, stream " + juce::String (linearToDb (masterStream), 1) + " dB");
        expect (linearToDb (masterMonitor) < -1.0f, "the headphone mix should keep some headroom");
        expect (linearToDb (masterStream) < -1.0f, "the stream mix should keep some headroom");
        expect (linearToDb (masterMonitor) > -20.0f, "the headphone mix should have some level");
    }
};

static DemoLevelTests demoLevelTests;
}
