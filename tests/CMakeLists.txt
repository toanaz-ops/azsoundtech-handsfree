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
    ../src/dsp/Biquad.cpp
    ../src/dsp/NotchChain.cpp
    ../src/dsp/Detector.cpp
)

# Detector uses juce::dsp::FFT / WindowingFunction, so the test target needs
# the juce_dsp module. JUCE modules are INTERFACE libraries and must be linked
# PRIVATE (see JUCE "CMake API.md": PUBLIC would compile the module twice).
target_link_libraries(HandsFreeTests PRIVATE
    GTest::gtest_main
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