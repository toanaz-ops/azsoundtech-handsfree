cmake_minimum_required(VERSION 3.22)

project(HandsFree VERSION 1.0.0)

# C++17 minimum standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add JUCE as subdirectory
add_subdirectory(external/JUCE)

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
)

# Link JUCE modules
target_link_libraries(HandsFree
    PRIVATE
        juce::juce_audio_devices
        juce::juce_audio_utils
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
