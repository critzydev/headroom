// WaveHardware: the Wave:3's USB side as seen by a backend. One worker thread owns every USB transfer, the
// permission request and the "keep ClipGuard where the user put it" logic, so the message thread and the PipeWire
// loop never wait for the device. Independent of PipeWire: PipeWireBackend owns one, tests drive tick() by hand.
#pragma once

#include "backend/BackendTypes.h"
#include "backend/wave/WavePermission.h"
#include "backend/wave/WaveUsb.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace hr
{
class WaveHardware
{
public:
    struct Options
    {
        std::unique_ptr<wave::WaveUsb> usb;             // null = the real one
        // Runs the password box and the install script. null = pkexec /bin/sh with the constant script. `abort` turns true when we are quitting.
        std::function<wave::permission::InstallResult (const std::function<bool()>& abort)> installPermission;
        std::function<void()> onChanged;                // the state changed; called on the worker thread
        std::function<int64_t()> nowMs;                 // null = steady clock
        std::function<void (int)> sleepMs;              // null = sleep
        int probeIntervalMs = 2000;                     // is a Wave:3 there / may we open it (sysfs only, no USB traffic)
        int readIntervalMs = 10000;                     // how often the mic is asked what it is set to
    };

    explicit WaveHardware (Options options);
    ~WaveHardware();

    void start();                                       // starts the worker thread
    void stop();                                        // joins it; also aborts a password box that is open

    // Any thread. usbAvailable here means "a Wave:3 is on the bus"; the backend narrows it to the selected input.
    HardwareControls get() const;

    // The ClipGuard state to keep the mic in. Applied as soon as possible, and again if the mic falls back (it forgets
    // its settings when it loses power). Calling it again starts a new budget of writes.
    void setClipGuard (bool on);

    void requestPermission();                           // "Allow hardware controls": password box, then the udev rule
    void readSoon (int delayMs = 0);                    // ask the mic what it is set to (its gain, after we changed it)

    // One step of the worker: everything that is due. Public so tests can run it without a thread.
    void tick();

private:
    void runLoop();
    void probe();
    void readAndPublish();
    void converge (int64_t now);
    void handlePermissionRequest();
    void publish (const std::function<void (HardwareControls&)>& change);

    Options options;
    std::unique_ptr<wave::WaveUsb> usb;

    mutable std::mutex mutex;                           // guards everything below
    HardwareControls published;
    std::optional<bool> desired;
    bool permissionRequested = false;
    bool newRequest = false;                            // setClipGuard() was called since the worker last looked
    std::optional<int64_t> readSoonAt;
    bool stopping = false;
    bool wake = false;
    std::condition_variable wakeUp;

    // Worker only.
    wave::Presence presence = wave::Presence::Absent;
    bool presenceKnown = false;
    int64_t nextProbeAt = 0, nextReadAt = 0;
    bool readDue = false;
    std::deque<int64_t> writeTimes;                     // the writes of the current budget
    bool gaveUp = false;
    std::optional<bool> lastLoggedClipGuard;
    std::string lastLoggedError;
    std::string readError, actionError;                 // what the last read / the last change or permission request said

    std::thread worker;
};
}
