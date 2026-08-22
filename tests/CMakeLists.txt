include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)

# For Windows: prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(HandsFreeTests
    test_ringbuffer.cpp
    test_biquad.cpp
    test_notchchain.cpp
    test_detector.cpp
    test_peakiness.cpp
    test_audioengine.cpp
    test_licensemanager.cpp
    ../src/app/LicenseManager.cpp
    ../src/app/AudioEngine.cpp
    ../src/dsp/Biquad.cpp
    ../src/dsp/NotchChain.cpp
    ../src/dsp/Detector.cpp
    ../src/dsp/PeakinessAnalyzer.cpp
)

# Detector uses juce::dsp::FFT / WindowingFunction, so the test target needs
# the juce_dsp module. AudioEngine needs juce_audio_devices for
# juce::AudioDeviceManager / AudioIODeviceCallback (it pulls in juce_events and
# juce_audio_basics transitively, which is where ScopedJuceInitialiser_GUI and
# ScopedNoDenormals live). JUCE modules are INTERFACE libraries and must be
# linked PRIVATE (see JUCE "CMake API.md": PUBLIC would compile the module
# twice).
#
# NOTE: AudioEngine.h deliberately includes <juce_audio_devices/...> rather than
# <JuceHeader.h>. JuceHeader.h only exists for targets created by a juce_add_*
# function, so a plain add_executable test target could never include it -- and
# that is exactly why AudioEngine::getTapBuffer() could be declared, never
# defined, and never noticed.
target_link_libraries(HandsFreeTests PRIVATE
    GTest::gtest_main
    juce::juce_audio_devices
    juce::juce_cryptography
    juce::juce_dsp
    juce::juce_recommended_config_flags
)

target_include_directories(HandsFreeTests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

# MSVC: silence "warning C4996: 'std::copy_n': ... was declared deprecated" etc.
if(MSVC)
    target_compile_definitions(HandsFreeTests PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _SCL_SECURE_NO_WARNINGS
    )
endif()

include(GoogleTest)
gtest_discover_tests(HandsFreeTests)