# Headroom top-level build: headroom (GUI app), headroom_tests (unit tests), shared headroom_core library.
cmake_minimum_required(VERSION 3.22)

project(Headroom VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# GNU extensions stay on (-std=gnu++20): that is what JUCE's own Linux builds use, and it keeps glibc/X11 headers happy.
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(HeadroomJuce)
include(HeadroomRnnoise)
include(HeadroomDfn)
include(HeadroomWarnings)

# ---------------------------------------------------------------------------------------------
# Sources. Globbed so nobody has to edit CMake to add a file (CONFIGURE_DEPENDS re-globs on build).
# ---------------------------------------------------------------------------------------------
file(GLOB_RECURSE HEADROOM_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(FILTER HEADROOM_CORE_SOURCES EXCLUDE REGEX "/src/Main\.cpp$")

set(HEADROOM_IS_LINUX FALSE)
if(UNIX AND NOT APPLE)
    set(HEADROOM_IS_LINUX TRUE)
endif()

# The PipeWire backend only exists on Linux; everything else must build on Windows too.
if(NOT HEADROOM_IS_LINUX)
    list(FILTER HEADROOM_CORE_SOURCES EXCLUDE REGEX "/src/backend/pipewire/")
endif()

file(GLOB HEADROOM_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/*.cpp")

headroom_set_warnings(${HEADROOM_CORE_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/src/Main.cpp" ${HEADROOM_TEST_SOURCES})

# ---------------------------------------------------------------------------------------------
# Embedded resources (namespace BinaryData): fonts and SVG icons.
# ---------------------------------------------------------------------------------------------
file(GLOB HEADROOM_FONT_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts/*.ttf")
file(GLOB_RECURSE HEADROOM_ICON_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/*.svg")

# The files Headroom writes when it installs itself (app/Install.cpp): the same ones the tarball ships, so there is one copy of each.
set(HEADROOM_PACKAGING_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/headroom.desktop"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/headroom.service"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/51-headroom-wave.conf")

juce_add_binary_data(headroom_data
    HEADER_NAME BinaryData.h
    NAMESPACE   BinaryData
    SOURCES     ${HEADROOM_FONT_FILES} ${HEADROOM_ICON_FILES} ${HEADROOM_PACKAGING_FILES})

# ---------------------------------------------------------------------------------------------
# headroom_core: everything except main(). JUCE modules are compiled once, here, and their include
# directories / definitions are re-exported so the two executables see the same headers and macros.
# ---------------------------------------------------------------------------------------------
set(HEADROOM_JUCE_MODULES
    juce::juce_core
    juce::juce_events
    juce::juce_data_structures
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_audio_basics
    juce::juce_audio_processors
    juce::juce_dsp)

add_library(headroom_core STATIC ${HEADROOM_CORE_SOURCES})
add_library(headroom::core ALIAS headroom_core)

target_include_directories(headroom_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(headroom_core PUBLIC cxx_std_20)

# Not juce_audio_devices / juce_audio_utils / juce_opengl: no ALSA/JACK/GL dependencies.
# PRIVATE, so the module .cpp files (INTERFACE_SOURCES) are compiled only into this library.
target_link_libraries(headroom_core
    PRIVATE
        ${HEADROOM_JUCE_MODULES}
        juce::juce_recommended_config_flags
    PUBLIC
        headroom_data
        rnnoise
        headroom_dfn)

# Re-export what the JUCE module targets would have given a direct consumer.
foreach(module IN LISTS HEADROOM_JUCE_MODULES)
    target_include_directories(headroom_core INTERFACE
        "$<TARGET_PROPERTY:${module},INTERFACE_INCLUDE_DIRECTORIES>")
    target_compile_definitions(headroom_core INTERFACE
        "$<TARGET_PROPERTY:${module},INTERFACE_COMPILE_DEFINITIONS>")
endforeach()

target_compile_definitions(headroom_core PUBLIC
    HEADROOM_VERSION="${PROJECT_VERSION}"
    HEADROOM_HAVE_DFN=$<BOOL:${HEADROOM_DFN_ENABLED}>   # DeepFilterNet (Fast/Best noise removal) compiled in
    JUCE_STANDALONE_APPLICATION=1   # same values juce_add_gui_app/console_app give the executables
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_PLUGINHOST_VST3=1
    $<$<CONFIG:Debug>:JUCE_LOG_ASSERTIONS=1>   # jassert failures go through juce::Logger (our log file, or the test runner)
    $<$<BOOL:${HEADROOM_IS_LINUX}>:JUCE_PLUGINHOST_LV2=1>
    $<$<BOOL:${HEADROOM_IS_LINUX}>:JUCE_PLUGINHOST_LADSPA=1>)

if(MSVC)
    target_compile_options(headroom_core PUBLIC /utf-8)
    target_compile_definitions(headroom_core PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

if(HEADROOM_IS_LINUX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
    target_link_libraries(headroom_core PRIVATE PkgConfig::PIPEWIRE)
endif()

# ---------------------------------------------------------------------------------------------
# headroom: the GUI application.
# ---------------------------------------------------------------------------------------------
set(HEADROOM_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/headroom.png")
set(HEADROOM_ICON_ARGS "")
if(EXISTS "${HEADROOM_ICON}")
    set(HEADROOM_ICON_ARGS ICON_BIG "${HEADROOM_ICON}" ICON_SMALL "${HEADROOM_ICON}")
endif()

juce_add_gui_app(headroom
    PRODUCT_NAME "Headroom"
    COMPANY_NAME "Headroom"
    VERSION "${PROJECT_VERSION}"
    ${HEADROOM_ICON_ARGS})

target_sources(headroom PRIVATE src/Main.cpp)
target_link_libraries(headroom PRIVATE headroom_core juce::juce_recommended_config_flags)

# JUCE names the binary after PRODUCT_NAME ("Headroom"); the packaging and CI expect "headroom".
# Both executables land directly in the build directory.
set_target_properties(headroom PROPERTIES
    OUTPUT_NAME headroom
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# ---------------------------------------------------------------------------------------------
# headroom_tests: console app that runs every juce::UnitTest linked in; non-zero exit on failure.
# ---------------------------------------------------------------------------------------------
juce_add_console_app(headroom_tests PRODUCT_NAME "headroom_tests")

target_sources(headroom_tests PRIVATE ${HEADROOM_TEST_SOURCES})
target_link_libraries(headroom_tests PRIVATE headroom_core juce::juce_recommended_config_flags)
set_target_properties(headroom_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
# Real speech clips for the noise-removal tests (tests/data/speech; credits in THIRD-PARTY-NOTICES.txt); they skip themselves
# when the folder is not there.
target_compile_definitions(headroom_tests PRIVATE HEADROOM_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
# The plugin scanner tests launch the real headroom executable as their worker process (it sits next to the test binary).
add_dependencies(headroom_tests headroom)

enable_testing()
add_test(NAME headroom_unit_tests COMMAND headroom_tests)
