// Shared includes, small helpers and RAII wrappers for the PipeWire backend (Linux only).
#pragma once

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/param.h>
#include <spa/param/profile.h>
#include <spa/param/props.h>
#include <spa/param/route.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>
#include <spa/utils/dict.h>
#include <spa/utils/hook.h>
#include <spa/utils/result.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace hr::pw
{

/** Every object Headroom creates in the graph has a node.name starting with this. */
inline constexpr const char* kNodePrefix = "headroom.";
inline constexpr const char* kChannelPrefix = "headroom.ch.";
inline constexpr const char* kEngineName = "headroom.engine";
inline constexpr const char* kMicSourceName = "headroom.mic";
inline constexpr const char* kStreamSourceName = "headroom.stream";

// -------------------------------------------------------------------------------------------------
// Logging (never call from the RT thread)

inline void pwLog (const char* format, ...) __attribute__ ((format (printf, 1, 2)));
inline void pwLog (const char* format, ...)
{
    char buffer[768];
    std::va_list args;
    va_start (args, format);
    std::vsnprintf (buffer, sizeof (buffer), format, args);
    va_end (args);
    juce::Logger::writeToLog (juce::String ("[pipewire] ") + juce::String::fromUTF8 (buffer));
}

// -------------------------------------------------------------------------------------------------
// Small helpers

inline int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds> (steady_clock::now().time_since_epoch()).count();
}

inline std::string dictGet (const spa_dict* dict, const char* key)
{
    if (dict == nullptr)
        return {};

    const char* value = spa_dict_lookup (dict, key);
    return value != nullptr ? std::string (value) : std::string();
}

inline bool startsWith (const std::string& text, const char* prefix)
{
    return text.rfind (prefix, 0) == 0;
}

inline uint64_t parseU64 (const std::string& text)
{
    if (text.empty())
        return 0;

    return std::strtoull (text.c_str(), nullptr, 10);
}

inline int parseInt (const std::string& text, int fallback)
{
    if (text.empty())
        return fallback;

    char* end = nullptr;
    const long value = std::strtol (text.c_str(), &end, 10);
    return end != text.c_str() ? (int) value : fallback;
}

/** Parses "0x0fd9" style ids. Returns -1 if it isn't a number. */
inline int parseHexId (const std::string& text)
{
    if (text.empty())
        return -1;

    char* end = nullptr;
    const long value = std::strtol (text.c_str(), &end, 16);
    return end != text.c_str() ? (int) value : -1;
}

inline juce::String toJuce (const std::string& text)
{
    return juce::String::fromUTF8 (text.c_str());
}

inline std::string toStd (const juce::String& text)
{
    return std::string (text.toRawUTF8());
}

/** Builds a juce::String from UTF-8 literal text (used for the few strings with an em dash). */
inline juce::String utf8 (const char* text)
{
    return juce::String::fromUTF8 (text);
}

// -------------------------------------------------------------------------------------------------
// Locking

/** Holds pw_thread_loop_lock for a scope. Recursive, so it is safe on the loop thread too. */
class LoopLock
{
public:
    explicit LoopLock (pw_thread_loop* threadLoopToLock) : threadLoop (threadLoopToLock)
    {
        if (threadLoop != nullptr)
            pw_thread_loop_lock (threadLoop);
    }

    ~LoopLock()
    {
        if (threadLoop != nullptr)
            pw_thread_loop_unlock (threadLoop);
    }

    LoopLock (const LoopLock&) = delete;
    LoopLock& operator= (const LoopLock&) = delete;

private:
    pw_thread_loop* threadLoop;
};

// -------------------------------------------------------------------------------------------------
// pw_properties wrapper

class Props
{
public:
    Props() : props (pw_properties_new (NULL, NULL)) {}

    ~Props()
    {
        if (props != nullptr)
            pw_properties_free (props);
    }

    Props (const Props&) = delete;
    Props& operator= (const Props&) = delete;

    Props& set (const char* key, const char* value)
    {
        pw_properties_set (props, key, value);
        return *this;
    }

    Props& set (const char* key, const std::string& value)
    {
        pw_properties_set (props, key, value.c_str());
        return *this;
    }

    Props& set (const char* key, int value)
    {
        pw_properties_set (props, key, std::to_string (value).c_str());
        return *this;
    }

    const spa_dict* dict() const noexcept { return &props->dict; }

    /** Hands ownership to a pw_* call that takes it (pw_filter_new, ...). */
    pw_properties* release() noexcept
    {
        auto* result = props;
        props = nullptr;
        return result;
    }

private:
    pw_properties* props;
};

// -------------------------------------------------------------------------------------------------
// Proxy + listener ownership

/**
    Owns one pw_proxy and the spa_hooks attached to it. Hooks are always removed before the
    proxy is destroyed. Callbacks only record what happened; owners must not destroy a
    ProxyHolder from inside one of its own callbacks (sweep later instead).
*/
class ProxyHolder
{
public:
    ProxyHolder() = default;
    ~ProxyHolder() { destroy(); }

    ProxyHolder (const ProxyHolder&) = delete;
    ProxyHolder& operator= (const ProxyHolder&) = delete;

    /** Takes ownership of a proxy returned by pw_registry_bind / pw_core_create_object. */
    void adopt (void* rawProxy)
    {
        destroy();
        gone = false;
        serverRemoved = false;
        failed = false;
        boundId = SPA_ID_INVALID;

        if (rawProxy == nullptr)
            return;

        proxy = static_cast<pw_proxy*> (rawProxy);
        std::memset (&proxyHook, 0, sizeof (proxyHook));
        pw_proxy_add_listener (proxy, &proxyHook, &getEvents(), this);
        proxyHookActive = true;
    }

    pw_proxy* get() const noexcept { return proxy; }
    bool alive() const noexcept { return proxy != nullptr; }

    /** Registers an interface listener (pw_node_add_listener, ...). Fn gets the hook to fill. */
    template <typename Fn>
    bool addObjectListener (Fn&& registerFn)
    {
        if (proxy == nullptr || objectHookActive)
            return false;

        std::memset (&objectHook, 0, sizeof (objectHook));
        const int result = registerFn (&objectHook);
        objectHookActive = result >= 0;
        return objectHookActive;
    }

    void destroy()
    {
        if (proxy == nullptr)
            return;

        auto* victim = proxy;
        proxy = nullptr;
        removeHooks();
        pw_proxy_destroy (victim);
    }

    // State recorded by the callbacks.
    bool gone = false;            // the proxy was destroyed underneath us (core disconnected)
    bool serverRemoved = false;   // the server told us the object no longer exists
    bool failed = false;          // the server reported an error for this object
    uint32_t boundId = SPA_ID_INVALID;
    int64_t failedAtMs = 0;

    std::function<void()> onEvent;   // called after any of the above changes (loop thread)

private:
    void removeHooks()
    {
        if (objectHookActive)
        {
            spa_hook_remove (&objectHook);
            objectHookActive = false;
        }

        if (proxyHookActive)
        {
            spa_hook_remove (&proxyHook);
            proxyHookActive = false;
        }
    }

    static void onDestroy (void* data)
    {
        auto* self = static_cast<ProxyHolder*> (data);
        self->proxy = nullptr;
        self->gone = true;
        self->removeHooks();

        if (self->onEvent)
            self->onEvent();
    }

    static void onRemoved (void* data)
    {
        auto* self = static_cast<ProxyHolder*> (data);
        self->serverRemoved = true;

        if (self->onEvent)
            self->onEvent();
    }

    static void onBound (void* data, uint32_t globalId)
    {
        auto* self = static_cast<ProxyHolder*> (data);
        self->boundId = globalId;

        if (self->onEvent)
            self->onEvent();
    }

    static void onError (void* data, int, int result, const char* message)
    {
        auto* self = static_cast<ProxyHolder*> (data);
        self->failed = true;
        self->failedAtMs = nowMs();
        pwLog ("object error %d: %s", result, message != nullptr ? message : "?");

        if (self->onEvent)
            self->onEvent();
    }

    static const pw_proxy_events& getEvents()
    {
        static const pw_proxy_events events = []
        {
            pw_proxy_events e {};
            e.version = PW_VERSION_PROXY_EVENTS;
            e.destroy = &ProxyHolder::onDestroy;
            e.bound = &ProxyHolder::onBound;
            e.removed = &ProxyHolder::onRemoved;
            e.error = &ProxyHolder::onError;
            return e;
        }();
        return events;
    }

    pw_proxy* proxy = nullptr;
    spa_hook proxyHook {};
    spa_hook objectHook {};
    bool proxyHookActive = false;
    bool objectHookActive = false;
};

// -------------------------------------------------------------------------------------------------
// Timers on the pw loop

class LoopTimer
{
public:
    LoopTimer (pw_loop* loopToUse, std::function<void()> function)
        : loop (loopToUse), callback (std::move (function))
    {
        source = pw_loop_add_timer (loop, &LoopTimer::onTimer, this);
    }

    ~LoopTimer()
    {
        if (source != nullptr)
            pw_loop_destroy_source (loop, source);
    }

    LoopTimer (const LoopTimer&) = delete;
    LoopTimer& operator= (const LoopTimer&) = delete;

    bool valid() const noexcept { return source != nullptr; }

    void startOnce (int milliseconds) { arm (milliseconds, 0); }
    void startRepeating (int milliseconds) { arm (milliseconds, milliseconds); }
    void stop() { arm (0, 0); }

private:
    static timespec toTimespec (int milliseconds)
    {
        timespec ts {};
        ts.tv_sec = milliseconds / 1000;
        ts.tv_nsec = (long) (milliseconds % 1000) * 1000000L;
        return ts;
    }

    void arm (int firstMs, int repeatMs)
    {
        if (source == nullptr)
            return;

        timespec value = toTimespec (firstMs > 0 ? firstMs : 0);
        timespec interval = toTimespec (repeatMs > 0 ? repeatMs : 0);
        pw_loop_update_timer (loop, source, &value, &interval, false);
    }

    static void onTimer (void* data, uint64_t)
    {
        auto* self = static_cast<LoopTimer*> (data);

        if (self->callback)
            self->callback();
    }

    pw_loop* loop;
    std::function<void()> callback;
    spa_source* source = nullptr;
};

} // namespace hr::pw
