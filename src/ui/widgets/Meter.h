// Meter ballistics and the one shared 30 fps clock every meter in the window hangs off.
#pragma once

#include "Juce.h"
#include "engine/EngineTypes.h"
#include "engine/Meters.h"
#include "engine/RtUtils.h"

namespace hr
{
// What the UI does with the engine's peak-since-last-read values: 20 dB/s decay and a 1.5 s peak hold.
struct MeterBar
{
    static constexpr float kFloorDb = -60.0f;           // below this nothing is drawn
    static constexpr float kDecayDbPerSecond = 20.0f;
    static constexpr float kPeakHoldSeconds = 1.5f;

    float displayDb = kMinusInfDb;
    float peakDb = kMinusInfDb;
    float holdSeconds = 0.0f;

    void push (float linearPeak, double dt) noexcept
    {
        const float db = linearToDb (linearPeak);
        const float fall = kDecayDbPerSecond * (float) dt;

        displayDb = juce::jmax (db, displayDb - fall, kMinusInfDb);

        if (db >= peakDb)
        {
            peakDb = db;
            holdSeconds = 0.0f;
        }
        else
        {
            holdSeconds += (float) dt;

            if (holdSeconds > kPeakHoldSeconds)
                peakDb = juce::jmax (displayDb, peakDb - fall, kMinusInfDb);
        }
    }

    void reset() noexcept   { *this = {}; }
};

struct StereoMeter
{
    MeterBar l, r;

    void push (const PeakPair& peaks, double dt) noexcept   { l.push (peaks.l, dt); r.push (peaks.r, dt); }
};

// One timer for the whole window. Widgets that animate register here instead of owning a Timer each.
// It idles at 2 Hz while the window is hidden (Linux closes to the background) so the service costs nothing.
class MeterClock final : private juce::Timer
{
public:
    static constexpr int kActiveHz = 30;
    static constexpr int kIdleHz = 2;

    struct Listener
    {
        virtual ~Listener() = default;
        virtual void meterTick (double deltaSeconds) = 0;
    };

    MeterClock() = default;
    ~MeterClock() override      { stopTimer(); }

    void addListener (Listener* l)          { listeners.add (l); }
    void removeListener (Listener* l)       { listeners.remove (l); }

    // The component whose visibility decides between the active and idle rate.
    void setWatched (juce::Component* c)    { watched = c; }
    void start()                            { lastTickMs = juce::Time::getMillisecondCounterHiRes(); startTimerHz (kActiveHz); }
    void stop()                             { stopTimer(); }

    // One step of `deltaSeconds`: the timer calls it; screenshot mode drives it by hand.
    void tick (double deltaSeconds)         { listeners.call ([deltaSeconds] (Listener& l) { l.meterTick (deltaSeconds); }); }

private:
    void timerCallback() override
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();
        const auto dt = juce::jlimit (0.0, 0.25, (now - lastTickMs) / 1000.0);
        lastTickMs = now;

        const bool showing = watched == nullptr || watched->isShowing();

        if (showing != wasShowing)
        {
            wasShowing = showing;
            startTimerHz (showing ? kActiveHz : kIdleHz);
        }

        if (showing)
            tick (dt);
    }

    juce::ListenerList<Listener> listeners;
    juce::Component* watched = nullptr;
    double lastTickMs = 0.0;
    bool wasShowing = true;
};
}
