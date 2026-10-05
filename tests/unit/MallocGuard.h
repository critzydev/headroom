// Test helper (Linux): counts malloc-family calls, on the current thread and, on request, on every thread.
#pragma once

#include <cstdint>

namespace hr::test
{
// On Linux the test binary defines malloc, calloc, realloc, posix_memalign, aligned_alloc and memalign itself (they
// forward to glibc's __libc_* functions), so this also sees allocations made by code that never calls operator new:
// C libraries and the Rust standard library inside DeepFilterNet. On other systems nothing is counted and
// isSupported() is false.
class MallocCounter
{
public:
    static bool isSupported() noexcept;

    // Calls made on the calling thread since it was constructed.
    class ScopedThread
    {
    public:
        ScopedThread() noexcept;
        ~ScopedThread();
        ScopedThread (const ScopedThread&) = delete;
        ScopedThread& operator= (const ScopedThread&) = delete;

        int allocations() const noexcept;

    private:
        int atStart;
    };

    // Calls made on all threads between start and stop (other threads' work included).
    static void startGlobal() noexcept;
    static uint64_t stopGlobal() noexcept;
};
}
