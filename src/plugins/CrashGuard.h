// Crash attribution for third-party plugins: which slot was running when the process died.
#pragma once

#include "Juce.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hr::CrashGuard
{
// Crash-id (SLOT crashId in the model) of the slot being touched, 0 = none. Lock-free so a signal
// handler can read them.
inline std::atomic<uint32_t> currentRtSlot { 0 };   // set by the RT thread around processBlock()
inline std::atomic<uint32_t> currentUiSlot { 0 };   // set by the message thread around instantiate / editor / setState

static_assert (std::atomic<uint32_t>::is_always_lock_free);

// RAII helper for the message-thread cases. Nests: leaving an inner scope restores what the outer one set, so a
// crash after the inner call but still inside the outer one is still attributed.
struct ScopedUiSlot
{
    explicit ScopedUiSlot (uint32_t crashId) noexcept   { previous = currentUiSlot.exchange (crashId, std::memory_order_relaxed); }
    ~ScopedUiSlot() noexcept                            { currentUiSlot.store (previous, std::memory_order_relaxed); }

    ScopedUiSlot (const ScopedUiSlot&) = delete;
    ScopedUiSlot& operator= (const ScopedUiSlot&) = delete;

private:
    uint32_t previous = 0;
};

// crash.txt format, written by the signal handler: "<rtSlotCrashId> <uiSlotCrashId>\n" in decimal, one line per crash.
constexpr size_t kMaxRecordChars = 24;      // two 10-digit numbers, a space, a newline

// Formats one crash record into `out` (at least kMaxRecordChars) and returns its length. Async-signal-safe: plain
// arithmetic on the caller's buffer, no libc, no allocation. (The handler can't use snprintf.) Not Linux-only so the
// format can be tested everywhere.
size_t formatCrashRecord (char* out, uint32_t rtSlot, uint32_t uiSlot) noexcept;

// Linux only, no-op elsewhere. Call once, early in startup, on the message thread:
//  1. Takes over the previous run's crash.txt, so that a later readAndClearLastCrash (the order in Main.cpp is
//     install() first, the AppController later) still sees it.
//  2. Opens a fresh crash.txt for writing now, not at crash time (a plugin that leaks file descriptors could make
//     open() fail then), close-on-exec so the scanner worker doesn't inherit it.
//  3. Installs SIGSEGV / SIGBUS / SIGILL / SIGFPE / SIGABRT handlers that write() the two atomic slot ids to that
//     file, restore the default action and raise() the signal again. Only async-signal-safe calls, chained to nothing.
// The scanner worker never calls this, so a crash while scanning can't disable one of the user's slots.
void install (const juce::File& crashFile);

// The crash ids left by the previous run (non-zero, de-duplicated). If install() ran for this file the ids it took
// over are returned once; otherwise the file is read and deleted here.
std::vector<uint32_t> readAndClearLastCrash (const juce::File& crashFile);
}
