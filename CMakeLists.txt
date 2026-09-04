cmake_minimum_required(VERSION 3.22)

project(HandsFree VERSION 1.1.0)

# C++17 minimum standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add JUCE as subdirectory
add_subdirectory(external/JUCE)

# melatonin_blur -- MIT, https://github.com/sudara/melatonin_blur
# Figma/CSS-accurate drop and inner shadows on JUCE paths, with an internal
# multi-layer cache so a shadow that has not changed is a blit, not a blur.
# JUCE ships no usable soft shadow (DropShadowEffect is a bilinear smear), and
# the console UI leans on elevation and on the sodium glow around a
# just-fired notch. Declared BEFORE juce_add_gui_app, per the module docs.
juce_add_module(${CMAKE_SOURCE_DIR}/external/melatonin_blur)

# THE TYPEFACES, embedded rather than resolved from the machine.
#
# The design was approved in Saira Condensed / IBM Plex Sans / IBM Plex Mono.
# Naming them in a FontOptions fallback list would give the design on a machine
# that happens to have them and something else everywhere else -- which is not
# a design, it is a lottery. All three are OFL; the licences ship beside the
# files in assets/fonts/.
#
# Declared BEFORE juce_add_gui_app so the generated BinaryData target exists
# for every consumer below.
juce_add_binary_data(HandsFreeFonts SOURCES
    ${CMAKE_SOURCE_DIR}/assets/fonts/SairaCondensed-SemiBold.ttf
    ${CMAKE_SOURCE_DIR}/assets/fonts/SairaCondensed-Bold.ttf
    ${CMAKE_SOURCE_DIR}/assets/fonts/IBMPlexSans-Regular.ttf
    ${CMAKE_SOURCE_DIR}/assets/fonts/IBMPlexMono-Regular.ttf
    ${CMAKE_SOURCE_DIR}/assets/fonts/IBMPlexMono-Medium.ttf
)

# ASIO SDK path
set(JUCE_ASIO_SDK_PATH "${CMAKE_SOURCE_DIR}/external/asiosdk" CACHE PATH "Path to ASIO SDK")

if(EXISTS "${JUCE_ASIO_SDK_PATH}/common/asio.h")
    message(STATUS "ASIO SDK found at ${JUCE_ASIO_SDK_PATH}")
    set(HANDSFREE_HAVE_ASIO ON)
else()
    message(WARNING "ASIO SDK not found. Download from Steinberg and place in external/asiosdk/")
endif()

# Create JUCE standalone GUI application
juce_add_gui_app(HandsFree
    PRODUCT_NAME "AZ Soundtech Hands-free"
    COMPANY_NAME "AZ Soundtech"
    BUNDLE_ID "com.azsoundtech.handsfree"

    # From project() above, NOT a literal. This target used to carry its own
    # copy of "1.0.0", so bumping the project version renamed the installer
    # while the binary inside it kept the old number -- caught 2026-08-26 when
    # AZSoundtech-Handsfree-Setup-1.0.1.exe shipped a 1.0.0 executable.
    #
    # There is now exactly ONE version in this repo: the project() line.
    # handsfree.nsi !searchparses that same line, so the filename, the Apps &
    # Features entry and the binary's own resource all follow it together.
    VERSION ${PROJECT_VERSION}
)

# ASIO support lives behind the JUCE_ASIO compile definition on the app target.
# A CACHE variable named JUCE_ASIO does NOTHING -- verified 2026-08-24: with the
# SDK present but only the cache var set, no vcxproj carried -DJUCE_ASIO=1 and
# the binary shipped without the ASIO device type.
if(HANDSFREE_HAVE_ASIO)
    target_compile_definitions(HandsFree PRIVATE JUCE_ASIO=1)
endif()

# THE source list, shared by all three targets that compile this code: the
# app, the test binary and the snapshot tool. It was duplicated by hand in
# tests/CMakeLists.txt before, which is how a file can end up compiled into one
# target and not another.
#
# Paths are ABSOLUTE: tools/CMakeLists.txt and tests/CMakeLists.txt resolve a
# relative path against their OWN directory, so a bare "src/..." here would
# silently not exist for them.
#
# src/main.cpp is NOT here -- it is the app's entry point, and the other two
# targets bring their own.
set(HANDSFREE_CORE_SOURCES
    ${CMAKE_SOURCE_DIR}/src/app/AudioEngine.cpp
    ${CMAKE_SOURCE_DIR}/src/app/LicenseManager.cpp
    ${CMAKE_SOURCE_DIR}/src/app/LicenseViewModel.cpp
    ${CMAKE_SOURCE_DIR}/src/app/PresetFirstRun.cpp
    ${CMAKE_SOURCE_DIR}/src/app/LicenseManager.h
    ${CMAKE_SOURCE_DIR}/src/app/AudioEngine.h
    ${CMAKE_SOURCE_DIR}/src/app/PresetManager.cpp
    ${CMAKE_SOURCE_DIR}/src/app/PresetManager.h
    ${CMAKE_SOURCE_DIR}/src/app/MainComponent.cpp
    ${CMAKE_SOURCE_DIR}/src/app/MainComponent.h
    ${CMAKE_SOURCE_DIR}/src/gui/DeviceDrawer.cpp
    ${CMAKE_SOURCE_DIR}/src/gui/DeviceDrawer.h
    ${CMAKE_SOURCE_DIR}/src/gui/DevicePanel.cpp
    ${CMAKE_SOURCE_DIR}/src/gui/DevicePanel.h
    ${CMAKE_SOURCE_DIR}/src/gui/DeviceViewModel.cpp
    ${CMAKE_SOURCE_DIR}/src/gui/DeviceViewModel.h
    ${CMAKE_SOURCE_DIR}/src/gui/SlotPanel.cpp
    ${CMAKE_SOURCE_DIR}/src/gui/SegmentedControl.cpp
    ${CMAKE_SOURCE_DIR}/src/gui/SegmentedControl.h
    ${CMAKE_SOURCE_DIR}/src/gui/SlotTabs.cpp
    ${CMAKE_SOURCE_DIR}/src/gui/SlotTabs.h
    ${CMAKE_SOURCE_DIR}/src/gui/SlotPanel.h
    ${CMAKE_SOURCE_DIR}/src/gui/TuningPanel.cpp
    ${CMAKE_SOURCE_DIR}/src/gui/TuningPanel.h
    ${CMAKE_SOURCE_DIR}/src/gui/ModeBar.cpp
    ${CMAKE_SOURCE_DIR}/src/gui/ModeBar.h
    ${CMAKE_SOURCE_DIR}/src/gui/ModeRail.cpp
    ${CMAKE_SOURCE_DIR}/src/gui/ModeRail.h
    ${CMAKE_SOURCE_DIR}/src/gui/NotchListPanel.cpp
    ${CMAKE_SOURCE_DIR}/src/gui/NotchListPanel.h
    ${CMAKE_SOURCE_DIR}/src/gui/StatusBadge.cpp
    ${CMAKE_SOURCE_DIR}/src/gui/StatusBadge.h
    ${CMAKE_SOURCE_DIR}/src/gui/SpectrumView.cpp
    ${CMAKE_SOURCE_DIR}/src/gui/SpectrumView.h
    ${CMAKE_SOURCE_DIR}/src/gui/StatusBar.cpp
    ${CMAKE_SOURCE_DIR}/src/gui/StatusBar.h
    ${CMAKE_SOURCE_DIR}/src/gui/theme/AzTheme.cpp
    ${CMAKE_SOURCE_DIR}/src/gui/theme/AzTheme.h
    ${CMAKE_SOURCE_DIR}/src/dsp/Biquad.cpp
    ${CMAKE_SOURCE_DIR}/src/dsp/Biquad.h
    ${CMAKE_SOURCE_DIR}/src/dsp/Detector.cpp
    ${CMAKE_SOURCE_DIR}/src/dsp/Detector.h
    ${CMAKE_SOURCE_DIR}/src/dsp/PeakinessAnalyzer.cpp
    ${CMAKE_SOURCE_DIR}/src/dsp/PeakinessAnalyzer.h
    ${CMAKE_SOURCE_DIR}/src/dsp/CandidateScorer.cpp
    ${CMAKE_SOURCE_DIR}/src/dsp/CandidateScorer.h
    ${CMAKE_SOURCE_DIR}/src/dsp/NotchChain.cpp
    ${CMAKE_SOURCE_DIR}/src/dsp/NotchChain.h
    ${CMAKE_SOURCE_DIR}/src/dsp/NotchCommand.h
    ${CMAKE_SOURCE_DIR}/src/app/NotchController.cpp
    ${CMAKE_SOURCE_DIR}/src/app/NotchController.h
)

target_sources(HandsFree PRIVATE
    src/main.cpp
    ${HANDSFREE_CORE_SOURCES}
)

# Generates JuceLibraryCode/JuceHeader.h for this target. Without it the
# `#include <JuceHeader.h>` in main.cpp / MainComponent.h / AudioEngine.h
# cannot resolve and the app target fails to compile from a clean configure.
juce_generate_juce_header(HandsFree)

target_include_directories(HandsFree PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

# Link JUCE modules
target_link_libraries(HandsFree
    PRIVATE
        HandsFreeFonts
        melatonin_blur
        juce::juce_audio_devices
        juce::juce_audio_utils
        juce::juce_cryptography
        juce::juce_dsp
        juce::juce_gui_basics
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# Compile definitions
target_compile_definitions(HandsFree
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:HandsFree,JUCE_PRODUCT_NAME>"
        JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:HandsFree,JUCE_VERSION>"
)

# Enable testing
enable_testing()
add_subdirectory(tests)

# Dev tooling. Not part of the shipped product: HandsFreeSnapshot renders the
# console to PNG with no window, so a GUI change can be reviewed without
# screen-capturing a live app.
add_subdirectory(tools)
