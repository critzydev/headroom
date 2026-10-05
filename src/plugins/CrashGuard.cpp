// CrashGuard: crash.txt record format, reading it back, and (Linux) the fatal-signal handlers.
#include "plugins/CrashGuard.h"

#include "app/Log.h"

#include <algorithm>

#if JUCE_LINUX
 #include <csignal>
 #include <fcntl.h>
 #include <signal.h>
 #include <unistd.h>
#endif

namespace hr::CrashGuard
{
size_t formatCrashRecord (char* out, uint32_t rtSlot, uint32_t uiSlot) noexcept
{
    size_t length = 0;

    const auto putNumber = [&] (uint32_t value) noexcept
    {
        char digits[10];        // uint32_t needs at most 10
        int count = 0;

        do
        {
            digits[count++] = (char) ('0' + value % 10);
            value /= 10;
        }
        while (value != 0);

        while (count > 0)
            out[length++] = digits[--count];
    };

    putNumber (rtSlot);
    out[length++] = ' ';
    putNumber (uiSlot);
    out[length++] = '\n';
    return length;
}

namespace
{
  #if JUCE_LINUX
    // The file descriptor the handler writes to (-1 = handlers not installed) and the file it belongs to.
    std::atomic<int> crashFd { -1 };
    static_assert (std::atomic<int>::is_always_lock_free);

    juce::String& installedPath()
    {
        static juce::String path;
        return path;
    }
  #endif

    std::vector<uint32_t> parseCrashText (const juce::String& text)
    {
        std::vector<uint32_t> ids;

        auto tokens = juce::StringArray::fromTokens (text, " \t\r\n", "");
        tokens.removeEmptyStrings();

        for (const auto& token : tokens)
        {
            const auto id = (uint32_t) token.getLargeIntValue();

            if (id != 0 && std::find (ids.begin(), ids.end(), id) == ids.end())
                ids.push_back (id);
        }

        return ids;
    }

  #if JUCE_LINUX
    // Runs on whatever thread crashed, possibly with a wrecked heap: syscalls and atomics only. No allocation, no
    // JUCE, no stdio, no snprintf.
    void handleFatalSignal (int signalNumber)
    {
        const int fd = crashFd.load (std::memory_order_relaxed);

        if (fd >= 0)
        {
            char record[kMaxRecordChars];
            const auto length = formatCrashRecord (record,
                                                   currentRtSlot.load (std::memory_order_relaxed),
                                                   currentUiSlot.load (std::memory_order_relaxed));

            // Nothing sensible to do if this fails: we are dying anyway.
            if (::write (fd, record, length) < 0)
                crashFd.store (-1, std::memory_order_relaxed);
        }

        // Back to the default action and raise again, so the exit status, systemd and any core dump still show
        // the real signal.
        ::signal (signalNumber, SIG_DFL);
        ::raise (signalNumber);
    }
  #endif
}

void install (const juce::File& crashFile)
{
   #if JUCE_LINUX
    crashFile.getParentDirectory().createDirectory();

    // No O_TRUNC: the previous run's record stays until readAndClearLastCrash has read it. The AppController does
    // that a moment after install() and empties the file in place, which is why this descriptor stays valid.
    const int fd = ::open (crashFile.getFullPathName().toRawUTF8(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);

    if (fd < 0)
    {
        Log::warn ("Crash guard: cannot open " + crashFile.getFullPathName() + ", crashes will not be attributed to plugins");
        return;
    }

    const int previousFd = crashFd.exchange (fd, std::memory_order_relaxed);

    if (previousFd >= 0)
        ::close (previousFd);

    installedPath() = crashFile.getFullPathName();

    struct sigaction action {};
    action.sa_handler = handleFatalSignal;
    sigemptyset (&action.sa_mask);
    action.sa_flags = SA_NODEFER | SA_RESETHAND;    // a fault inside the handler itself just kills us

    for (const int signalNumber : { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT })
        sigaction (signalNumber, &action, nullptr);
   #else
    juce::ignoreUnused (crashFile);
   #endif
}

std::vector<uint32_t> readAndClearLastCrash (const juce::File& crashFile)
{
    if (! crashFile.existsAsFile())
        return {};

    const auto ids = parseCrashText (crashFile.loadFileAsString());

   #if JUCE_LINUX
    // Installed for this very file: empty it in place. Deleting it would leave the handler writing to an unlinked inode.
    if (crashFd.load (std::memory_order_relaxed) >= 0 && installedPath() == crashFile.getFullPathName())
    {
        juce::FileOutputStream stream (crashFile);

        if (stream.openedOk())
        {
            stream.setPosition (0);
            stream.truncate();
        }

        return ids;
    }
   #endif

    crashFile.deleteFile();
    return ids;
}
}
