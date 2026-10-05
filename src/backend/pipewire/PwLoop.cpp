// Implementation of PwLoop: thread loop lifetime, connecting and reconnecting to the daemon.
#include "PwLoop.h"

namespace hr::pw
{

PwLoop::PwLoop() = default;

PwLoop::~PwLoop()
{
    stop();
}

pw_loop* PwLoop::loop() const noexcept
{
    return threadLoopPtr != nullptr ? pw_thread_loop_get_loop (threadLoopPtr) : nullptr;
}

bool PwLoop::start()
{
    if (threadLoopPtr != nullptr)
        return true;

    pw_init (nullptr, nullptr);
    initialised = true;

    threadLoopPtr = pw_thread_loop_new ("headroom-pw", nullptr);

    if (threadLoopPtr == nullptr)
    {
        pwLog ("could not create the thread loop");
        return false;
    }

    contextPtr = pw_context_new (pw_thread_loop_get_loop (threadLoopPtr), nullptr, 0);

    if (contextPtr == nullptr)
    {
        pwLog ("could not create the context");
        pw_thread_loop_destroy (threadLoopPtr);
        threadLoopPtr = nullptr;
        return false;
    }

    stopping = false;
    connectTimer = std::make_unique<LoopTimer> (pw_thread_loop_get_loop (threadLoopPtr), [this] { tryConnect(); });
    lostTimer = std::make_unique<LoopTimer> (pw_thread_loop_get_loop (threadLoopPtr), [this] { handleLost(); });

    if (pw_thread_loop_start (threadLoopPtr) < 0)
    {
        pwLog ("could not start the thread loop");
        connectTimer.reset();
        lostTimer.reset();
        pw_context_destroy (contextPtr);
        pw_thread_loop_destroy (threadLoopPtr);
        contextPtr = nullptr;
        threadLoopPtr = nullptr;
        return false;
    }

    LoopLock lock (threadLoopPtr);
    connectTimer->startOnce (1);
    return true;
}

void PwLoop::stop()
{
    if (threadLoopPtr == nullptr)
        return;

    {
        LoopLock lock (threadLoopPtr);
        stopping = true;

        if (connectTimer != nullptr)
            connectTimer->stop();

        if (lostTimer != nullptr)
            lostTimer->stop();

        disconnectCore();
        connectTimer.reset();
        lostTimer.reset();
    }

    pw_thread_loop_stop (threadLoopPtr);

    if (contextPtr != nullptr)
        pw_context_destroy (contextPtr);

    pw_thread_loop_destroy (threadLoopPtr);

    contextPtr = nullptr;
    threadLoopPtr = nullptr;

    if (initialised)
    {
        pw_deinit();
        initialised = false;
    }
}

void PwLoop::tryConnect()
{
    if (stopping || corePtr != nullptr || contextPtr == nullptr)
        return;

    Props clientProps;
    clientProps.set (PW_KEY_APP_NAME, "Headroom");
    clientProps.set (PW_KEY_APP_ICON_NAME, "headroom");

    corePtr = pw_context_connect (contextPtr, clientProps.release(), 0);

    if (corePtr == nullptr)
    {
        const int error = errno;

        if (reconnectAttempt == 0)
            pwLog ("PipeWire is not reachable yet (%s), will keep trying", std::strerror (error));

        const int delays[] = { 500, 1000, 2000, 5000 };
        const int delay = delays[juce::jmin (reconnectAttempt, 3)];
        ++reconnectAttempt;
        scheduleReconnect (delay);
        return;
    }

    static const pw_core_events coreEvents = []
    {
        pw_core_events e {};
        e.version = PW_VERSION_CORE_EVENTS;
        e.error = &PwLoop::onCoreError;
        e.done = &PwLoop::onCoreDone;
        return e;
    }();

    std::memset (&coreHook, 0, sizeof (coreHook));
    pw_core_add_listener (corePtr, &coreHook, &coreEvents, this);
    coreHookActive = true;

    reconnectAttempt = 0;
    pwLog ("connected to PipeWire");

    if (onConnected)
        onConnected();
}

void PwLoop::onCoreError (void* data, uint32_t id, int seq, int res, const char* message)
{
    auto* self = static_cast<PwLoop*> (data);
    pwLog ("core error id=%u seq=%d res=%d (%s): %s", id, seq, res, spa_strerror (res),
           message != nullptr ? message : "");

    // Only a broken connection is fatal; errors about single objects are handled by their owners.
    if (id == PW_ID_CORE && (res == -EPIPE || res == -ECONNRESET))
    {
        if (self->lostTimer != nullptr)
            self->lostTimer->startOnce (1);   // never destroy the core from inside its own callback
    }
}

void PwLoop::requestSync()
{
    if (corePtr != nullptr)
        pendingSync = pw_core_sync (corePtr, PW_ID_CORE, 0);
}

void PwLoop::onCoreDone (void* data, uint32_t id, int seq)
{
    auto* self = static_cast<PwLoop*> (data);

    if (id != PW_ID_CORE || self->pendingSync < 0 || seq != self->pendingSync)
        return;

    self->pendingSync = -1;

    if (self->onSyncDone)
        self->onSyncDone();
}

void PwLoop::handleLost()
{
    if (corePtr == nullptr)
        return;

    pwLog ("lost the connection to PipeWire, reconnecting");
    disconnectCore();

    if (! stopping)
    {
        reconnectAttempt = 1;
        scheduleReconnect (500);
    }
}

void PwLoop::disconnectCore()
{
    if (corePtr == nullptr)
        return;

    if (onDisconnecting)
        onDisconnecting();

    pendingSync = -1;

    if (coreHookActive)
    {
        spa_hook_remove (&coreHook);
        coreHookActive = false;
    }

    pw_core_disconnect (corePtr);
    corePtr = nullptr;
}

void PwLoop::scheduleReconnect (int delayMs)
{
    if (connectTimer != nullptr && ! stopping)
        connectTimer->startOnce (delayMs);
}

} // namespace hr::pw
