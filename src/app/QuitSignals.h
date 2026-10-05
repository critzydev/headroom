// QuitSignals: SIGTERM and SIGINT mean "save and quit". Linux only; a no-op elsewhere.
#pragma once

#include "Juce.h"

#include <functional>

namespace hr::QuitSignals
{
/** The orderly quit gets this long after the first signal. If the process is still there then (a hung plug-in on
    shutdown, a stuck message thread), the signal does what it would have done without us: it ends the process. */
constexpr int kDefaultGraceMs = 10000;

/**
    Linux, the primary instance only, once, on the message thread (after the event loop exists):

    From now on SIGTERM (what `systemctl --user stop` sends at logout, shutdown and from install.sh) and SIGINT do not
    kill the process: `onSignal (signalNumber)` is called on the message thread, once, for the first of them, and is
    meant to start the normal quit (`JUCEApplicationBase::quit()`: state saved, backend stopped, status 0). Later
    signals are ignored while that is going on.

    The handler itself only does what a signal handler may: it writes one byte to a pipe. A small thread reads the pipe
    and posts `onSignal` to the message thread, and keeps the clock for the grace period above.

    Nothing else is touched: no signal mask is changed and the pipe is close-on-exec, and a handled signal goes back to
    its default action across exec(), so the plug-in scanner processes (which are started with exec) are unaffected. The
    crash guard's handlers (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT) are other signals.
*/
void install (std::function<void (int signalNumber)> onSignal, int graceMs = kDefaultGraceMs);
}
