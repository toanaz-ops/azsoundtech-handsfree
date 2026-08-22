#include "gui/ModeBar.h"

namespace gui
{
namespace
{
// Any non-zero id makes the three buttons mutually exclusive, and makes JUCE
// keep the pressed one ON when it is clicked again rather than toggling it off.
constexpr int kModeRadioGroupId = 0x4d6f6465;  // 'Mode'

// Deliberately a hue nothing else in the window uses, not a lighter grey.
const juce::Colour kActiveModeColour { 0xff4fc3f7 };
} // namespace

ModeBar::ModeBar()
{
    addAndMakeVisible (modeLabel_);

    for (auto* button : { &soundcheckButton, &autoButton, &bypassButton })
    {
        button->setClickingTogglesState (true);
        button->setRadioGroupId (kModeRadioGroupId);

        // The default LookAndFeel's "on" colour is close enough to its "off"
        // colour on a dark scheme that all three buttons read as identical --
        // found by running the app and looking at it, not by compiling it.
        // Which mode is live is the difference between notches being applied to
        // a live PA and not, so it gets an unmistakable colour rather than a
        // shade.
        button->setColour (juce::TextButton::buttonOnColourId, kActiveModeColour);
        button->setColour (juce::TextButton::textColourOnId,   juce::Colours::black);

        addAndMakeVisible (*button);
    }

    soundcheckButton.onClick = [this] { request (AudioEngine::Mode::Soundcheck); };
    autoButton      .onClick = [this] { request (AudioEngine::Mode::Auto); };
    bypassButton    .onClick = [this] { request (AudioEngine::Mode::Bypass); };

    // No button is pre-selected here. The owner calls setDisplayedMode() with
    // the engine's actual mode, so the bar never asserts a mode of its own.
}

void ModeBar::setDisplayedMode (AudioEngine::Mode mode)
{
    // dontSendNotification is the whole point: this reflects the engine, it
    // does not drive it. The status refresh calls this several times a second,
    // and a notification here would re-issue the mode on every tick -- fighting
    // any mode set from anywhere but these three buttons, which is exactly what
    // the detector will do once Soundcheck is real.
    soundcheckButton.setToggleState (mode == AudioEngine::Mode::Soundcheck, juce::dontSendNotification);
    autoButton      .setToggleState (mode == AudioEngine::Mode::Auto,       juce::dontSendNotification);
    bypassButton    .setToggleState (mode == AudioEngine::Mode::Bypass,     juce::dontSendNotification);
}

void ModeBar::request (AudioEngine::Mode mode)
{
    if (onModeRequested != nullptr)
        onModeRequested (mode);
}

void ModeBar::resized()
{
    auto row = getLocalBounds().reduced (0, 4);

    modeLabel_      .setBounds (row.removeFromLeft (60));
    soundcheckButton.setBounds (row.removeFromLeft (170).reduced (2, 0));
    autoButton      .setBounds (row.removeFromLeft (90) .reduced (2, 0));
    bypassButton    .setBounds (row.removeFromLeft (90) .reduced (2, 0));
}

} // namespace gui
