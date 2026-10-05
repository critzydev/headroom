// hr::createPlatformBackend() for Linux: the real PipeWire backend.
#include "backend/AudioBackend.h"
#include "backend/pipewire/PipeWireBackend.h"
#include "engine/denoise/DfnEngine.h"

#include <pipewire/thread.h>

#include <dlfcn.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>

namespace hr
{
namespace
{
    // rtkit over D-Bus through libsystemd's sd-bus, loaded at run time (no build or link dependency). This is what
    // PipeWire's module-rt does for its own data thread; PipeWire's pw_thread_utils_acquire_rt only works for threads
    // PipeWire created itself, so it refuses our worker (seen on the real Bazzite PC: the data loop was RR 20, the worker
    // stayed at normal priority). rtkit also needs RLIMIT_RTTIME set on the process, which module-rt has already done.
    struct SdBus
    {
        struct Error { const char* name; const char* message; int needFree; };

        using OpenSystem = int (*) (void**);
        using CallMethod = int (*) (void*, const char*, const char*, const char*, const char*, Error*, void**, const char*, ...);
        using GetPropertyU32 = int (*) (void*, const char*, const char*, const char*, const char*, Error*, char, void*);
        using MessageUnref = void* (*) (void*);
        using BusUnref = void* (*) (void*);
        using ErrorFree = void (*) (Error*);

        SdBus()
        {
            handle = dlopen ("libsystemd.so.0", RTLD_NOW | RTLD_LOCAL);

            if (handle == nullptr)
                return;

            openSystem = reinterpret_cast<OpenSystem> (dlsym (handle, "sd_bus_open_system"));
            callMethod = reinterpret_cast<CallMethod> (dlsym (handle, "sd_bus_call_method"));
            getProperty = reinterpret_cast<GetPropertyU32> (dlsym (handle, "sd_bus_get_property_trivial"));
            messageUnref = reinterpret_cast<MessageUnref> (dlsym (handle, "sd_bus_message_unref"));
            busUnref = reinterpret_cast<BusUnref> (dlsym (handle, "sd_bus_unref"));
            errorFree = reinterpret_cast<ErrorFree> (dlsym (handle, "sd_bus_error_free"));
        }

        bool ok() const noexcept
        {
            return openSystem != nullptr && callMethod != nullptr && getProperty != nullptr && messageUnref != nullptr
                && busUnref != nullptr && errorFree != nullptr;
        }

        void* handle = nullptr;
        OpenSystem openSystem = nullptr;
        CallMethod callMethod = nullptr;
        GetPropertyU32 getProperty = nullptr;
        MessageUnref messageUnref = nullptr;
        BusUnref busUnref = nullptr;
        ErrorFree errorFree = nullptr;
    };

    constexpr const char* kRtkitService = "org.freedesktop.RealtimeKit1";
    constexpr const char* kRtkitPath = "/org/freedesktop/RealtimeKit1";

    // Runs on the worker thread itself (setup, not the audio path).
    bool promoteThroughRtkit (int requested) noexcept
    {
        static SdBus sd;

        if (! sd.ok())
            return false;

        void* bus = nullptr;

        if (sd.openSystem (&bus) < 0 || bus == nullptr)
            return false;

        SdBus::Error error {};
        int32_t maxPriority = 0;
        int priority = requested;

        // Stay two steps below rtkit's ceiling so PipeWire's own data threads (which get the ceiling) keep precedence.
        if (sd.getProperty (bus, kRtkitService, kRtkitPath, kRtkitService, "MaxRealtimePriority", &error, 'i', &maxPriority) >= 0
            && maxPriority > 2)
            priority = std::min (requested, (int) maxPriority - 2);

        sd.errorFree (&error);
        error = {};

        void* reply = nullptr;
        const int result = sd.callMethod (bus, kRtkitService, kRtkitPath, kRtkitService, "MakeThreadRealtimeWithPID", &error,
                                          &reply, "ttu", (uint64_t) getpid(), (uint64_t) syscall (SYS_gettid), (uint32_t) priority);

        if (reply != nullptr)
            sd.messageUnref (reply);

        sd.errorFree (&error);
        sd.busUnref (bus);
        return result >= 0;
    }

    // Noise removal's DeepFilterNet worker asks for realtime priority the way PipeWire's own threads get it (rtkit, which
    // an ordinary desktop user is allowed to use), instead of sched_setscheduler, which needs an rlimit most users lack.
    bool promoteThread (int priority) noexcept
    {
        if (pw_thread_utils_acquire_rt (reinterpret_cast<struct spa_thread*> (pthread_self()), priority) >= 0)
            return true;

        return promoteThroughRtkit (priority);
    }
}

std::unique_ptr<AudioBackend> createPlatformBackend()
{
    denoise::setWorkerThreadPromoter (&promoteThread);
    return std::make_unique<PipeWireBackend>();
}

} // namespace hr
