# HandsFreeSnapshot -- renders the console to PNG with no window.
#
# A plain add_executable rather than a juce_add_gui_app: it has no bundle, no
# resources and no JuceHeader.h, and it compiles the SAME sources the app and
# the test binary do (HANDSFREE_CORE_SOURCES, defined in the root list).
add_executable(HandsFreeSnapshot
    snapshot.cpp
)

target_sources(HandsFreeSnapshot PRIVATE
    ${HANDSFREE_CORE_SOURCES}
)

target_include_directories(HandsFreeSnapshot PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

# Same module set as the test target, for the same reasons documented there:
# AudioEngine needs juce_audio_devices, Detector needs juce_dsp, and the
# LookAndFeel and PNG writer come with juce_gui_basics/juce_graphics.
target_link_libraries(HandsFreeSnapshot PRIVATE
    melatonin_blur
    juce::juce_audio_devices
    juce::juce_gui_basics
    juce::juce_cryptography
    juce::juce_dsp
    juce::juce_recommended_config_flags
)

if(MSVC)
    target_compile_definitions(HandsFreeSnapshot PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _SCL_SECURE_NO_WARNINGS
    )

    # Same 8 MB stack the test target needs: a NotchController's scorer history
    # is ~512 kB since the FFT went 2048-wide, and MainComponent builds eight.
    target_link_options(HandsFreeSnapshot PRIVATE "/STACK:8388608")
endif()
