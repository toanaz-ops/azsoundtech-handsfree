cmake_minimum_required(VERSION 3.22)

project(HandsFree VERSION 1.0.0)

# C++17 minimum standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add JUCE as subdirectory
add_subdirectory(external/JUCE)

# ASIO SDK path
set(JUCE_ASIO_SDK_PATH "${CMAKE_SOURCE_DIR}/external/asiosdk" CACHE PATH "Path to ASIO SDK")

if(EXISTS "${JUCE_ASIO_SDK_PATH}/common/asio.h")
    message(STATUS "ASIO SDK found at ${JUCE_ASIO_SDK_PATH}")
    set(JUCE_ASIO ON CACHE BOOL "Enable ASIO support" FORCE)
else()
    message(WARNING "ASIO SDK not found. Download from Steinberg and place in external/asiosdk/")
endif()

# Create JUCE standalone GUI application
juce_add_gui_app(HandsFree
    PRODUCT_NAME "AZ Soundtech Hands-free"
    COMPANY_NAME "AZ Soundtech"
    BUNDLE_ID "com.azsoundtech.handsfree"
    VERSION "1.0.0"
)

# Source files
target_sources(HandsFree PRIVATE
    src/main.cpp
    src/app/AudioEngine.cpp
    src/app/LicenseManager.cpp
    src/app/LicenseManager.h
    src/app/AudioEngine.h
    src/app/PresetManager.cpp
    src/app/PresetManager.h
    src/app/MainComponent.cpp
    src/app/MainComponent.h
    src/gui/DevicePanel.cpp
    src/gui/DevicePanel.h
    src/gui/DeviceViewModel.cpp
    src/gui/DeviceViewModel.h
    src/gui/ModeBar.cpp
    src/gui/ModeBar.h
    src/gui/StatusBar.cpp
    src/gui/StatusBar.h
    src/dsp/Biquad.cpp
    src/dsp/Biquad.h
    src/dsp/Detector.cpp
    src/dsp/Detector.h
    src/dsp/PeakinessAnalyzer.cpp
    src/dsp/PeakinessAnalyzer.h
    src/dsp/NotchChain.cpp
    src/dsp/NotchChain.h
    src/dsp/NotchCommand.h
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
