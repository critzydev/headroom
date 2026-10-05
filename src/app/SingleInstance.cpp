// SingleInstance: per-user primary election (a lock) plus a small command channel (a named pipe), see SingleInstance.h.
#include "app/SingleInstance.h"

#include <string>

#if JUCE_LINUX
 #include <pwd.h>
 #include <sys/stat.h>
 #include <unistd.h>
#endif

namespace hr
{
namespace
{
    // Every line on the pipe, in both directions, is "\nHR1 <nonce> <word>\n". The leading newline ends whatever half-line
    // a crashed sender left behind, and the prefix lets a reader skip stray bytes (JUCE leaves a NUL in the answer
    // FIFO on Linux when a client closes its end). The nonce ties an answer to the request it belongs to.
    constexpr const char* kPrefix = "HR1 ";
    constexpr int kReadPollMs = 200;        // the listener looks at its stop flag this often
    constexpr int kReplyTimeoutMs = 500;    // what a sender that left without reading its answer costs the listener
    constexpr int kMaxLineBytes = 256;
    [[maybe_unused]] constexpr int kPipeCheckMs = 2000;      // Linux: how often the listener checks that its FIFOs still exist

    juce::String legalPart (const juce::String& text)
    {
        const auto legal = text.retainCharacters ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-");
        return legal.isNotEmpty() ? legal : juce::String ("user");
    }

    juce::String accountName()
    {
       #if JUCE_LINUX
        // From the passwd entry of our uid: $USER may be missing or different in a systemd user unit, and two processes
        // of one account that disagree on the name would each think they are the only Headroom.
        struct passwd entry {};
        struct passwd* found = nullptr;
        char buffer[1024];

        if (::getpwuid_r (::getuid(), &entry, buffer, sizeof (buffer), &found) == 0 && found != nullptr && entry.pw_name != nullptr)
            return legalPart (juce::String::fromUTF8 (entry.pw_name));

        return "uid" + juce::String ((int) ::getuid());
       #else
        return legalPart (juce::SystemStats::getLogonName());
       #endif
    }

   #if JUCE_LINUX
    // The user's runtime directory is private to them and empty after a reboot. /tmp is the last resort; the name is a
    // fixed string, not TMPDIR, so every process of the account looks in the same place.
    juce::File runtimeDirectory()
    {
        const auto fromEnvironment = juce::SystemStats::getEnvironmentVariable ("XDG_RUNTIME_DIR", {});

        if (fromEnvironment.isNotEmpty() && juce::File::isAbsolutePath (fromEnvironment) && juce::File (fromEnvironment).isDirectory())
            return juce::File (fromEnvironment);

        const juce::File standard ("/run/user/" + juce::String ((int) ::getuid()));

        if (standard.isDirectory())
            return standard;

        return juce::File ("/tmp");
    }
   #endif

    // What juce::NamedPipe is given. Windows: a bare name (it lives under \\.\pipe\). Linux: an absolute path in the runtime
    // directory; JUCE adds "_in" and "_out" for the two FIFOs.
    juce::String makePipeName (const juce::String& name)
    {
       #if JUCE_LINUX
        return runtimeDirectory().getChildFile (name + "-" + juce::String ((int) ::getuid())).getFullPathName();
       #else
        return name;
       #endif
    }

    bool isNonce (const juce::String& text)
    {
        return text.length() >= 1 && text.length() <= 32 && text.containsOnly ("0123456789abcdefABCDEF");
    }

    // "\nHR1 <nonce> <word>\n"
    juce::String makeLine (const juce::String& nonce, const juce::String& word)
    {
        return juce::String ("\n") + kPrefix + nonce + " " + word + "\n";
    }

    // The words of one received line ("HR1 <nonce> <word>", possibly with junk in front): {nonce, word}, or nothing.
    juce::StringArray parseLine (const std::string& line)
    {
        const auto text = juce::String::fromUTF8 (line.data(), (int) line.size()).fromFirstOccurrenceOf (kPrefix, false, false).trim();
        auto words = juce::StringArray::fromTokens (text, " ", {});

        if (words.size() != 2 || ! isNonce (words[0]))
            words.clear();

        return words;
    }

    // Grows `line` one byte at a time. True when a whole line has arrived. Over-long junk is dropped up to its newline.
    bool collect (std::string& line, bool& discarding, char byte)
    {
        if (byte == '\n')
        {
            const bool wholeLine = ! discarding;
            discarding = false;
            return wholeLine;
        }

        if (discarding)
            return false;

        if ((int) line.size() >= kMaxLineBytes)
        {
            line.clear();
            discarding = true;
            return false;
        }

        line.push_back (byte);
        return false;
    }
}

// =================================================================================================
// The listener thread of the primary

class SingleInstance::Listener final : public juce::Thread
{
public:
    Listener (SingleInstance& ownerToUse, const juce::String& pipeNameToUse)
        : juce::Thread ("Headroom single instance"), owner (ownerToUse), pipeName (pipeNameToUse)
    {
    }

    ~Listener() override
    {
        signalThreadShouldExit();
        pipe.close();               // wakes a read or write that is waiting; NamedPipe::close is meant to be called from another thread
        stopThread (3000);
    }

    // Creates the pipe before the thread starts, so it is there the moment the election is won. Failing is not fatal:
    // run() keeps trying.
    void open()
    {
        openPipe();
    }

    void run() override
    {
        std::string line;
        bool discarding = false;
        int quickFailures = 0;
        auto lastPipeCheck = juce::Time::getMillisecondCounter();

        while (! threadShouldExit())
        {
            if (! pipe.isOpen() || pipeFilesGone (lastPipeCheck))
            {
                if (! openPipe())
                {
                    wait (1000.0);
                    continue;
                }

                line.clear();
                discarding = false;
            }

            char byte = 0;
            const auto started = juce::Time::getMillisecondCounter();

            // One byte per read: on Linux JUCE waits until the buffer is full, on Windows it returns what is there.
            if (pipe.read (&byte, 1, kReadPollMs) == 1)
            {
                quickFailures = 0;

                if (collect (line, discarding, byte))
                    handle (line);

                if (byte == '\n')
                    line.clear();
            }
            else if (juce::Time::getMillisecondCounter() - started >= 20)
            {
                quickFailures = 0;      // nobody wrote for a while: the normal idle case
            }
            else if (++quickFailures >= 3)
            {
                // A read that fails at once means the pipe is broken. On Windows JUCE's pipe gets stuck like that for good
                // when a client connects and leaves before we listen, so start over with a new one.
                pipe.close();
                quickFailures = 0;
            }
            else
            {
                wait (20.0);            // and never a busy loop
            }
        }
    }

private:
    bool openPipe()
    {
        // Not "must not exist": the lock decides who is primary, and the FIFOs a killed primary left are simply reused.
        if (! pipe.createNewPipe (pipeName, false))
            return false;

       #if JUCE_LINUX
        // Only this user may talk to us, whatever the umask says.
        ::chmod ((pipeName + "_in").toRawUTF8(), 0600);
        ::chmod ((pipeName + "_out").toRawUTF8(), 0600);

        // Requests that piled up while nobody was listening (an old "quit", say) must not run now.
        char discard = 0;

        while (pipe.read (&discard, 1, 0) == 1)
        {
        }
       #endif

        return true;
    }

    // Linux: /tmp cleaners and helpful users can delete a FIFO from under a running primary; then nobody could reach us.
    bool pipeFilesGone ([[maybe_unused]] juce::uint32& lastCheck)
    {
       #if JUCE_LINUX
        const auto now = juce::Time::getMillisecondCounter();

        if (now - lastCheck < (juce::uint32) kPipeCheckMs)
            return false;

        lastCheck = now;
        return ! juce::File (pipeName + "_in").exists() || ! juce::File (pipeName + "_out").exists();
       #else
        return false;
       #endif
    }

    void handle (const std::string& line)
    {
        const auto words = parseLine (line);

        if (words.isEmpty())
            return;         // not ours, or damaged

        const auto command = words[1].toLowerCase();
        const auto reply = makeLine (words[0], owner.handleCommand (command));
        pipe.write (reply.toRawUTF8(), (int) reply.getNumBytesAsUTF8(), kReplyTimeoutMs);

        // Only now: a "quit" handled before its answer was written could end the process first, and the sender would
        // see no answer at all.
        owner.afterReply (command);
    }

    SingleInstance& owner;
    const juce::String pipeName;
    juce::NamedPipe pipe;
};

// =================================================================================================

SingleInstance::SingleInstance (const juce::String& name)
    : lockName (legalPart (name)),
      clientLockName (legalPart (name) + "-client"),
      pipeName (makePipeName (legalPart (name))),
      weakSelf (this)
{
}

SingleInstance::~SingleInstance()
{
    listener.reset();       // joins the thread and removes the pipe before...
    lock.reset();           // ...the lock is given up
}

juce::String SingleInstance::defaultName()
{
    const auto word = juce::SystemStats::getEnvironmentVariable ("HEADROOM_INSTANCE", {});
    return "headroom-" + accountName() + (word.isNotEmpty() ? "-" + legalPart (word) : juce::String());
}

bool SingleInstance::tryBecomePrimary()
{
    if (isPrimary())
        return true;

    auto candidate = std::make_unique<juce::InterProcessLock> (lockName);

    if (! candidate->enter (0))
        return false;

    lock = std::move (candidate);
    listener = std::make_unique<Listener> (*this, pipeName);
    listener->open();
    listener->startThread();
    return true;
}

bool SingleInstance::isPrimary() const noexcept
{
    return lock != nullptr;
}

void SingleInstance::stopListening()
{
    listener.reset();
}

bool SingleInstance::isPrimaryRunning()
{
    // Asking our own lock again would succeed on Linux (record locks belong to the process) and, worse, closing the probe
    // would release the real one.
    if (isPrimary())
        return true;

    juce::InterProcessLock probe (lockName);

    if (! probe.enter (0))
        return true;

    probe.exit();
    return false;
}

// -------------------------------------------------------------------------------------------------
// Primary side

juce::String SingleInstance::handleCommand (const juce::String& command)
{
    if (command == "ping")
        return "pong";

    if (command == "show" || command == "quit" || command == "setup")
        return "ok";

    return "unknown";
}

void SingleInstance::afterReply (const juce::String& command)
{
    if (command == "show" || command == "quit" || command == "setup")
        postToMessageThread (command);
}

void SingleInstance::postToMessageThread (const juce::String& command)
{
    // The answer has already been sent by the time this runs: the message thread may be busy, and the sender must not wait for it.
    juce::MessageManager::callAsync ([self = weakSelf, command]
    {
        if (auto* instance = self.get())
        {
            const auto& handler = command == "show" ? instance->onShowRequested
                                : command == "setup" ? instance->onSetupRequested
                                                     : instance->onQuitRequested;

            if (handler)
                handler();
        }
    });
}

// -------------------------------------------------------------------------------------------------
// Secondary side

juce::String SingleInstance::sendCommand (const juce::String& command, int timeoutMs)
{
    const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) juce::jmax (0, timeoutMs);

    const auto remaining = [deadline]
    {
        const auto now = juce::Time::getMillisecondCounter();
        return now >= deadline ? 0 : (int) (deadline - now);
    };

    // One secondary at a time: the pipe is a single channel and the answers would otherwise get mixed up.
    juce::InterProcessLock clientLock (clientLockName);

    if (! clientLock.enter (remaining()))
        return {};

    const juce::ScopeGuard leave { [&clientLock] { clientLock.exit(); } };

    juce::NamedPipe pipe;

    while (! pipe.openExisting (pipeName))
    {
        if (remaining() == 0)
            return {};

        juce::Thread::sleep (20);
    }

    const auto nonce = juce::Uuid().toString().substring (0, 12);
    const auto request = makeLine (nonce, command);
    const auto requestBytes = (int) request.getNumBytesAsUTF8();

    if (pipe.write (request.toRawUTF8(), requestBytes, juce::jmax (50, remaining())) != requestBytes)
        return {};

    std::string line;
    bool discarding = false;

    while (remaining() > 0)
    {
        char byte = 0;

        if (pipe.read (&byte, 1, remaining()) != 1)
            break;

        if (collect (line, discarding, byte))
        {
            const auto words = parseLine (line);

            if (words.size() == 2 && words[0] == nonce)
                return words[1];            // (an answer with another nonce is a leftover from an earlier sender)
        }

        if (byte == '\n')
            line.clear();
    }

    return {};
}

bool SingleInstance::sendShowToPrimary (int timeoutMs)
{
    return sendCommand ("show", timeoutMs) == "ok";
}

juce::String SingleInstance::pipeNameFor (const juce::String& name)
{
    return makePipeName (legalPart (name));
}

void SingleInstance::removeLeftovers (const juce::String& name)
{
    const auto legal = legalPart (name);
    const auto pipe = makePipeName (legal);

   #if JUCE_LINUX
    juce::File (pipe + "_in").deleteFile();
    juce::File (pipe + "_out").deleteFile();
    juce::File ("/var/tmp").getChildFile (legal).deleteFile();
    juce::File ("/var/tmp").getChildFile (legal + "-client").deleteFile();
   #else
    juce::ignoreUnused (pipe);
   #endif
}
}
