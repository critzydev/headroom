// SingleInstance: elects the one primary Headroom per user and lets a second launch talk to it.
#pragma once

#include "Juce.h"

#include <functional>
#include <memory>

namespace hr
{
// JUCE's anotherInstanceStarted never fires on Linux, so the app does this itself:
//
//  * Election: a juce::InterProcessLock named "headroom-<user>" (per user), held for the life of the primary. The OS drops it
//    when the process dies however it dies (a POSIX record lock on Linux, a named mutex on Windows), so a crashed primary
//    never blocks the next start.
//  * Commands: the primary listens on a juce::NamedPipe on a background thread and answers one-line requests from any number
//    of secondaries, one after another: "show", "quit", "setup" and "ping" (-> "pong"). Every request is acknowledged, so a sender
//    knows the primary is alive and listening. The handlers run on the message thread, never on the listener.
//
// The pipe is a single channel, so secondaries take a second lock ("headroom-<user>-client") while they talk; that keeps
// replies from being mixed up. A pipe or lock left behind by a killed primary is simply reused by the next one.
class SingleInstance
{
public:
    // `name` decides which lock and pipe are used. Only the tests pass one; the app uses the per-user default.
    explicit SingleInstance (const juce::String& name = defaultName());
    ~SingleInstance();

    // "headroom-<user>", from the account (not from $USER, which the systemd unit and a menu launch may not agree on).
    // HEADROOM_INSTANCE=<word> in the environment adds "-<word>": a dev run or a test gets an instance of its own next to
    // the real one.
    static juce::String defaultName();

    // Try to become the primary: takes the lock and starts listening. True if this process is the primary (also when it
    // already was). False if another process holds the lock. Call on the message thread.
    bool tryBecomePrimary();
    bool isPrimary() const noexcept;

    // Primary: stop answering commands but keep the lock (a shutdown that is still saving). The pipe is gone at once, so a
    // secondary that comes now waits for the lock to go instead of being acknowledged by a process that is about to exit.
    void stopListening();

    // Is some other process the primary right now? Looks at the lock only and never keeps it. A process that is itself
    // the primary answers true. Do not use it to probe a lock this process holds through another SingleInstance.
    bool isPrimaryRunning();

    // Primary only: called on the message thread when a secondary sent "show" / "quit".
    std::function<void()> onShowRequested;
    std::function<void()> onQuitRequested;
    std::function<void()> onSetupRequested;     // "setup": show the window with the setup wizard (headroom --setup while Headroom runs)

    // Secondary: sends one command and waits for the answer ("ok", "pong", "unknown"). Retries connecting until the timeout, so
    // a primary that is still starting up is found. Returns "" if nothing answered in time.
    juce::String sendCommand (const juce::String& command, int timeoutMs);

    // Secondary: ask the primary to show its window. True once it acknowledged.
    bool sendShowToPrimary (int timeoutMs = 2000);

    // For tests: what juce::NamedPipe is opened with for `name` (to be able to misbehave on the channel), and deleting the
    // lock and pipe files that a killed primary leaves behind on Linux (the OS cleans up everything else).
    static juce::String pipeNameFor (const juce::String& name);
    static void removeLeftovers (const juce::String& name);

private:
    class Listener;

    juce::String handleCommand (const juce::String& command);      // listener thread: the answer only, no side effects
    void afterReply (const juce::String& command);                 // listener thread: acts once the answer is written
    void postToMessageThread (const juce::String& command);

    JUCE_DECLARE_WEAK_REFERENCEABLE (SingleInstance)

    const juce::String lockName, clientLockName, pipeName;
    juce::WeakReference<SingleInstance> weakSelf;

    // Declared in this order so the listener stops before the lock is given up.
    std::unique_ptr<juce::InterProcessLock> lock;
    std::unique_ptr<Listener> listener;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SingleInstance)
};
}
