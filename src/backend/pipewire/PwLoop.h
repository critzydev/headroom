// RAII owner of the PipeWire thread loop, context and core connection, with reconnect + backoff.
#pragma once

#include "PwCommon.h"

namespace hr::pw
{

/**
    Owns the pw_thread_loop, pw_context and pw_core. Connecting happens on the loop thread;
    if the daemon isn't there yet (or dies later) it keeps retrying with 0.5 s, 1 s, 2 s, then
    5 s pauses. Callbacks run on the loop thread with the loop lock held.
*/
class PwLoop
{
public:
    PwLoop();
    ~PwLoop();

    PwLoop (const PwLoop&) = delete;
    PwLoop& operator= (const PwLoop&) = delete;

    /** Creates the loop and its thread. The connection itself is made asynchronously. */
    bool start();

    /** Tears everything down and joins the thread. Safe to call more than once. */
    void stop();

    pw_thread_loop* threadLoop() const noexcept { return threadLoopPtr; }
    pw_loop* loop() const noexcept;
    pw_core* core() const noexcept { return corePtr; }
    bool isConnected() const noexcept { return corePtr != nullptr; }
    bool isRunning() const noexcept { return threadLoopPtr != nullptr; }

    /** The core just connected. Build the graph here. */
    std::function<void()> onConnected;

    /** The core is going away (daemon died, or we are stopping). The core is still valid:
        destroy every proxy and the filter here, in dependency order. */
    std::function<void()> onDisconnecting;

    /** Round trip to the daemon. onSyncDone then fires (loop thread) once the daemon has delivered everything it had
        queued before it saw the request: after a fresh registry, the whole initial list of objects. Loop thread only. */
    void requestSync();
    std::function<void()> onSyncDone;

private:
    void tryConnect();
    void handleLost();
    void scheduleReconnect (int delayMs);
    void disconnectCore();

    static void onCoreError (void* data, uint32_t id, int seq, int res, const char* message);
    static void onCoreDone (void* data, uint32_t id, int seq);

    pw_thread_loop* threadLoopPtr = nullptr;
    pw_context* contextPtr = nullptr;
    pw_core* corePtr = nullptr;

    spa_hook coreHook {};
    bool coreHookActive = false;

    std::unique_ptr<LoopTimer> connectTimer;
    std::unique_ptr<LoopTimer> lostTimer;

    int reconnectAttempt = 0;
    int pendingSync = -1;
    bool stopping = false;
    bool initialised = false;
};

} // namespace hr::pw
