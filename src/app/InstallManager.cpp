// InstallManager implementation.
#include "app/InstallManager.h"

#include "app/Log.h"
#include "app/Paths.h"
#include "app/SingleInstance.h"

namespace hr
{
namespace
{
    std::shared_ptr<install::CommandRunner> runnerFor (const InstallManager::Setup& setup)
    {
        return setup.runner != nullptr ? setup.runner : std::shared_ptr<install::CommandRunner> (install::makeSystemRunner());
    }

    juce::File resolve (juce::File file)
    {
        for (int i = 0; i < 8 && file.isSymbolicLink(); ++i)
            file = file.getLinkedTarget();

        return file;
    }
}

InstallManager::Setup InstallManager::realSetup()
{
    Setup s;
   #if JUCE_LINUX
    s.supported = true;
   #endif

    const auto appImage = juce::SystemStats::getEnvironmentVariable ("APPIMAGE", {});

    if (appImage.isNotEmpty() && juce::File::isAbsolutePath (appImage))
        s.appImage = juce::File (appImage);

    s.settingsFolders = { Paths::getConfigDirPath(), Paths::getStateDirPath() };
    s.log = [] (const juce::String& line) { Log::info ("install: " + line); };
    s.quitRunning = [] { quitRunningHeadroom(); };

    return s;
}

// Another Headroom that is running (the service, usually) saves and quits by itself before it is replaced or removed.
void InstallManager::quitRunningHeadroom()
{
    SingleInstance instance;

    if (instance.isPrimaryRunning() && instance.sendCommand ("quit", 3000) == "ok")
    {
        const auto giveUpAt = juce::Time::getMillisecondCounter() + 8000;

        while (instance.isPrimaryRunning() && juce::Time::getMillisecondCounter() < giveUpAt)
            juce::Thread::sleep (100);
    }
}

InstallManager::InstallManager (Setup s)
    : setup (std::move (s)),
      runner (runnerFor (setup)),
      installer (setup.locations, *runner, install::Payload::fromBinaryData())
{
    installer.setLogger (setup.log);
    installer.setQuitRunning (setup.quitRunning);
    installer.setPauses (setup.pauseAfterWirePlumberMs, setup.serviceCheckMs);
    installer.setUdevRule (setup.udevRule);
    installer.setPkexec (setup.pkexec);
}

InstallManager::~InstallManager()
{
    if (worker.joinable())
        worker.join();
}

bool InstallManager::isThisTheInstalledCopy() const
{
    const auto info = installer.inspect();
    return runsFromAppImage() && info.programPresent && resolve (info.program) == resolve (setup.appImage);
}

bool InstallManager::isInManagedFolder() const
{
    return runsFromAppImage() && installer.isInManagedFolder (setup.appImage);
}

juce::String InstallManager::installedVersion()
{
    const auto info = installer.inspect();

    // Only an AppImage is asked: the tarball's program of the first releases does not know --version, and would start up (and show
    // its window) instead of answering. Its version is unknown, which counts as older.
    return info.installed() && info.isAppImage ? installer.installedVersion (info.program) : juce::String();
}

bool InstallManager::offersUpdate (juce::String& installedVersionOut)
{
    installedVersionOut = {};

    if (! canInstall())
        return false;

    const auto info = installer.inspect();

    // Not installed: that is setup, not an update. A file an AppImage manager looks after is its to update, not ours.
    if (! info.installed() || info.managedElsewhere || isThisTheInstalledCopy())
        return false;

    installedVersionOut = info.isAppImage ? installer.installedVersion (info.program) : juce::String();
    return install::compareVersions (install::currentVersion(), installedVersionOut) > 0;
}

void InstallManager::installAsync (SetupRequest request, std::function<void (const install::Result&)> done)
{
    if (busy.exchange (true))
        return;

    if (worker.joinable())
        worker.join();

    juce::WeakReference<InstallManager> self (this);

    worker = std::thread ([this, self, request, done = std::move (done)]
    {
        install::InstallOptions options;
        options.source = setup.appImage;
        options.keepInPlace = request.keepInPlace;
        options.startAtLogin = request.startAtLogin;
        options.startNow = request.startNow;

        // An AppImage of this version or a newer one is installed already (and this is another file): keep that one.
        const auto info = installer.inspect();

        if (info.installed() && info.isAppImage && ! isThisTheInstalledCopy()
            && install::compareVersions (install::currentVersion(), installer.installedVersion (info.program)) <= 0)
        {
            options.source = info.program;
            options.keepInPlace = info.managedElsewhere;
        }

        auto result = installer.install (options);

        juce::MessageManager::callAsync ([self, result = std::move (result), done]
        {
            if (auto* manager = self.get())
            {
                manager->busy = false;

                if (done)
                    done (result);
            }
        });
    });
}

void InstallManager::setStartAtLogin (bool enabled)
{
    if (isSupported() && installer.inspect().unitPresent)
        installer.setStartAtLogin (enabled);
}
}
