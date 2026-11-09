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
    test_dsp_boundary.cpp
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
    test_hostsettings.cpp
    test_windowfit.cpp
    test_appsettings.cpp
    test_dumbstatus.cpp
    test_lane_u_wiring.cpp
    test_setupwizardmodel.cpp
    test_afc.cpp
    test_copilotfindings.cpp
    test_sidecarrunner.cpp
    test_copilotask.cpp
    test_copilotanswer.cpp
    test_copilotcredentialstore.cpp
    test_copilotpanel.cpp
    test_copilotsetup.cpp
    test_candidateclassifier.cpp
    # Lane A research code (not in the app, see tools/CMakeLists.txt AfcSim).
    ${CMAKE_SOURCE_DIR}/tools/afc/AfcCanceller.cpp
    ${CMAKE_SOURCE_DIR}/tools/afc/AfcSupervisor.cpp
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
    az_host_settings
    az_rtneural_headers
    juce::juce_audio_devices
    juce::juce_gui_basics
    juce::juce_cryptography
    juce::juce_dsp
    juce::juce_recommended_config_flags
)

# CopilotCredentialStore calls CryptProtectData / CryptUnprotectData.
if(WIN32)
    target_link_libraries(HandsFreeTests PRIVATE crypt32)
endif()

target_include_directories(HandsFreeTests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tools/afc
)

# Task 26's two shipped presets are read off disk BY THE REAL LOADER rather
# than reproduced as string literals here -- a preset that only ever exists
# inside a test proves nothing about the file that ships. The tests therefore
# need the repo-relative location of presets/, which only CMake knows.
target_compile_definitions(HandsFreeTests PRIVATE
    HANDSFREE_PRESET_SOURCE_DIR="${CMAKE_SOURCE_DIR}/presets"
    HANDSFREE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

# The theme grep-test reads src/gui as text at run time (spec section 6: no
# hex colour literal outside src/gui/theme/), so it needs the repo-relative
# location of the GUI tree, which only CMake knows.
target_compile_definitions(HandsFreeTests PRIVATE
    HANDSFREE_GUI_SOURCE_DIR="${CMAKE_SOURCE_DIR}/src/gui"
    HANDSFREE_DSP_SOURCE_DIR="${CMAKE_SOURCE_DIR}/src/dsp"
)

# Same two switches as the app target (CMakeLists.txt): nothing here fetches a
# URL or embeds a browser, and on Linux JUCE_USE_CURL=1 makes juce_core link
# against libcurl, so a machine without it failed to link this target while the
# app linked fine (review 2026-10-20, M10).
target_compile_definitions(HandsFreeTests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
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

# Lane C1a, Q5 condition 1: score() makes zero global operator new / delete
# calls. SEPARATE executable because it replaces every global allocation
# function -- doing that in HandsFreeTests would override the sanitizer
# allocators of the nightly ASan/TSan jobs. Compiles the classifier source
# directly (no detector, no GUI): the class depends only on juce_core,
# juce_audio_basics and RTNeural.
# OFF in the sanitizer jobs (build.yml): the global operator new replacement must
# never be built under ASan/TSan, and those jobs build only HandsFreeTests then run
# an unfiltered ctest, so the test must not even be registered there. A sanitizer
# flag in CMAKE_CXX_FLAGS turns it off by itself as a second guard.
option(HANDSFREE_BUILD_ALLOC_TEST "Build the operator-new counting test" ON)
if(CMAKE_CXX_FLAGS MATCHES "-fsanitize")
    set(HANDSFREE_BUILD_ALLOC_TEST OFF)
endif()
if(HANDSFREE_BUILD_ALLOC_TEST)
    add_executable(CandidateClassifierAllocTest
        candidate_classifier_alloc_test.cpp
        ${CMAKE_SOURCE_DIR}/src/app/CandidateClassifier.cpp
    )
    target_link_libraries(CandidateClassifierAllocTest PRIVATE
        GTest::gtest_main
        az_rtneural_headers
        juce::juce_audio_basics
        juce::juce_recommended_config_flags
    )
    target_include_directories(CandidateClassifierAllocTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(CandidateClassifierAllocTest PRIVATE
        HANDSFREE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
    )
    if(MSVC)
        target_compile_definitions(CandidateClassifierAllocTest PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    add_test(NAME CandidateClassifierAllocTest COMMAND CandidateClassifierAllocTest)
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
                --expect-soundcheck-replaced 1 --expect-superseded 0)
    # A Set onto a key whose Clear never reached the log closes the old record
    # instead of leaving it "still active" for the whole session.
    add_test(NAME logstats_superseded
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/logstats.py
                ${CMAKE_SOURCE_DIR}/tests/fixtures/session-superseded.jsonl
                --expect-notches 2 --expect-superseded 1)
    # Lane C0: candidate_features lines are counted (total and per slot); the
    # old fixtures above carry none and must keep summarising as before.
    add_test(NAME logstats_candidates
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/logstats.py
                ${CMAKE_SOURCE_DIR}/tests/fixtures/session-candidates.jsonl
                --expect-notches 1 --expect-candidates 3)
    # Lane L: the copilot's finding rules. Offline; the --ask tools are
    # covered only where the anthropic package happens to be installed.
    add_test(NAME copilot_findings
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_copilot.py)
    # Lane C1a: the pure-Python reference of the classifier (normalise + MLP)
    # must reproduce the expected scores stored next to the C++ parity test, so
    # both implementations are pinned to the same numbers. -B: no __pycache__.
    add_test(NAME classifier_reference_parity
        COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/tools/nn/reference_forward.py
                --weights ${CMAKE_SOURCE_DIR}/tests/fixtures/classifier-fixture-v1.json
                --parity ${CMAKE_SOURCE_DIR}/tests/fixtures/classifier-parity-v1.json)
else()
    add_test(NAME logstats_fixture COMMAND ${CMAKE_COMMAND} -E echo "SKIP: python3 not found")
    set_tests_properties(logstats_fixture PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP:")
    add_test(NAME logstats_superseded COMMAND ${CMAKE_COMMAND} -E echo "SKIP: python3 not found")
    set_tests_properties(logstats_superseded PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP:")
    add_test(NAME logstats_candidates COMMAND ${CMAKE_COMMAND} -E echo "SKIP: python3 not found")
    set_tests_properties(logstats_candidates PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP:")
    add_test(NAME copilot_findings COMMAND ${CMAKE_COMMAND} -E echo "SKIP: python3 not found")
    set_tests_properties(copilot_findings PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP:")
    add_test(NAME classifier_reference_parity COMMAND ${CMAKE_COMMAND} -E echo "SKIP: python3 not found")
    set_tests_properties(classifier_reference_parity PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP:")
endif()