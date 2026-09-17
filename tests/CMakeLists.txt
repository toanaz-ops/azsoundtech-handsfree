include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)

# For Windows: prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# The test binary compiles every test file plus THE shared source list from the
# root CMakeLists (HANDSFREE_CORE_SOURCES) -- not a hand-maintained copy of it.
#
# It used to carry its own list of ../src/... paths, and a new component added
# to the app was simply absent here until someone noticed the link error. That
# is what happened to src/gui/SlotTabs.cpp on 2026-08-25.
add_executable(HandsFreeTests
    test_slotconfig.cpp
    test_slotpanel.cpp
    test_tuningpanel.cpp
    test_ringbuffer.cpp
    test_biquad.cpp
    test_notchchain.cpp
    test_detector.cpp
    test_peakiness.cpp
    test_audioengine.cpp
    test_deviceviewmodel.cpp
    test_gui_wiring.cpp
    test_moderail.cpp
    test_spectrumview.cpp
    test_notchlistpanel.cpp
    test_aztheme.cpp
    test_presetmanager.cpp
    test_licensemanager.cpp
    test_clocksource.cpp
    test_candidatescorer.cpp
    test_notchcontroller.cpp
    test_licenseviewmodel.cpp
    test_presetsfirstrun.cpp
    test_sessionlogger.cpp
    test_soundchecksignal.cpp
    test_loopgainestimator.cpp
    test_soundcheck_candidates.cpp
    test_soundcheckcontroller.cpp
    ${HANDSFREE_CORE_SOURCES}
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
    HandsFreeFonts
    melatonin_blur
    juce::juce_audio_devices
    juce::juce_gui_basics
    juce::juce_cryptography
    juce::juce_dsp
    juce::juce_recommended_config_flags
)

target_include_directories(HandsFreeTests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

# Task 26's two shipped presets are read off disk BY THE REAL LOADER rather
# than reproduced as string literals here -- a preset that only ever exists
# inside a test proves nothing about the file that ships. The tests therefore
# need the repo-relative location of presets/, which only CMake knows.
target_compile_definitions(HandsFreeTests PRIVATE
    HANDSFREE_PRESET_SOURCE_DIR="${CMAKE_SOURCE_DIR}/presets"
)

# The theme grep-test reads src/gui as text at run time (spec section 6: no
# hex colour literal outside src/gui/theme/), so it needs the repo-relative
# location of the GUI tree, which only CMake knows.
target_compile_definitions(HandsFreeTests PRIVATE
    HANDSFREE_GUI_SOURCE_DIR="${CMAKE_SOURCE_DIR}/src/gui"
)

# MSVC: silence "warning C4996: 'std::copy_n': ... was declared deprecated" etc.
if(MSVC)
    target_compile_definitions(HandsFreeTests PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _SCL_SECURE_NO_WARNINGS
    )
endif()

# The default 1 MB main-thread stack no longer fits the test rigs: since the
# FFT went 2048-wide, one CandidateScorer's history alone is ~512 kB, and
# HarmonicOfLockedNotchIsPenalised holds TWO Rigs (two scorers) on the stack
# at once -- measured as a segfault at 1 MB. 8 MB removes the ceiling for any
# future rig shape too.
if(MSVC)
    target_link_options(HandsFreeTests PRIVATE "/STACK:8388608")
endif()

include(GoogleTest)
gtest_discover_tests(HandsFreeTests)

# Lane D: the log summary tool runs against a checked-in fixture. Python is
# optional on a build box; without it the test SKIPS rather than fails.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME logstats_fixture
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/logstats.py
                ${CMAKE_SOURCE_DIR}/tests/fixtures/session-sample.jsonl
                --expect-notches 5 --expect-verdicts 3 --expect-false 1
                --expect-recurrence-max 2 --expect-retunes 2
                --expect-soundcheck-replaced 1)
else()
    add_test(NAME logstats_fixture COMMAND ${CMAKE_COMMAND} -E echo "SKIP: python3 not found")
    set_tests_properties(logstats_fixture PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP:")
endif()