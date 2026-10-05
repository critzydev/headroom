// Test helper: counts global operator new / delete calls made on the current thread while a guard is alive.
#pragma once

namespace hr::test
{
// The counting operators are defined in AllocationGuard.cpp and replace the global ones for the whole test binary.
// They only count on a thread that has a guard open, so the rest of the tests are unaffected. This catches std::vector
// growth, make_shared, std::function, std::string, juce::String ... in code that must not allocate on the RT thread.
// (JUCE containers backed by malloc are not seen; the RT code avoids them by construction and review.)
class ScopedNoAllocation
{
public:
    ScopedNoAllocation() noexcept;
    ~ScopedNoAllocation();

    ScopedNoAllocation (const ScopedNoAllocation&) = delete;
    ScopedNoAllocation& operator= (const ScopedNoAllocation&) = delete;

    int allocations() const noexcept;       // operator new calls since construction, this thread
    int frees() const noexcept;             // operator delete calls since construction, this thread

private:
    int newAtStart, deleteAtStart;
};
}
