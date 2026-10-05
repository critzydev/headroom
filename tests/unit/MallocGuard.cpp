// Counting malloc-family interposers for the test binary on Linux (see MallocGuard.h).
#include "MallocGuard.h"

#include <atomic>

#if defined(__linux__) && defined(__GLIBC__)

#include <cerrno>
#include <cstddef>

extern "C"
{
void* __libc_malloc (size_t);
void* __libc_calloc (size_t, size_t);
void* __libc_realloc (void*, size_t);
void* __libc_memalign (size_t, size_t);
}

namespace
{
    thread_local int threadCalls = 0;
    std::atomic<bool> globalOn { false };
    std::atomic<uint64_t> globalCalls { 0 };

    inline void note() noexcept
    {
        ++threadCalls;

        if (globalOn.load (std::memory_order_relaxed))
            globalCalls.fetch_add (1, std::memory_order_relaxed);
    }
}

extern "C"
{
void* malloc (size_t n) noexcept                { note(); return __libc_malloc (n); }
void* calloc (size_t a, size_t b) noexcept      { note(); return __libc_calloc (a, b); }
void* realloc (void* p, size_t n) noexcept      { note(); return __libc_realloc (p, n); }
void* memalign (size_t align, size_t n) noexcept { note(); return __libc_memalign (align, n); }
void* aligned_alloc (size_t align, size_t n) noexcept { note(); return __libc_memalign (align, n); }

int posix_memalign (void** out, size_t align, size_t n) noexcept
{
    note();
    void* p = __libc_memalign (align, n);

    if (p == nullptr)
        return ENOMEM;

    *out = p;
    return 0;
}
}

namespace hr::test
{
bool MallocCounter::isSupported() noexcept                  { return true; }
MallocCounter::ScopedThread::ScopedThread() noexcept        : atStart (threadCalls) {}
MallocCounter::ScopedThread::~ScopedThread()                = default;
int MallocCounter::ScopedThread::allocations() const noexcept   { return threadCalls - atStart; }
void MallocCounter::startGlobal() noexcept                  { globalCalls.store (0); globalOn.store (true); }
uint64_t MallocCounter::stopGlobal() noexcept               { globalOn.store (false); return globalCalls.load(); }
}

#else

namespace hr::test
{
bool MallocCounter::isSupported() noexcept                  { return false; }
MallocCounter::ScopedThread::ScopedThread() noexcept        : atStart (0) {}
MallocCounter::ScopedThread::~ScopedThread()                = default;
int MallocCounter::ScopedThread::allocations() const noexcept   { return 0; }
void MallocCounter::startGlobal() noexcept                  {}
uint64_t MallocCounter::stopGlobal() noexcept               { return 0; }
}

#endif
