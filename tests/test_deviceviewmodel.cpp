// Presentation logic for the device / status GUI (Tasks 16, 17, 18).
//
// Why these tests exist at all
// ===========================
// Everything the device GUI has to get RIGHT is a decision or a format, and
// neither needs a juce::Component, a window or an open audio device:
//
//   - which driver type to select when the app starts (Task 16)
//   - what "48 kHz / 2 in / 2 out / Latency 5.3 ms" is made of (Task 18)
//   - which combo item corresponds to the rate the device is actually running
//
// Putting those in a GUI-free translation unit is the same move the project
// already made for Detector, which is thread-free so it can be driven by hand.
// A ComboBox populated by tested rules leaves the Component itself with
// nothing to be wrong about.
//
// This file therefore links only juce_core. It must NOT need juce_gui_basics.

#include <gtest/gtest.h>

#include "gui/DeviceViewModel.h"

namespace
{
// U+2014 EM DASH, written as explicit UTF-8 bytes rather than as a literal so
// the expectation does not depend on the compiler's source-charset handling.
const juce::String kEmDash = juce::String::fromUTF8 ("\xe2\x80\x94");

std::string s (const juce::String& text)
{
    return text.toStdString();  // so a gtest failure prints readable text
}
} // namespace

//==============================================================================
// Task 16 -- which driver type is selected at startup.
//
// The plan said "filter to show ASIO devices only". On this machine, and on
// CI, the ASIO SDK is absent (.gitignore excludes external/asiosdk/), so the
// ASIO type is never registered and an ASIO-only list is EMPTY. The rule is
// therefore "prefer ASIO, fall back to whatever exists", never "require ASIO".

TEST (DeviceViewModel, DefaultDeviceTypePrefersAsioWhenItIsRegistered)
{
    const juce::StringArray types { "Windows Audio", "DirectSound", "ASIO" };

    EXPECT_EQ (s (gui::chooseDefaultDeviceType (types)), "ASIO");
}

TEST (DeviceViewModel, DefaultDeviceTypeFallsBackToTheFirstTypeWhenAsioIsAbsent)
{
    // This is the shape of every machine the project currently builds on.
    const juce::StringArray types { "Windows Audio", "DirectSound" };

    EXPECT_EQ (s (gui::chooseDefaultDeviceType (types)), "Windows Audio");
}

TEST (DeviceViewModel, DefaultDeviceTypeIsEmptyWhenNoTypeIsRegisteredAtAll)
{
    EXPECT_EQ (s (gui::chooseDefaultDeviceType ({})), "");
}

//==============================================================================
// Task 18 -- the status line, spec section 6.1.

TEST (DeviceViewModel, SampleRateIsShownInKilohertz)
{
    EXPECT_EQ (s (gui::formatSampleRate (48000.0)), "48 kHz");
}

TEST (DeviceViewModel, AFractionalKilohertzRateKeepsItsDecimals)
{
    // 44100 must not round to "44 kHz" -- 44.1 and 48 are the two rates a
    // soundman actually distinguishes at a glance.
    EXPECT_EQ (s (gui::formatSampleRate (44100.0)), "44.1 kHz");
}

TEST (DeviceViewModel, AThreeDecimalRateIsNotRoundedAway)
{
    EXPECT_EQ (s (gui::formatSampleRate (11025.0)), "11.025 kHz");
}

TEST (DeviceViewModel, SampleRateShowsAPlaceholderWhenNoDeviceIsOpen)
{
    EXPECT_EQ (s (gui::formatSampleRate (0.0)), s (kEmDash));
}

TEST (DeviceViewModel, AChannelCountOfZeroRendersAsAPlaceholderNotAsTwo)
{
    // AudioEngine publishes channel counts FROM THE CALLBACK, so they read 0
    // until audio has actually flowed. Printing "2 in" before any block has
    // arrived would be a claim the engine cannot support.
    EXPECT_EQ (s (gui::formatChannelCount (0)), s (kEmDash));
}

TEST (DeviceViewModel, ANonZeroChannelCountRendersAsTheNumber)
{
    EXPECT_EQ (s (gui::formatChannelCount (2)), "2");
}

TEST (DeviceViewModel, LatencyIsConvertedFromSecondsToMilliseconds)
{
    // AudioEngine::getCurrentLatency() returns SECONDS.
    EXPECT_EQ (s (gui::formatLatency (0.0053)), "5.3 ms");
}

TEST (DeviceViewModel, TheStatusLineMatchesTheExampleInSpecSection61)
{
    gui::DeviceStatus status;
    status.running           = true;
    status.sampleRateHz      = 48000.0;
    status.numInputChannels  = 2;
    status.numOutputChannels = 2;
    status.latencySeconds    = 0.0053;

    EXPECT_EQ (s (gui::formatStatusLine (status)),
               "48 kHz / 2 in / 2 out / Latency 5.3 ms");
}

TEST (DeviceViewModel, TheStatusLineReportsZeroChannelsHonestly)
{
    gui::DeviceStatus status;
    status.running        = true;
    status.sampleRateHz   = 48000.0;
    status.latencySeconds = 0.0053;
    // channel counts left at 0: device open, no callback delivered yet.

    const juce::String line = gui::formatStatusLine (status);

    EXPECT_EQ (s (line), s ("48 kHz / " + kEmDash + " in / " + kEmDash
                            + " out / Latency 5.3 ms"));
}

TEST (DeviceViewModel, AStoppedEngineSaysStoppedRatherThanShowingStaleNumbers)
{
    gui::DeviceStatus status;
    status.running           = false;
    status.sampleRateHz      = 48000.0;   // last known values, now meaningless
    status.numInputChannels  = 2;
    status.numOutputChannels = 2;

    EXPECT_EQ (s (gui::formatStatusLine (status)), "Stopped");
}

//==============================================================================
// Task 17 -- combo items, and finding the item that matches the running device.

TEST (DeviceViewModel, ASampleRateComboItemIsThePlainNumberAsInSpecSection61)
{
    EXPECT_EQ (s (gui::formatSampleRateItem (48000.0)), "48000");
}

TEST (DeviceViewModel, ABufferSizeComboItemIsLabelledInSamples)
{
    EXPECT_EQ (s (gui::formatBufferSizeItem (64)), "64 samples");
}

TEST (DeviceViewModel, TheRunningSampleRateIsMatchedDespiteFloatingPointDrift)
{
    // getAvailableSampleRates() and getCurrentSampleRateHz() come from
    // different JUCE calls; an exact == would silently select nothing and the
    // combo would show a blank while the device ran perfectly well.
    const juce::Array<double> rates { 44100.0, 48000.0, 96000.0 };

    EXPECT_EQ (gui::indexOfSampleRate (rates, 48000.0000001), 1);
}

TEST (DeviceViewModel, AnUnsupportedSampleRateMatchesNothing)
{
    const juce::Array<double> rates { 44100.0, 48000.0 };

    EXPECT_EQ (gui::indexOfSampleRate (rates, 96000.0), -1);
}

//==============================================================================
// The banner -- spec section 8, and the reason getLastDeviceError() exists.
//
// Before commit 194a090 a device failure darkened the status indicator with the
// reason available NOWHERE in the UI: a soundman mid-show got a dead app and no
// cause. The banner is the one place a reason is allowed to reach the screen,
// and it carries two different kinds of message: the engine's device error, and
// the GUI's own report that the hardware refused a setting.

TEST (DeviceViewModel, AHealthyEngineShowsNoBanner)
{
    EXPECT_EQ (s (gui::formatDeviceBanner ("")), "");
}

TEST (DeviceViewModel, ADeviceErrorReachesTheScreenVerbatim)
{
    // Verbatim, not replaced by a generic "Audio device disconnected": the
    // driver's own words are the only diagnostic the user will ever get.
    EXPECT_EQ (s (gui::formatDeviceBanner ("ASIO driver lost")),
               "Audio device error: ASIO driver lost");
}

TEST (DeviceViewModel, ARefusedSampleRateSaysWhatIsActuallyRunning)
{
    // AudioEngine::setSampleRate() returns false and supplies NO reason string,
    // so the honest message is the fact plus the value still in force.
    EXPECT_EQ (s (gui::refusedSampleRateMessage (96000.0, 48000.0)),
               "Device refused 96000 Hz - still running at 48000 Hz");
}

TEST (DeviceViewModel, ARefusedBufferSizeSaysWhatIsActuallyRunning)
{
    EXPECT_EQ (s (gui::refusedBufferSizeMessage (32, 64)),
               "Device refused 32 samples - still running at 64 samples");
}
