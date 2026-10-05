// Config / state directory resolution.
#include "app/Paths.h"

namespace hr::Paths
{
namespace
{
    juce::File& programmaticRoot()
    {
        static juce::File dir;
        return dir;
    }

    // setRootOverride() wins, then the HEADROOM_DATA_DIR environment variable (integration tests, dev runs that
    // must not touch the real settings), otherwise the real per-user locations.
    juce::File rootOverride()
    {
        if (programmaticRoot() != juce::File())
            return programmaticRoot();

        const auto fromEnvironment = juce::SystemStats::getEnvironmentVariable ("HEADROOM_DATA_DIR", {});

        if (fromEnvironment.isNotEmpty() && juce::File::isAbsolutePath (fromEnvironment))
            return juce::File (fromEnvironment);

        return {};
    }

    juce::File ensureDir (const juce::File& dir)
    {
        dir.createDirectory();
        return dir;
    }

#if JUCE_LINUX || JUCE_BSD
    juce::File xdgDir (const char* variable, const char* fallbackUnderHome)
    {
        const auto value = juce::SystemStats::getEnvironmentVariable (variable, {});

        // The XDG spec says relative paths are invalid and must be ignored.
        if (value.isNotEmpty() && juce::File::isAbsolutePath (value))
            return juce::File (value).getChildFile ("headroom");

        return juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                   .getChildFile (fallbackUnderHome)
                   .getChildFile ("headroom");
    }
#endif
}

juce::File getConfigDirPath()
{
    if (const auto root = rootOverride(); root != juce::File())
        return root;

#if JUCE_LINUX || JUCE_BSD
    return xdgDir ("XDG_CONFIG_HOME", ".config");
#else
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Headroom");
#endif
}

juce::File getStateDirPath()
{
    if (const auto root = rootOverride(); root != juce::File())
        return root;

#if JUCE_LINUX || JUCE_BSD
    return xdgDir ("XDG_STATE_HOME", ".local/state");
#else
    return getConfigDirPath();      // Windows has one per-user folder for everything
#endif
}

juce::File getConfigDir()           { return ensureDir (getConfigDirPath()); }
juce::File getStateDir()            { return ensureDir (getStateDirPath()); }

juce::File getStateFile()           { return getConfigDir().getChildFile ("state.xml"); }
juce::File getPluginsFile()         { return getConfigDir().getChildFile ("plugins.xml"); }
juce::File getScanDeadMansFile()    { return getStateDir().getChildFile ("plugin-scan-deadmans.txt"); }
juce::File getCrashFile()           { return getStateDir().getChildFile ("crash.txt"); }
juce::File getLogFile()             { return getStateDir().getChildFile ("headroom.log"); }

void setRootOverride (const juce::File& directory)
{
    programmaticRoot() = directory;
}
}
