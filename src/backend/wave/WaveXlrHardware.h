// WaveXlrHardware: the Wave XLR's USB side as seen by a backend, EXPERIMENTAL. One worker thread owns every USB
// transfer and the permission request, so the message thread and the PipeWire loop never wait for the device. It does nothing
// at all (not even a look in /sys) until the user has opted in (setEnabled). The one thing it does is the mic gain, which the
// kernel does not offer for this model: it reads it (when the mic appears and every 10 s) and sets it once when asked.
// A wish is applied once, not held: the unit has a gain dial, and Headroom does not fight it.
// Independent of PipeWire: PipeWireBackend owns one, tests drive tick() by hand.
#pragma once

#include "backend/BackendTypes.h"
#include "backend/wave/WavePermission.h"
#include "backend/wave/WaveXlrUsb.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace hr
{
class WaveXlrHardware
{
public:
    struct Options
    {
        std::unique_ptr<wave::xlr::XlrUsb> usb;         // null = the real one
        // Runs the password box and the install script. null = pkexec /bin/sh with the constant script that includes the
        // experimental models. `abort` turns true when we are quitting.
        std::function<wave::permission::InstallResult (const std::function<bool()>& abort)> installPermission;
        std::function<void()> onChanged;                // the state changed; called on the worker thread
        std::function<int64_t()> nowMs;                 // null = steady clock
        std::function<void (int)> sleepMs;              // null = sleep
        int probeIntervalMs = 2000;                     // is a Wave XLR there / may we open it (sysfs only, no USB traffic)
        int readIntervalMs = 10000;                     // how often the mic is asked what its gain is
    };

    explicit WaveXlrHardware (Options options);
    ~WaveXlrHardware();

    void start();                                       // starts the worker thread
    void stop();                                        // joins it; also aborts a password box that is open

    // Any thread. The opt-in: off (the default) means no look at the bus and no transfer, whatever else is asked.
    void setEnabled (bool on);
    bool isEnabled() const;

    // Any thread. `model` is left empty here; the backend fills it for the selected input.
    ExperimentalControls get() const;

    // The gain to put the mic at, in dB (0 ... 75, quantised to 0.5). Applied once, as soon as the mic has been read.
    void setGainDb (double db);
    void requestPermission();                           // "Allow hardware controls": password box, then the udev rule
    void readSoon (int delayMs = 0);

    // One step of the worker: everything that is due. Public so tests can run it without a thread.
    void tick();

private:
    void runLoop();
    void probe();
    void readAndPublish();
    void applyWish();
    void handlePermissionRequest();
    void publish (const std::function<void (ExperimentalControls&)>& change);

    Options options;
    std::unique_ptr<wave::xlr::XlrUsb> usb;

    mutable std::mutex mutex;                           // guards everything below
    ExperimentalControls published;
    bool enabled = false;
    bool enabledChanged = false;
    std::optional<double> wish;
    int wishAttempts = 0;
    bool permissionRequested = false;
    std::optional<int64_t> readSoonAt;
    bool stopping = false;
    bool wake = false;
    std::condition_variable wakeUp;

    // Worker only.
    wave::Presence presence = wave::Presence::Absent;
    bool presenceKnown = false;
    int64_t nextProbeAt = 0, nextReadAt = 0;
    bool readDue = false;
    std::optional<float> lastLoggedGain;
    std::string lastLoggedError;
    std::string readError, actionError;                 // what the last read / the last change or permission request said

    std::thread worker;
};
}
