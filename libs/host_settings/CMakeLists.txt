# az_host_settings -- the first shared library of the platform (lane U, spec
# 2026-10-18 section 3.1). Remembers the audio device between launches; the
# voice isolator (docs/research/2026-09-06-cedar-realtime-plugins.md section 7)
# is the second product that needs exactly this.
#
# An INTERFACE target whose sources the CONSUMER compiles, the same shape as a
# JUCE module. A STATIC library linking juce::juce_core would compile
# juce_core.cpp into the library AND into the app (JUCE modules are INTERFACE
# libraries that add their sources to whoever links them), which is two copies
# of every JUCE symbol. This way the consumer's own JUCE configuration is used
# and the library still has its own target, its own include root, and no path
# back into src/.
add_library(az_host_settings INTERFACE)

target_sources(az_host_settings INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/HostSettings.cpp
)

target_include_directories(az_host_settings INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
