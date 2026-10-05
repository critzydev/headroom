// Counting replacements for the global operator new / delete (see AllocationGuard.h).
#include "AllocationGuard.h"

#include <cstdlib>
#include <new>

namespace
{
    thread_local int guardDepth = 0;
    thread_local int newCalls = 0;
    thread_local int deleteCalls = 0;

    void* allocate (std::size_t size)
    {
        if (guardDepth > 0)
            ++newCalls;

        if (void* p = std::malloc (size != 0 ? size : 1))
            return p;

        throw std::bad_alloc();
    }

    void release (void* p) noexcept
    {
        if (p == nullptr)
            return;

        if (guardDepth > 0)
            ++deleteCalls;

        std::free (p);
    }
}

void* operator new (std::size_t size)                                  { return allocate (size); }
void* operator new[] (std::size_t size)                                { return allocate (size); }
void* operator new (std::size_t size, const std::nothrow_t&) noexcept
{
    try { return allocate (size); } catch (...) { return nullptr; }
}
void* operator new[] (std::size_t size, const std::nothrow_t&) noexcept
{
    try { return allocate (size); } catch (...) { return nullptr; }
}

void operator delete (void* p) noexcept                                { release (p); }
void operator delete[] (void* p) noexcept                              { release (p); }
void operator delete (void* p, std::size_t) noexcept                   { release (p); }
void operator delete[] (void* p, std::size_t) noexcept                 { release (p); }
void operator delete (void* p, const std::nothrow_t&) noexcept         { release (p); }
void operator delete[] (void* p, const std::nothrow_t&) noexcept       { release (p); }

namespace hr::test
{
ScopedNoAllocation::ScopedNoAllocation() noexcept : newAtStart (newCalls), deleteAtStart (deleteCalls)
{
    ++guardDepth;
}

ScopedNoAllocation::~ScopedNoAllocation()
{
    --guardDepth;
}

int ScopedNoAllocation::allocations() const noexcept    { return newCalls - newAtStart; }
int ScopedNoAllocation::frees() const noexcept          { return deleteCalls - deleteAtStart; }
}
