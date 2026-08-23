// The application's single window content -- and, as of this lane, the only
// place in the program that owns an AudioEngine.
//
// Before this, nothing anywhere constructed one: Tasks 8 and 9 built the engine
// and its post-notch tap, 75 tests passed, and the shipped binary was an
// 800x600 window that drew a string and processed no audio at all.
//
// Ownership is deliberately shallow -- an AudioEngine held BY VALUE, not a
// singleton and not a global. The audio/detector bridge design (section 6) has
// MainComponent owning both the engine and the future NotchController and
// wiring them together; a singleton would have to be undone to get there.

#pragma once

// Module headers rather than <JuceHeader.h>. JuceHeader.h is generated only for
// targets created with a juce_add_* function, so a plain add_executable test
// target could never include it -- which is exactly how AudioEngine::
// getTapBuffer() was once declared, never defined, and never noticed. Every
// component in this app is therefore reachable from the test target.
#include <juce_gui_basics/juce_gui_basics.h>

#include "app/AudioEngine.h"
#include "app/NotchController.h"
#include "dsp/ClockSource.h"
#include "gui/DevicePanel.h"
#include "gui/ModeBar.h"
#include "gui/StatusBar.h"

class MainComponent : public juce::Component,
                      private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    // The engine this window owns. The bridge design (section 6) needs this to
    // hand the tap and the command queue to a NotchController.
    AudioEngine& getAudioEngine();

    // Open an audio device and start processing.
    //
    // Deliberately NOT done in the constructor. Opening a device is a side
    // effect on real hardware; keeping it out of construction is what lets a
    // test build the whole application object on a machine with no audio
    // interface, and what lets a future caller decide the device from a preset
    // before anything is opened.
    void startAudio();

    // Route a mode request to the engine. Named rather than inlined into the
    // ModeBar lambda because Soundcheck will have to do more here once the
    // detector exists.
    void requestMode (AudioEngine::Mode mode);

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    // Polls the engine for display. The engine publishes its state through
    // atomics precisely so a reader like this never blocks the audio thread.
    void timerCallback() override;
    void refreshStatus();

    AudioEngine engine_;

    // Clock first, controller second: the controller holds a reference to it.
    // Declaration order = destruction order: notchController_'s thread is
    // joined BEFORE engine_ tears down (bridge design §6.5).
    JuceMonotonicClock systemClock_;
    NotchController notchController_;

    gui::DevicePanel devicePanel_ { engine_ };
    gui::StatusBar   statusBar_;
    gui::ModeBar     modeBar_;

    // A message the GUI itself produced -- a setting the hardware refused.
    // Device errors come from the engine and take precedence over it.
    juce::String panelMessage_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
