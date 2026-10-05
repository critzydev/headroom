// WaveXlrHardware implementation: the worker's rules for probing, reading the gain, applying a wish once, and asking for
// permission (see WaveXlrHardware.h).
#include "backend/wave/WaveXlrHardware.h"

#include "Juce.h"

#include <chrono>
#include <utility>

namespace hr
{
namespace
{
    constexpr int kMaxWishAttempts = 2;                 // a gain the mic does not take is tried this often, then reported

    void logLine (const std::string& text)
    {
        juce::Logger::writeToLog ("[wave-xlr] " + juce::String::fromUTF8 (text.c_str()));
    }

    const char* presenceText (wave::Presence presence)
    {
        switch (presence)
        {
            case wave::Presence::Unsupported:   return "not supported on this system";
            case wave::Presence::Absent:        return "not connected";
            case wave::Presence::Ambiguous:     return "more than one is connected, leaving them alone";
            case wave::Presence::NoPermission:  return "connected, but this user may not open it yet";
            case wave::Presence::Ready:         return "connected and ready";
        }

        return "";
    }

    bool sameControls (const ExperimentalControls& a, const ExperimentalControls& b)
    {
        return a.on == b.on && a.available == b.available && a.permission == b.permission && a.gainDb == b.gainDb
            && a.busy == b.busy && a.error == b.error;
    }

    std::function<int64_t()> steadyClock()
    {
        return []
        {
            using namespace std::chrono;
            return (int64_t) duration_cast<milliseconds> (steady_clock::now().time_since_epoch()).count();
        };
    }

    // Trouble that trying again cannot cure.
    bool isFinal (wave::Error error)
    {
        return error == wave::Error::NoPermission || error == wave::Error::WrongDevice || error == wave::Error::UnknownLayout
            || error == wave::Error::Unsupported || error == wave::Error::NotFound;
    }
}

WaveXlrHardware::WaveXlrHardware (Options o) : options (std::move (o))
{
    usb = options.usb != nullptr ? std::move (options.usb) : std::make_unique<wave::xlr::XlrUsb>();

    if (! options.nowMs)
        options.nowMs = steadyClock();

    if (! options.sleepMs)
        options.sleepMs = [] (int ms) { std::this_thread::sleep_for (std::chrono::milliseconds (ms)); };

    if (! options.installPermission)
        options.installPermission = [] (const std::function<bool()>& abort) { return wave::permission::installViaPkexec (abort, true); };
}

WaveXlrHardware::~WaveXlrHardware()
{
    stop();
}

void WaveXlrHardware::start()
{
    const std::lock_guard<std::mutex> lock (mutex);

    if (worker.joinable())
        return;

    stopping = false;
    worker = std::thread ([this] { runLoop(); });
}

void WaveXlrHardware::stop()
{
    {
        const std::lock_guard<std::mutex> lock (mutex);
        stopping = true;
    }

    wakeUp.notify_all();

    if (worker.joinable())
        worker.join();
}

void WaveXlrHardware::setEnabled (bool on)
{
    {
        const std::lock_guard<std::mutex> lock (mutex);

        if (enabled == on)
            return;

        enabled = on;
        enabledChanged = true;
        wake = true;
    }

    wakeUp.notify_all();
}

bool WaveXlrHardware::isEnabled() const
{
    const std::lock_guard<std::mutex> lock (mutex);
    return enabled;
}

ExperimentalControls WaveXlrHardware::get() const
{
    const std::lock_guard<std::mutex> lock (mutex);
    return published;
}

void WaveXlrHardware::setGainDb (double db)
{
    {
        const std::lock_guard<std::mutex> lock (mutex);

        if (! enabled)
            return;                     // not opted in: nothing is ever sent

        wish = db;
        wishAttempts = 0;
        wake = true;
    }

    wakeUp.notify_all();
}

void WaveXlrHardware::requestPermission()
{
    {
        const std::lock_guard<std::mutex> lock (mutex);

        if (! enabled)
            return;

        permissionRequested = true;
        wake = true;
    }

    wakeUp.notify_all();
}

void WaveXlrHardware::readSoon (int delayMs)
{
    {
        const std::lock_guard<std::mutex> lock (mutex);
        readSoonAt = options.nowMs() + delayMs;
        wake = true;
    }

    wakeUp.notify_all();
}

void WaveXlrHardware::runLoop()
{
    for (;;)
    {
        {
            const std::lock_guard<std::mutex> lock (mutex);

            if (stopping)
                return;
        }

        tick();

        std::unique_lock<std::mutex> lock (mutex);

        if (stopping)
            return;

        wakeUp.wait_for (lock, std::chrono::milliseconds (200), [this] { return stopping || wake; });
        wake = false;
    }
}

void WaveXlrHardware::publish (const std::function<void (ExperimentalControls&)>& change)
{
    ExperimentalControls before, after;

    {
        const std::lock_guard<std::mutex> lock (mutex);
        before = published;
        change (published);
        published.on = enabled;
        published.error = juce::String::fromUTF8 ((actionError.empty() ? readError : actionError).c_str());
        after = published;
    }

    if (! sameControls (before, after) && options.onChanged)
        options.onChanged();
}

void WaveXlrHardware::tick()
{
    const int64_t now = options.nowMs();

    bool on = false, permissionAsked = false, justChanged = false;

    {
        const std::lock_guard<std::mutex> lock (mutex);
        on = enabled;
        justChanged = std::exchange (enabledChanged, false);
        permissionAsked = std::exchange (permissionRequested, false);
    }

    if (justChanged)
    {
        // Switched on: look at once. Switched off: forget everything, and send nothing more.
        presenceKnown = false;
        presence = wave::Presence::Absent;
        readDue = false;
        readError.clear();
        actionError.clear();
        lastLoggedGain.reset();

        if (! on)
        {
            // A wish made before the switch was seen (switched on, then asked at once) must survive; switching off drops it.
            const std::lock_guard<std::mutex> lock (mutex);
            wish.reset();
            readSoonAt.reset();
        }

        publish ([] (ExperimentalControls& controls) { controls = ExperimentalControls(); });
        logLine (on ? "experimental hardware controls switched on" : "experimental hardware controls switched off");
    }

    if (! on)
        return;

    if (permissionAsked)
        handlePermissionRequest();

    if (! presenceKnown || now >= nextProbeAt)
    {
        nextProbeAt = now + options.probeIntervalMs;
        probe();
    }

    if (presence != wave::Presence::Ready)
        return;

    bool askedToRead = false;

    {
        const std::lock_guard<std::mutex> lock (mutex);

        if (readSoonAt.has_value() && now >= *readSoonAt)
        {
            readSoonAt.reset();
            askedToRead = true;
        }
    }

    if (readDue || askedToRead || now >= nextReadAt)
    {
        readDue = false;
        nextReadAt = now + options.readIntervalMs;
        readAndPublish();
    }

    applyWish();
}

void WaveXlrHardware::probe()
{
    const auto found = usb->probe();
    const bool firstLook = ! presenceKnown;
    const bool changed = firstLook || found != presence;

    presence = found;
    presenceKnown = true;

    if (! changed)
        return;

    // Nobody needs a log line for a computer that has no Wave XLR on the USB bus (or cannot say: not Linux).
    const bool nothingToSay = firstLook && (found == wave::Presence::Absent || found == wave::Presence::Unsupported);

    if (! nothingToSay)
        logLine (std::string ("Wave XLR on USB: ") + presenceText (found));

    const bool there = found == wave::Presence::NoPermission || found == wave::Presence::Ready;

    if (found != wave::Presence::Ready)
        readError.clear();

    publish ([&] (ExperimentalControls& controls)
    {
        controls.available = there;
        controls.permission = found == wave::Presence::Ready;

        if (found != wave::Presence::Ready)
            controls.gainDb.reset();

        if (found == wave::Presence::Ready)
            actionError.clear();        // the permission is there now: whatever was said about it is done with
    });

    if (found == wave::Presence::Ready)
        readDue = true;                 // it has just appeared (plugged in, or the permission arrived): ask what it is set to
}

void WaveXlrHardware::readAndPublish()
{
    wave::xlr::ConfigBlock block {};

    if (! usb->readConfig (block))
    {
        readError = usb->lastErrorText();
        const auto detail = usb->lastErrorDetail();

        if (detail != lastLoggedError)
            logLine ("could not read the Wave XLR: " + detail);

        lastLoggedError = detail;
        publish ([] (ExperimentalControls& controls) { controls.gainDb.reset(); });
        return;
    }

    readError.clear();
    lastLoggedError.clear();

    const auto gain = wave::xlr::gainDbOf (block);       // looksLikeConfig has checked that it is there
    const std::optional<float> gainValue = gain.has_value() ? std::optional<float> ((float) *gain) : std::nullopt;

    publish ([&] (ExperimentalControls& controls) { controls.gainDb = gainValue; });

    if (lastLoggedGain != gainValue)
    {
        lastLoggedGain = gainValue;
        logLine (std::string ("Wave XLR mic gain is ") + (gain.has_value() ? juce::String (*gain, 1).toStdString() + " dB" : std::string ("unknown")));
    }
}

// Puts the gain where the user wants it, once. Only ever runs when the mic has been read (so the block is known to be the
// documented one) and only for a wish made since the last time.
void WaveXlrHardware::applyWish()
{
    std::optional<double> wanted;
    int attempt = 0;

    {
        const std::lock_guard<std::mutex> lock (mutex);

        if (! wish.has_value() || ! enabled)
            return;

        wanted = wish;
    }

    if (! get().gainDb.has_value())
        return;                         // not read yet (or not readable): nothing is written to a block we have not seen

    {
        const std::lock_guard<std::mutex> lock (mutex);

        if (wish != wanted)
            return;                     // a newer wish arrived meanwhile: that one is dealt with next

        attempt = ++wishAttempts;
    }

    logLine ("setting the Wave XLR mic gain to " + juce::String (wave::xlr::quantiseGainDb (*wanted), 1).toStdString() + " dB");
    publish ([] (ExperimentalControls& controls) { controls.busy = true; });

    const bool ok = usb->setGainDb (*wanted);

    if (ok)
    {
        actionError.clear();

        const std::lock_guard<std::mutex> lock (mutex);

        if (wish == wanted)             // a newer wish stays
            wish.reset();
    }
    else
    {
        actionError = usb->lastErrorText();
        logLine ("could not set the Wave XLR mic gain: " + usb->lastErrorDetail());

        const std::lock_guard<std::mutex> lock (mutex);

        if (wish == wanted && (attempt >= kMaxWishAttempts || isFinal (usb->lastError())))
            wish.reset();               // reported, and not tried again until the user asks again
    }

    // Whatever happened, find out what the mic says now.
    readAndPublish();
    publish ([] (ExperimentalControls& controls) { controls.busy = false; });
}

void WaveXlrHardware::handlePermissionRequest()
{
    actionError.clear();
    publish ([] (ExperimentalControls& controls) { controls.busy = true; });

    logLine ("asking for permission to control the Wave XLR (pkexec)");

    const auto result = options.installPermission ([this]
    {
        const std::lock_guard<std::mutex> lock (mutex);
        return stopping;
    });

    logLine ("permission request: " + result.detail);

    if (result.outcome == wave::permission::InstallResult::Outcome::Done)
    {
        // udev has been told; the access shows up within moments.
        auto found = usb->probe();

        for (int attempt = 0; attempt < 10 && found == wave::Presence::NoPermission; ++attempt)
        {
            options.sleepMs (300);
            found = usb->probe();
        }

        if (found == wave::Presence::NoPermission)
            actionError = "It is set up, but the Wave XLR has to be unplugged and plugged back in once before it works.";
    }
    else
    {
        actionError = wave::permission::describe (result.outcome);
    }

    presenceKnown = false;              // look again right now
    publish ([] (ExperimentalControls& controls) { controls.busy = false; });
}
}
