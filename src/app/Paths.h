// Where Headroom keeps its files: XDG dirs on Linux, %APPDATA%\Headroom on Windows.
#pragma once

#include "Juce.h"

namespace hr::Paths
{
juce::File getConfigDir();          // state.xml, plugins.xml   ($XDG_CONFIG_HOME/headroom)
juce::File getStateDir();           // crash.txt, headroom.log  ($XDG_STATE_HOME/headroom)

// The same two folders without creating them (the uninstaller asks where they would be; there may be nothing to delete).
juce::File getConfigDirPath();
juce::File getStateDirPath();

juce::File getStateFile();          // <config>/state.xml
juce::File getPluginsFile();        // <config>/plugins.xml (KnownPluginList cache)
juce::File getScanDeadMansFile();   // <state>/plugin-scan-deadmans.txt (the file being scanned right now)
juce::File getCrashFile();          // <state>/crash.txt
juce::File getLogFile();            // <state>/headroom.log

// Redirects both dirs under one folder (used by --screenshot so it never touches the user's real settings).
// Pass juce::File() to go back to the real locations. Independently of this, an absolute HEADROOM_DATA_DIR
// environment variable does the same for a whole run (CI integration test, manual dev runs).
void setRootOverride (const juce::File& directory);
}
