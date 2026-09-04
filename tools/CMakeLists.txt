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
    HandsFreeFonts
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

# PeakinessSweep -- offline peakiness distribution sweep, no window, no device.
#
# Same add_executable / target_sources / target_include_directories /
# MSVC-options STRUCTURE as HandsFreeSnapshot above, but a much smaller
# source list: this tool drives only Detector and PeakinessAnalyzer, not the
# whole app. Reusing HANDSFREE_CORE_SOURCES here would pull in MainComponent,
# SpectrumView, AudioEngine and the rest of the GUI/app layer -- none of which
# this tool touches -- and force-link juce_gui_basics, juce_graphics,
# juce_audio_devices and juce_cryptography for no reason. Detector.cpp and
# PeakinessAnalyzer.cpp compile standalone against juce_dsp and
# LockFreeRingBuffer.h (a header-only template), so that is all that is
# listed and linked.
add_executable(PeakinessSweep
    peakiness_sweep.cpp
)

target_sources(PeakinessSweep PRIVATE
    ${CMAKE_SOURCE_DIR}/src/dsp/Detector.cpp
    ${CMAKE_SOURCE_DIR}/src/dsp/Detector.h
    ${CMAKE_SOURCE_DIR}/src/dsp/PeakinessAnalyzer.cpp
    ${CMAKE_SOURCE_DIR}/src/dsp/PeakinessAnalyzer.h
    ${CMAKE_SOURCE_DIR}/src/dsp/LockFreeRingBuffer.h
)

target_include_directories(PeakinessSweep PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

# Detector uses juce_dsp for its FFT and windowing function; nothing here
# needs a GUI module, an audio device, graphics, fonts or cryptography.
target_link_libraries(PeakinessSweep PRIVATE
    juce::juce_dsp
    juce::juce_recommended_config_flags
)

if(MSVC)
    target_compile_definitions(PeakinessSweep PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _SCL_SECURE_NO_WARNINGS
    )

    # Same stack margin as the other tool target; this one's own call depth
    # is much shallower, but there is no reason to run it any tighter.
    target_link_options(PeakinessSweep PRIVATE "/STACK:8388608")
endif()
