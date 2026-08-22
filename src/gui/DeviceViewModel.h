// Presentation logic for the device / status GUI -- the decisions and the
// formats, with no juce::Component anywhere.
//
// Why this is a separate translation unit
// =======================================
// The test target is a plain add_executable and links only juce_core,
// juce_audio_devices and juce_dsp. Keeping the rules here means Task 16's
// "which driver type do we select", Task 17's "which combo item is the device
// actually running" and Task 18's status format are all testable with no
// window, no message loop and no audio device -- the same reason Detector was
// kept thread-free.
//
// Everything below is a pure function of its arguments. Read the AudioEngine
// once on the message thread, fill in a DeviceStatus, format it here.

#pragma once

// juce_core only, deliberately. If this file ever needs juce_gui_basics, the
// logic being added belongs in the Component instead.
#include <juce_core/juce_core.h>

namespace gui
{

// One snapshot of everything the status line shows, read from AudioEngine in a
// single pass on the message thread so the formatting below cannot observe a
// half-updated engine.
struct DeviceStatus
{
    bool   running            = false;
    double sampleRateHz       = 0.0;
    int    numInputChannels   = 0;
    int    numOutputChannels  = 0;
    double latencySeconds     = 0.0;
};

// Which driver type to select at startup: ASIO when it is registered,
// otherwise the first type that is. NOT "ASIO only" -- the ASIO SDK is absent
// from this repo by licence, so an ASIO-only list is empty on every machine
// the project currently builds on.
juce::String chooseDefaultDeviceType (const juce::StringArray& typeNames);

// "48 kHz", "44.1 kHz", "11.025 kHz"; the placeholder when no device is open.
juce::String formatSampleRate (double hz);

// The count, or the placeholder for 0. AudioEngine publishes channel counts
// from the audio callback, so 0 means "no block has arrived yet" -- printing
// "2 in" there would be a claim the engine cannot support.
juce::String formatChannelCount (int numChannels);

// AudioEngine::getCurrentLatency() is in SECONDS.
juce::String formatLatency (double seconds);

// Spec 6.1: "48 kHz / 2 in / 2 out / Latency 5.3 ms".
juce::String formatStatusLine (const DeviceStatus& status);

// Combo items. Spec 6.1 shows "[48000 v]" and "[64 samples v]".
juce::String formatSampleRateItem (double hz);
juce::String formatBufferSizeItem (int numSamples);

// The banner, spec section 8. Empty message means healthy, so the banner is
// hidden; anything else reaches the screen VERBATIM. This is the only path by
// which a reason gets in front of the user -- before it existed a device
// failure just darkened the indicator and the cause lived in a log nobody
// reads mid-show.
juce::String formatDeviceBanner (const juce::String& message);

// AudioEngine::setSampleRate() / setBufferSize() return false when the
// hardware refuses, and supply NO reason string. The honest thing the GUI can
// say is the fact plus the value still in force, so the user is never left
// looking at a setting the device is not running.
juce::String refusedSampleRateMessage (double requestedHz, double actualHz);
juce::String refusedBufferSizeMessage (int requestedSamples, int actualSamples);

// Index of the rate the device is running, or -1. Tolerant rather than ==,
// because the available list and the current rate reach us through different
// JUCE calls and a near-miss would leave the combo blank over a healthy device.
int indexOfSampleRate (const juce::Array<double>& rates, double rateHz);

} // namespace gui
