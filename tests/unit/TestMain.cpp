// headroom_tests entry point: runs every registered juce::UnitTest, prints to stdout, exits non-zero on failure.
#include "Juce.h"

#include <atomic>
#include <iostream>

namespace
{
    // Routes juce::Logger output (DBG, jassert failures in Debug builds) to stdout and counts assertion failures,
    // so a tripped jassert fails the run instead of scrolling past unnoticed.
    class StdoutLogger final : public juce::Logger
    {
    public:
        void logMessage (const juce::String& message) override
        {
            if (message.contains ("JUCE Assertion failure"))
                ++assertions;

            std::cout << message << std::endl;
        }

        std::atomic<int> assertions { 0 };
    };

    class ConsoleRunner final : public juce::UnitTestRunner
    {
    protected:
        void logMessage (const juce::String& message) override
        {
            std::cout << message << std::endl;
        }
    };
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;    // gives the tests a message thread and timers

    StdoutLogger logger;
    juce::Logger::setCurrentLogger (&logger);

    int failures = 0;
    int checks = 0;

    {
        ConsoleRunner runner;
        runner.setAssertOnFailure (false);

        // Optional: `headroom_tests <category>` runs one category ("headroom" is the only one we use), and
        // `headroom_tests <category> <name part>` only the tests of that category whose name contains the part
        // (a quick local run of one area instead of the whole suite).
        if (argc > 2)
        {
            juce::Array<juce::UnitTest*> selected;

            for (auto* test : juce::UnitTest::getTestsInCategory (juce::String (argv[1])))
                if (test->getName().containsIgnoreCase (juce::String (argv[2])))
                    selected.add (test);

            runner.runTests (selected);
        }
        else if (argc > 1)
            runner.runTestsInCategory (juce::String (argv[1]));
        else
            runner.runAllTests();

        for (int i = 0; i < runner.getNumResults(); ++i)
        {
            if (auto* result = runner.getResult (i))
            {
                failures += result->failures;
                checks += result->passes + result->failures;
            }
        }
    }

    failures += logger.assertions.load();
    juce::Logger::setCurrentLogger (nullptr);

    std::cout << "\n" << checks << " checks, " << failures << " failures" << std::endl;
    return failures == 0 ? 0 : 1;
}
