// Logging: a rotating file in the state dir (1 MB) plus stdout. Not for the RT thread.
#pragma once

#include "Juce.h"

namespace hr::Log
{
// Installs the file+stdout logger as juce::Logger's current logger (so DBG() and Logger::writeToLog() land here too).
void init (const juce::File& logFile);
void shutdown();

void info (const juce::String& message);
void warn (const juce::String& message);
void error (const juce::String& message);
}
