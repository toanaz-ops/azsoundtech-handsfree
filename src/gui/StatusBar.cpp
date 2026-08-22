#include "gui/StatusBar.h"

namespace gui
{

void StatusBar::setStatus (const DeviceStatus& status, const juce::String& message)
{
    status_  = status;
    message_ = message;
    repaint();
}

void StatusBar::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().reduced (4, 2);
    auto line = area.removeFromTop (24);

    g.setFont (16.0f);
    g.setColour (juce::Colours::lightgrey);
    g.drawText ("Status:", line.removeFromLeft (58), juce::Justification::centredLeft);

    // The indicator. Dark red rather than dark grey when stopped: a stopped
    // engine mid-show is a fault, not a neutral state.
    const auto dot = line.removeFromLeft (18).withSizeKeepingCentre (10, 10);
    g.setColour (status_.running ? juce::Colours::limegreen : juce::Colours::darkred);
    g.fillEllipse (dot.toFloat());

    g.setColour (juce::Colours::white);
    g.drawText (formatStatusLine (status_), line, juce::Justification::centredLeft);

    // The reason, when there is one. This is the only path by which a device
    // failure's cause reaches the screen; before AudioEngine retained it, the
    // indicator simply went dark and the message lived in a log nobody reads
    // during a show.
    if (message_.isNotEmpty())
    {
        g.setColour (juce::Colours::orangered);
        g.setFont (14.0f);
        g.drawText (message_, area.removeFromTop (20), juce::Justification::centredLeft);
    }
}

} // namespace gui
