// WaveHardware implementation: the worker's rules for probing, reading, restoring ClipGuard and asking for permission.
#include "backend/wave/WaveHardware.h"

#include "Juce.h"

#include <chrono>

namespace hr
{
namespace
{
    constexpr int kBudgetWindowMs = 10 * 60 * 1000;     // a mic that keeps falling back is left alone after this many
    constexpr size_t kMaxWritesPerBudget = 4;            //   automatic writes in this long (a new request starts over)

    void logLine (const std::string& text)
    {
        juce::Logger::writeToLog ("[wave] " + juce::String::fromUTF8 (text.c_str()));
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

    bool sameControls (const HardwareControls& a, const HardwareControls& b)
    {
        return a.usbAvailable == b.usbAvailable && a.usbPermission == b.usbPermission && a.clipGuard == b.clipGuard
            && a.gainDb == b.gainDb && a.gainLocked == b.gainLocked && a.busy == b.busy && a.error == b.error;
    }

    std::function<int64_t()> steadyClock()
    {
        return []
        {
            using namespace std::chrono;
            return (int64_t) duration_cast<milliseconds> (steady_clock::now().time_since_epoch()).count();
        };
    }
}

WaveHardware::WaveHardware (Options o) : options (std::move (o))
{
    usb = options.usb != nullptr ? std::move (options.usb) : std::make_unique<wave::WaveUsb>();

    if (! options.nowMs)
        options.nowMs = steadyClock();

    if (! options.sleepMs)
        options.sleepMs = [] (int ms) { std::this_thread::sleep_for (std::chrono::milliseconds (ms)); };

    if (! options.installPermission)
    {
        options.installPermission = [] (const std::function<bool()>& abort) { return wave::permission::installViaPkexec (abort); };
    }
}

WaveHardware::~WaveHardware()
{
    stop();
}

void WaveHardware::start()
{
    const std::lock_guard<std::mutex> lock (mutex);

    if (worker.joinable())
        return;

    stopping = false;
    worker = std::thread ([this] { runLoop(); });
}

void WaveHardware::stop()
{
    {
        const std::lock_guard<std::mutex> lock (mutex);
        stopping = true;
    }

    wakeUp.notify_all();

    if (worker.joinable())
        worker.join();
}

HardwareControls WaveHardware::get() const
{
    const std::lock_guard<std::mutex> lock (mutex);
    return published;
}

void WaveHardware::setClipGuard (bool on)
{
    {
        const std::lock_guard<std::mutex> lock (mutex);
        desired = on;
        newRequest = true;
        wake = true;
    }

    wakeUp.notify_all();
}

void WaveHardware::requestPermission()
{
    {
        const std::lock_guard<std::mutex> lock (mutex);
        permissionRequested = true;
        wake = true;
    }

    wakeUp.notify_all();
}

void WaveHardware::readSoon (int delayMs)
{
    {
        const std::lock_guard<std::mutex> lock (mutex);
        readSoonAt = options.nowMs() + delayMs;
        wake = true;
    }

    wakeUp.notify_all();
}

void WaveHardware::runLoop()
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

void WaveHardware::publish (const std::function<void (HardwareControls&)>& change)
{
    HardwareControls before, after;

    {
        const std::lock_guard<std::mutex> lock (mutex);
        before = published;
        change (published);
        published.error = juce::String::fromUTF8 ((actionError.empty() ? readError : actionError).c_str());
        after = published;
    }

    if (! sameControls (before, after) && options.onChanged)
        options.onChanged();
}

void WaveHardware::tick()
{
    const int64_t now = options.nowMs();

    bool permissionAsked = false;

    {
        const std::lock_guard<std::mutex> lock (mutex);
        permissionAsked = std::exchange (permissionRequested, false);

        if (std::exchange (newRequest, false))
        {
            // A new wish from the user (or the start-up restore): a fresh budget, and the last complaint is old news.
            writeTimes.clear();
            gaveUp = false;
            actionError.clear();
        }
    }

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

    converge (now);
}

void WaveHardware::probe()
{
    const auto found = usb->probe();
    const bool firstLook = ! presenceKnown;
    const bool changed = firstLook || found != presence;

    presence = found;
    presenceKnown = true;

    if (! changed)
        return;

    // Nobody needs a log line for a computer that has no Wave:3 on the USB bus (or cannot say: not Linux).
    const bool nothingToSay = firstLook && (found == wave::Presence::Absent || found == wave::Presence::Unsupported);

    if (! nothingToSay)
        logLine (std::string ("Wave:3 on USB: ") + presenceText (found));

    const bool there = found == wave::Presence::NoPermission || found == wave::Presence::Ready;

    if (found != wave::Presence::Ready)
        readError.clear();

    publish ([&] (HardwareControls& controls)
    {
        controls.usbAvailable = there;
        controls.usbPermission = found == wave::Presence::Ready;

        if (found != wave::Presence::Ready)
        {
            controls.clipGuard.reset();
            controls.gainDb.reset();
            controls.gainLocked.reset();
        }

        if (found == wave::Presence::Ready)
            actionError.clear();        // the permission is there now: whatever was said about it is done with
    });

    if (found == wave::Presence::Ready)
    {
        // It has just appeared (plugged in, or the permission arrived): ask what it is set to, and put back what the
        // user wants, since it forgets its settings when it loses power.
        readDue = true;
        gaveUp = false;
        writeTimes.clear();
        lastLoggedClipGuard.reset();
    }
}

void WaveHardware::readAndPublish()
{
    wave::ConfigBlock block {};

    if (! usb->readConfig (block))
    {
        readError = usb->lastErrorText();
        const auto detail = usb->lastErrorDetail();

        if (detail != lastLoggedError)
            logLine ("could not read the Wave:3: " + detail);

        lastLoggedError = detail;

        publish ([] (HardwareControls&) {});
        return;
    }

    const auto guard = wave::clipGuardOf (block);

    if (! guard.has_value())
    {
        readError = wave::describe (wave::Error::UnknownLayout);

        if (lastLoggedError != "layout")
            logLine ("the ClipGuard byte is not 0 or 1; block: " + wave::toHex (block));

        lastLoggedError = "layout";
        publish ([] (HardwareControls&) {});
        return;
    }

    readError.clear();
    lastLoggedError.clear();

    const auto gain = wave::gainDbOf (block);
    const auto gainLock = wave::gainLockOf (block);

    publish ([&] (HardwareControls& controls)
    {
        controls.clipGuard = guard;
        controls.gainDb = gain.has_value() ? std::optional<float> ((float) *gain) : std::nullopt;
        controls.gainLocked = gainLock;
    });

    if (lastLoggedClipGuard != guard)
    {
        lastLoggedClipGuard = guard;
        logLine (std::string ("Wave:3 ClipGuard is ") + (*guard ? "on" : "off") + ", mic gain "
                 + (gain.has_value() ? juce::String (*gain, 1).toStdString() + " dB" : std::string ("unknown"))
                 + ", gain lock " + (gainLock.has_value() ? (*gainLock ? "on" : "off") : "unknown"));
    }
}

// Puts the mic in the state the user wants, if it is not in it. Only ever runs when we know what the mic says.
void WaveHardware::converge (int64_t now)
{
    std::optional<bool> want;

    {
        const std::lock_guard<std::mutex> lock (mutex);
        want = desired;
    }

    const auto current = get().clipGuard;

    if (! want.has_value() || ! current.has_value() || *want == *current || gaveUp)
        return;

    while (! writeTimes.empty() && now - writeTimes.front() > kBudgetWindowMs)
        writeTimes.pop_front();

    if (writeTimes.size() >= kMaxWritesPerBudget)
    {
        gaveUp = true;
        actionError = "ClipGuard keeps switching back by itself, so Headroom left it alone.";
        logLine ("ClipGuard was set " + std::to_string (kMaxWritesPerBudget) + " times in the last 10 minutes and went back each time; not writing again until asked");
        publish ([] (HardwareControls&) {});
        return;
    }

    writeTimes.push_back (now);

    logLine (std::string ("setting ClipGuard ") + (*want ? "on" : "off") + " (the mic says " + (*current ? "on" : "off") + ")");
    publish ([&] (HardwareControls& controls) { controls.busy = true; controls.clipGuard = want; });

    const bool ok = usb->setClipGuard (*want);

    if (ok)
    {
        actionError.clear();
    }
    else
    {
        actionError = usb->lastErrorText();
        logLine ("could not set ClipGuard: " + usb->lastErrorDetail());
    }

    // Whatever happened, find out what the mic says now.
    readAndPublish();
    publish ([] (HardwareControls& controls) { controls.busy = false; });
}

void WaveHardware::handlePermissionRequest()
{
    actionError.clear();
    publish ([] (HardwareControls& controls) { controls.busy = true; });

    logLine ("asking for permission to control the Wave:3 (pkexec)");

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
            actionError = "It is set up, but the Wave:3 has to be unplugged and plugged back in once before it works.";
    }
    else
    {
        actionError = wave::permission::describe (result.outcome);
    }

    presenceKnown = false;          // look again right now
    publish ([] (HardwareControls& controls) { controls.busy = false; });
}
}
