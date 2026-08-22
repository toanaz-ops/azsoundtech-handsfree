// Mode buttons -- Task 23, PARTIALLY implemented on purpose. See the comment
// on soundcheckButton below and in ModeBar.cpp.

#pragma once

// Module headers rather than <JuceHeader.h>: JuceHeader.h is generated only for
// targets created with a juce_add_* function, so including it here would make
// this component impossible to compile into the plain add_executable test
// target -- the same reason AudioEngine.h is written this way.
#include <juce_gui_basics/juce_gui_basics.h>

#include "app/AudioEngine.h"

#include <functional>

namespace gui
{

class ModeBar : public juce::Component
{
public:
    ModeBar();

    // Fired when the USER picks a mode. setDisplayedMode() deliberately does
    // NOT fire it -- see there.
    std::function<void (AudioEngine::Mode)> onModeRequested;

    // Reflect the engine's actual mode on screen without asking for a change.
    void setDisplayedMode (AudioEngine::Mode mode);

    void resized() override;

    // Public because they ARE this component's interface: the owner wires
    // nothing else, and the wiring test drives them directly.
    //
    // "Run Soundcheck (15s)" carries no countdown, and that is deliberate.
    // Owner decision D-06 freezes the detector's timers while the tap is dead,
    // so a GUI-side juce::Timer counting wall-clock would disagree with the
    // detector and show a countdown that does not match what the app is doing.
    // The remaining time has to come from the detector's
    // getSoundcheckSecondsRemaining() (bridge design section 4), which does not
    // exist yet. Setting the mode is the whole of what is wired here.
    juce::TextButton soundcheckButton { "Run Soundcheck (15s)" };
    juce::TextButton autoButton       { "Auto" };
    juce::TextButton bypassButton     { "Bypass" };

private:
    void request (AudioEngine::Mode mode);

    juce::Label modeLabel_ { {}, "Mode:" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModeBar)
};

} // namespace gui
