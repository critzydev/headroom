// Logger implementation: juce::FileLogger with size-based rotation, mirrored to stdout.
#include "app/Log.h"

#include <cstdio>
#include <memory>

namespace hr::Log
{
namespace
{
    constexpr juce::int64 kMaxLogBytes = 1024 * 1024;

    class TeeLogger final : public juce::Logger
    {
    public:
        explicit TeeLogger (const juce::File& file) : logFile (file)
        {
            rotateIfNeeded();
            openFile();
        }

        void logMessage (const juce::String& message) override
        {
            const auto line = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H:%M:%S  ") + message;

            {
                const juce::ScopedLock lock (mutex);

                if (++writesSinceCheck >= 64)
                {
                    writesSinceCheck = 0;

                    if (rotateIfNeeded())
                        openFile();
                }

                if (fileLogger != nullptr)
                    fileLogger->logMessage (line);
            }

            std::fprintf (stdout, "%s\n", line.toRawUTF8());
            std::fflush (stdout);
        }

    private:
        void openFile()
        {
            // -1: never trim the existing file on open (0 would delete it); rotation handles size.
            fileLogger = std::make_unique<juce::FileLogger> (logFile, "Headroom log", -1);
        }

        // Keeps one previous generation (headroom.log.1) so a crash right after rotation still leaves context.
        // Returns true if the file was rotated, in which case the caller must reopen it.
        bool rotateIfNeeded()
        {
            if (logFile.getSize() < kMaxLogBytes)
                return false;

            fileLogger.reset();

            const auto previous = logFile.withFileExtension (logFile.getFileExtension() + ".1");
            previous.deleteFile();
            logFile.moveFileTo (previous);
            return true;
        }

        juce::File logFile;
        std::unique_ptr<juce::FileLogger> fileLogger;
        juce::CriticalSection mutex;
        int writesSinceCheck = 0;
    };

    std::unique_ptr<TeeLogger>& logger()
    {
        static std::unique_ptr<TeeLogger> instance;
        return instance;
    }
}

void init (const juce::File& logFile)
{
    logFile.getParentDirectory().createDirectory();

    logger() = std::make_unique<TeeLogger> (logFile);
    juce::Logger::setCurrentLogger (logger().get());
    info ("Headroom " HEADROOM_VERSION " started");
}

void shutdown()
{
    juce::Logger::setCurrentLogger (nullptr);
    logger().reset();
}

void info (const juce::String& message)     { juce::Logger::writeToLog (message); }
void warn (const juce::String& message)     { juce::Logger::writeToLog ("WARN  " + message); }
void error (const juce::String& message)    { juce::Logger::writeToLog ("ERROR " + message); }
}
