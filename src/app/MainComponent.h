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
#include <juce_data_structures/juce_data_structures.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

#include "app/AudioEngine.h"
#include "app/NotchController.h"
#include "dsp/ClockSource.h"
#include "gui/DeviceDrawer.h"
#include "gui/DevicePanel.h"
#include "gui/ModeBar.h"
#include "gui/ModeRail.h"
#include "gui/SpectrumView.h"
#include "gui/StatusBar.h"
#include "gui/StatusBadge.h"
#include "gui/theme/AzTheme.h"

class MainComponent : public juce::Component,
                      private juce::Timer
{
public:
    // Minimum window size, enforced by the owning DocumentWindow (see
    // main.cpp): must fit the 96 px rail next to / above a usable spectrum
    // with no overlap at either layout (spec section 3).
    static constexpr int kMinimumWidth  = 720;
    static constexpr int kMinimumHeight = 560;

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

    // L1/L2 (spec G-2): switches the arrangement, persists it under the
    // ApplicationProperties key "layout", default Performance.
    void setLayout (gui::ScreenLayout layout);
    [[nodiscard]] gui::ScreenLayout getLayout() const { return layout_; }

    // The drawer (device controls + settings row). Public so tests can drive
    // its toggle buttons like any other component interface.
    [[nodiscard]] gui::DeviceDrawer& getDeviceDrawer() { return deviceDrawer_; }

    // TEST ACCESSORS -- let headless tests assert rail/spectrum geometry
    // without reaching into private members.
    [[nodiscard]] juce::Rectangle<int> railBoundsForTest() const     { return modeRail_.getBounds(); }
    [[nodiscard]] juce::Rectangle<int> spectrumBoundsForTest() const { return spectrumView_.getBounds(); }

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    // Polls the engine for display. The engine publishes its state through
    // atomics precisely so a reader like this never blocks the audio thread.
    void timerCallback() override;
    void refreshStatus();

    // Applies a layout choice to the EXISTING components (rail orientation,
    // drawer behaviour, button reflection). No component is ever destroyed or
    // recreated on a switch -- spec section 3.
    void applyLayoutState (gui::ScreenLayout layout);

    // Declared BEFORE every child component: the look and feel must outlive
    // anything that might query it during teardown. The constructor installs
    // it once with setLookAndFeel(); children inherit it through the normal
    // Component::getLookAndFeel() chain.
    az::theme::AzLookAndFeel azLookAndFeel_;

    AudioEngine engine_;

    // Clock first, controller second: the controller holds a reference to it.
    // Declaration order = destruction order: notchController_'s thread is
    // joined BEFORE engine_ tears down (bridge design §6.5).
    JuceMonotonicClock systemClock_;
    NotchController notchController_;

    gui::DevicePanel devicePanel_ { engine_ };

    // Old console components. Kept ALIVE but HIDDEN while the new console UI
    // lands: other lanes' tests still reference their types, and their status/
    // mode plumbing keeps running harmlessly off-display. Do not delete yet.
    gui::StatusBar   statusBar_;
    gui::ModeBar     modeBar_;

    // New console components (Tasks 1-3). Declaration order matters:
    // SpectrumView reads notchController_, DeviceDrawer re-parents devicePanel_
    // -- both are declared above, so they outlive these.
    gui::SpectrumView spectrumView_;
    gui::ModeRail     modeRail_;
    gui::StatusBadge  statusBadge_;      // hosted inside deviceDrawer_'s header
    gui::DeviceDrawer deviceDrawer_;

    // Ruling R-1: NotchListPanel does not exist yet (Task 4). This slot stays
    // null this lane; resized() tolerates that.
    std::unique_ptr<juce::Component> notchListSlot_;

    // Layout persistence (spec section 3): the choice survives app restarts.
    juce::ApplicationProperties appProperties_;
    gui::ScreenLayout           layout_ = gui::ScreenLayout::Performance;

    // A message the GUI itself produced -- a setting the hardware refused.
    // Device errors come from the engine and take precedence over it.
    juce::String panelMessage_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
