#include "gui/DeviceViewModel.h"

#include <cmath>

namespace gui
{
namespace
{
// U+2014 EM DASH as explicit UTF-8 bytes. A literal would depend on the
// compiler's source-charset handling; these three bytes do not.
juce::String placeholder()
{
    return juce::String::fromUTF8 ("\xe2\x80\x94");
}

// juce::String (value, n) produces FIXED decimals, so 48 kHz would come out as
// "48.000 kHz". Trimming the trailing zeros and then the orphaned point is
// exact for every rate a device actually offers -- 48, 44.1, 11.025 -- with no
// rounding rule that could quietly drop a digit.
juce::String withoutTrailingZeros (double value)
{
    juce::String text (value, 3);

    if (text.containsChar ('.'))
        text = text.trimCharactersAtEnd ("0").trimCharactersAtEnd (".");

    return text;
}
} // namespace

juce::String chooseDefaultDeviceType (const juce::StringArray& typeNames)
{
    if (typeNames.isEmpty())
        return {};

    if (typeNames.contains ("ASIO"))
        return "ASIO";

    return typeNames[0];
}

juce::String formatSampleRate (double hz)
{
    if (hz <= 0.0)
        return placeholder();

    return withoutTrailingZeros (hz / 1000.0) + " kHz";
}

juce::String formatChannelCount (int numChannels)
{
    if (numChannels <= 0)
        return placeholder();

    return juce::String (numChannels);
}

juce::String formatLatency (double seconds)
{
    return juce::String (seconds * 1000.0, 1) + " ms";
}

juce::String formatStatusLine (const DeviceStatus& status)
{
    // A stopped engine still holds the last device's rate and channel counts.
    // Rendering them would show a soundman a healthy-looking line over an app
    // that is passing no audio at all.
    if (! status.running)
        return "Stopped";

    return formatSampleRate (status.sampleRateHz)
         + " / " + formatChannelCount (status.numInputChannels)  + " in"
         + " / " + formatChannelCount (status.numOutputChannels) + " out"
         + " / Latency " + formatLatency (status.latencySeconds);
}

juce::String formatSampleRateItem (double hz)
{
    return withoutTrailingZeros (hz);
}

juce::String formatBufferSizeItem (int numSamples)
{
    return juce::String (numSamples) + " samples";
}

juce::String formatDeviceBanner (const juce::String& message)
{
    if (message.isEmpty())
        return {};

    return "Audio device error: " + message;
}

juce::String refusedSampleRateMessage (double requestedHz, double actualHz)
{
    return "Device refused " + withoutTrailingZeros (requestedHz) + " Hz"
         + " - still running at " + withoutTrailingZeros (actualHz) + " Hz";
}

juce::String refusedBufferSizeMessage (int requestedSamples, int actualSamples)
{
    return "Device refused " + formatBufferSizeItem (requestedSamples)
         + " - still running at " + formatBufferSizeItem (actualSamples);
}

int indexOfSampleRate (const juce::Array<double>& rates, double rateHz)
{
    for (int i = 0; i < rates.size(); ++i)
    {
        if (std::abs (rates[i] - rateHz) < 0.5)
            return i;
    }

    return -1;
}

} // namespace gui
