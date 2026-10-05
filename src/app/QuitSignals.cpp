// QuitSignals: the SIGTERM / SIGINT handler (self-pipe), the thread that turns a signal into a quit request, and the grace clock.
#include "app/QuitSignals.h"

#include "app/Log.h"

#if JUCE_LINUX
 #include <atomic>
 #include <cerrno>
 #include <chrono>
 #include <csignal>
 #include <exception>
 #include <fcntl.h>
 #include <poll.h>
 #include <signal.h>
 #include <thread>
 #include <unistd.h>
#endif

namespace hr::QuitSignals
{
#if JUCE_LINUX
namespace
{
    // The write end of the pipe, for the handler (-1 = not installed). Lock-free so that a signal handler can read it.
    std::atomic<int> pipeWriteFd { -1 };
    static_assert (std::atomic<int>::is_always_lock_free);

    // Runs on whatever thread the signal arrived on, in the middle of anything: write() and nothing else (errno kept).
    void handleSignal (int signalNumber)
    {
        const int savedErrno = errno;
        const int fd = pipeWriteFd.load (std::memory_order_relaxed);

        if (fd >= 0)
        {
            const char byte = (char) signalNumber;

            // The pipe is non-blocking: when it is full a request is already waiting, so a failed write changes nothing.
            const auto written = ::write (fd, &byte, 1);
            (void) written;
        }

        errno = savedErrno;
    }

    void say (const char* text, size_t length)
    {
        const auto written = ::write (STDERR_FILENO, text, length);     // the journal; the log file may be the thing that hangs
        (void) written;
    }

    // The thread: waits for the first byte, posts the quit request to the message thread, then watches the clock.
    void watchForSignals (int readFd, std::function<void (int)> onSignal, int graceMs)
    {
        using Clock = std::chrono::steady_clock;

        bool requested = false;
        int requestedSignal = SIGTERM;
        Clock::time_point deadline;

        for (;;)
        {
            pollfd waiting {};
            waiting.fd = readFd;
            waiting.events = POLLIN;

            // Nothing to do until a signal comes; once one has, look at the clock now and then.
            const int ready = ::poll (&waiting, 1, requested ? 200 : -1);

            if (ready > 0)
            {
                if ((waiting.revents & POLLIN) == 0)
                {
                    // The pipe is broken (cannot happen: both ends are ours). Do not leave signals swallowed with nobody
                    // listening: give them their normal meaning back.
                    for (const int signalNumber : { SIGTERM, SIGINT })
                        ::signal (signalNumber, SIG_DFL);

                    return;
                }

                char bytes[16];
                const auto count = ::read (readFd, bytes, sizeof (bytes));

                if (count > 0 && ! requested)
                {
                    requested = true;
                    requestedSignal = (int) bytes[0];
                    deadline = Clock::now() + std::chrono::milliseconds (graceMs);

                    const int signalNumber = requestedSignal;
                    juce::MessageManager::callAsync ([onSignal, signalNumber] { onSignal (signalNumber); });
                }
            }

            if (requested && Clock::now() >= deadline)
            {
                static const char message[] = "Headroom: the orderly quit after the signal took too long, ending the process.\n";
                say (message, sizeof (message) - 1);

                ::signal (requestedSignal, SIG_DFL);
                ::raise (requestedSignal);
            }
        }
    }
}
#endif

void install (std::function<void (int)> onSignal, int graceMs)
{
   #if JUCE_LINUX
    if (pipeWriteFd.load() >= 0)
        return;     // once

    int fds[2] = { -1, -1 };

    // Close-on-exec: the scanner processes must not inherit it. Non-blocking: the handler must never wait.
    if (::pipe2 (fds, O_CLOEXEC | O_NONBLOCK) != 0)
    {
        Log::warn ("Could not set up the SIGTERM handler; Headroom will not save on SIGTERM");
        return;
    }

    try
    {
        std::thread (watchForSignals, fds[0], std::move (onSignal), graceMs).detach();
    }
    catch (const std::exception&)
    {
        ::close (fds[0]);
        ::close (fds[1]);
        Log::warn ("Could not start the SIGTERM thread; Headroom will not save on SIGTERM");
        return;
    }

    pipeWriteFd.store (fds[1]);

    struct sigaction action {};
    action.sa_handler = handleSignal;
    sigemptyset (&action.sa_mask);
    action.sa_flags = SA_RESTART;       // not an excuse for EINTR in code that never expected one

    for (const int signalNumber : { SIGTERM, SIGINT })
        sigaction (signalNumber, &action, nullptr);
   #else
    juce::ignoreUnused (onSignal, graceMs);
   #endif
}
}
