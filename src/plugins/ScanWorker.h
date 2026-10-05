// Plugin-related command-line modes that run before any app state exists: the scan worker and two dev tools.
#pragma once

#include "Juce.h"

namespace hr::PluginCli
{
// Handles, when `args` (the command line without the executable) starts with one of:
//   --scan-worker <jobFile> <outFile> <parentPid>     the out-of-process plugin scanner (ScanProtocol.h)
//   --dev-scan-report [outFile]                       scan everything now with the real scanner and print what was found
//   --dev-crash-selftest <file> <rtId> <uiId>         (Linux) install CrashGuard, set both slots, die with SIGSEGV
// Returns true if it took over: the caller returns from initialise() at once, without touching settings, logs or the
// crash guard. A scanner worker must stay a bare process, because whatever a hostile plugin does to it must not
// leave anything behind.
bool runIfRequested (const juce::StringArray& args);
}
