# Fetches JUCE. The dependency name must stay "JUCE" so -DFETCHCONTENT_SOURCE_DIR_JUCE=<path> works.
include(FetchContent)

FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        9.0.3
    GIT_SHALLOW    TRUE)

# We only want the modules and the CMake API, not the demos or Projucer.
set(JUCE_BUILD_EXTRAS   OFF CACHE BOOL "" FORCE)
set(JUCE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(JUCE)
