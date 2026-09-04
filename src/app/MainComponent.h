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
#include "app/SlotConfig.h"
#include "dsp/ClockSource.h"
#include "gui/DeviceDrawer.h"
#include "gui/DevicePanel.h"
#include "gui/ModeBar.h"
#include "gui/ModeRail.h"
#include "gui/NotchListPanel.h"
#include "gui/SlotPanel.h"
#include "gui/SlotTabs.h"
#include "gui/SpectrumView.h"
#include "gui/StatusBar.h"
#include "gui/StatusBadge.h"
#include "gui/TuningPanel.h"
#include "gui/theme/AzTheme.h"

class MainComponent : public juce::Component,
                      private juce::Timer
{
public:
    // Minimum window size, enforced by the owning DocumentWindow (see
    // main.cpp). Raised from 720x560 with the 2026-08-25 rebuild: the floor is
    // now a TWO-COLUMN split (notch table beside the rig controls) instead of
    // a stack of full-width bands, and below ~940 the two columns start
    // truncating each other's values rather than merely tightening.
    // Wide enough that the routing table never scrolls sideways: that table
    // is the widest fixed thing in the app, and a control you have to scroll
    // to reach is one you will not reach mid-show.
    static constexpr int kMinimumWidth  = 1200;
    static constexpr int kMinimumHeight = 700;

    // The size the window OPENS at. Without this the window opened at exactly
    // the minimum -- which is the one size where the fixed rig column crowds
    // the analyser hardest, so the app's very first impression was its worst
    // possible layout.
    static constexpr int kDefaultWidth  = 1520;
    static constexpr int kDefaultHeight = 980;

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

    // Loads a preset file through the CHANNEL-AWARE PresetManager path, with
    // the open device's channel counts (stereo when no device is open):
    // every declared routing config lands in the engine, each notch is
    // adopted by the controller of its routing slot at that slot's engine
    // width, and an out-of-range slot id is logged as a warning -- never a
    // refused file. Returns false (with nothing applied) when the load fails.
    bool loadPreset (const juce::File& file);

    // THE one route a routing-table change takes (the SlotPanel lambda calls
    // this): the same detector stop/apply/start cycle as a device change,
    // then the CURRENT mode's detection gating re-applied to the changed slot,
    // so a slot enabled mid-Auto/-Soundcheck is armed immediately instead of
    // waiting for the next mode request. Public so tests drive the exact path
    // the UI uses.
    void changeSlotConfig (int slotIndex, const SlotConfig& config);

    // The drawer (device controls + settings row). Public so tests can drive
    // its controls like any other component interface.
    [[nodiscard]] gui::DeviceDrawer& getDeviceDrawer() { return deviceDrawer_; }

    // TEST ACCESSORS -- let headless tests assert rail/spectrum geometry
    // without reaching into private members.
    [[nodiscard]] juce::Rectangle<int> railBoundsForTest() const     { return modeRail_.getBounds(); }
    [[nodiscard]] juce::Rectangle<int> spectrumBoundsForTest() const { return spectrumView_.getBounds(); }
    [[nodiscard]] juce::Rectangle<int> notchListBoundsForTest() const { return notchListPanel_.getBounds(); }
    // The routing table's CONTENT rect -- a zero height here means the
    // Viewport was never given a sized child (the 2026-08-24 invisible-table
    // bug), so tests pin it to the panel's preferred height.
    [[nodiscard]] juce::Rectangle<int> slotTableBoundsForTest() const { return slotPanel_.getBounds(); }

    // Which routing slot the analyser and the notch table are showing.
    //
    // The app has always had one detector per slot; until the masthead grew a
    // selector, the GUI could only ever display slot 0's. This is the ONE
    // route that changes it -- both display panels are re-pointed together,
    // because showing one slot's spectrum beside another slot's notch list
    // would be worse than showing neither.
    //
    // Out-of-range indices, and slots with no row in the routing table, are
    // ignored rather than clamped.
    void setDisplayedSlot (int slotIndex);
    [[nodiscard]] int getDisplayedSlot() const { return displayedSlot_; }

    // TEST ACCESSOR ONLY -- drive the masthead selector directly.
    [[nodiscard]] gui::SlotTabs& getSlotTabsForTest() { return slotTabs_; }

    // The DETECTION tuning strip (brief 2026-08-24) and its bounds -- same
    // headless-test pattern as the accessors above.
    [[nodiscard]] gui::TuningPanel& getTuningPanel() { return tuningPanel_; }
    [[nodiscard]] juce::Rectangle<int> tuningPanelBoundsForTest() const { return tuningPanel_.getBounds(); }

    // TEST ACCESSOR ONLY -- lets a headless test drive the routing table's
    // row count exactly as the "+ Add slot" button does.
    [[nodiscard]] gui::SlotPanel& getSlotPanelForTest() { return slotPanel_; }

    // The two panels that read a published snapshot. Exposed for the same
    // reason the accessors above are: a headless caller has no timer running,
    // so it has to drive refreshFromSnapshot() itself. tools/snapshot.cpp uses
    // these to render the console with real detector output in it.
    [[nodiscard]] gui::SpectrumView&   getSpectrumViewForTest()   { return spectrumView_; }
    [[nodiscard]] gui::NotchListPanel& getNotchListPanelForTest() { return notchListPanel_; }

    // TEST ACCESSOR ONLY -- lets a headless test reach ONE slot's detector
    // (pump runOnce(), read the soundcheck timer). Null for an out-of-range
    // slot; never null for [0, kMaxSlots).
    NotchController* getNotchControllerForTest (int slot);

    void paint (juce::Graphics& g) override;
    void resized() override;

    // Called when this component gains (or loses) a desktop window. That is
    // the first moment there is a window to size, so it is where the opening
    // height is settled -- setSize() in the constructor only states a wish,
    // and DocumentWindow's own resize limits and border can trim it.
    void parentHierarchyChanged() override;

private:
    // How tall the bottom floor band should be given the vertical space left
    // under the transport. paint() and resized() BOTH need this figure and it
    // must agree between them, so neither computes it inline.
    [[nodiscard]] int floorHeightFor (int available) const;

    // What the floor's tallest column WANTS, before either ceiling is applied.
    [[nodiscard]] int naturalFloorHeight() const;

    // The window height at which nothing in the floor has to scroll. Used to
    // grow the window when a routing row is revealed.
    [[nodiscard]] int heightThatFitsTheFloor() const;

    // Grows the OWNING WINDOW so the floor's content fits. No-op with no
    // window (headless) and never past the display the window is on.
    void growWindowToFitFloor();

    // The floor's rect in this component's coordinates, for paint(). resized()
    // carves the same rect out of its own running area rectangle.
    [[nodiscard]] juce::Rectangle<int> floorBoundsForPaint() const;

    // Polls the engine for display. The engine publishes its state through
    // atomics precisely so a reader like this never blocks the audio thread.
    void timerCallback() override;
    void refreshStatus();

    // Applies whatever the ENGINE'S CURRENT MODE implies for ONE slot's
    // detection gate (no-op for an out-of-range or disabled slot).
    // requestMode() runs this for every slot; changeSlotConfig() for just the
    // changed one.
    void applyModeGating (int slotIndex);

    // Declared BEFORE every child component: the look and feel must outlive
    // anything that might query it during teardown. The constructor installs
    // it once with setLookAndFeel(); children inherit it through the normal
    // Component::getLookAndFeel() chain.
    az::theme::AzLookAndFeel azLookAndFeel_;

    AudioEngine engine_;

    // Clock first, controllers second: each controller holds a reference to
    // the clock. Declaration order = destruction order: the controllers'
    // threads are joined BEFORE engine_ tears down (bridge design §6.5).
    JuceMonotonicClock systemClock_;
    // One detector controller per routing slot; element i is wired to
    // engine_'s slot-i tap and command queue (see the constructor).
    //
    // Heap-held (unique_ptr) deliberately: a NotchController is ~1.1 MB
    // since stereo-aware detection doubled it (two Detectors, two
    // CandidateScorers -- each with a 128x1025-float history -- and two
    // persistence arrays), so eight BY VALUE would put ~8.8 MB on this
    // object's owner's stack -- over the default 1 MB Windows thread stack,
    // measured as a segfault in every MainComponent-constructing test.
    std::array<std::unique_ptr<NotchController>, kMaxSlots> notchControllers_;

    // Per-slot tuning mode (brief 2026-08-24): true = the slot's detector
    // follows the global DETECTION strip; false = Custom, its controller is
    // driven only by the slot panel's per-slot values. All Global at start.
    std::array<bool, kMaxSlots> slotUsesGlobalTuning_ {};

    gui::DevicePanel devicePanel_ { engine_ };

    // Old console components. Kept ALIVE but HIDDEN while the new console UI
    // lands: other lanes' tests still reference their types, and their status/
    // mode plumbing keeps running harmlessly off-display. Do not delete yet.
    gui::StatusBar   statusBar_;
    gui::ModeBar     modeBar_;

    // New console components (Tasks 1-3). Declaration order matters:
    // SpectrumView reads notchControllers_[0], DeviceDrawer re-parents
    // devicePanel_ -- both are declared above, so they outlive these.
    gui::SpectrumView spectrumView_;
    gui::ModeRail     modeRail_;
    gui::StatusBadge  statusBadge_;      // masthead furniture (see the ctor)
    gui::SlotTabs     slotTabs_;         // masthead furniture likewise
    gui::DeviceDrawer deviceDrawer_;

    // The 8-slot routing table (Task 7). Reads the engine in refresh();
    // changes come back through onSlotConfigChanged, wired to the §6.5 cycle
    // below. Hosted in a Viewport so all 8 rows scroll when the window is
    // too short to show them.
    gui::SlotPanel   slotPanel_ { engine_ };
    juce::Viewport   slotScroller_;

    // The DETECTION tuning strip (brief 2026-08-24): sits directly above the
    // routing table in both layouts; changes loop over every controller.
    gui::TuningPanel tuningPanel_;

    // The live notch list (reads notchControllers_[0] above, so it is
    // declared after it). Always visible: a fixed bottom strip.
    gui::NotchListPanel notchListPanel_;

    // A message the GUI itself produced -- a setting the hardware refused.
    // Device errors come from the engine and take precedence over it.
    juce::String panelMessage_;

    // The masthead's right-hand readout, rebuilt by refreshStatus() and drawn
    // by paint(). Held as a string rather than a child Label because it is one
    // line of mono text with no interaction -- a Label would be a component
    // and a layout pass for nothing.
    juce::String rigLine_;

    // The masthead readout turns amber when the engine is NOT running: a
    // stopped device mid-show is a fault, not a neutral state.
    bool rigIsHealthy_ = false;

    // The two readouts added beside the protection badge, both rebuilt by
    // refreshStatus() so paint() never formats a string.
    //
    // deviceLine_ answers "is the interface there at all?" -- separate from
    // the protection badge, which answers "is it filtering?". Those are
    // different questions and a stopped device with BYPASSED showing looked
    // like a deliberate choice rather than a fault.
    //
    // cpuLine_ is the AUDIO CALLBACK's share of its budget, not the process's
    // share of the machine. That is the figure that predicts a dropout.
    juce::String deviceLine_ { "NO DEVICE" };
    juce::String cpuLine_;
    bool deviceIsUp_ = false;
    bool cpuIsHot_   = false;

    static constexpr int kDeviceChipWidth = 128;
    static constexpr int kCpuChipWidth    = 86;

    // Above this share of the callback budget the figure turns amber: past
    // roughly three quarters, a transient spike is what produces a dropout.
    static constexpr double kCpuWarnFraction = 0.75;

    // The slot both display panels are pointed at. 0 until the user picks
    // another, which is the slot every rig has.
    int displayedSlot_ = 0;

    // Same figure paint() and resized() both need: the cell the status badge
    // occupies at the masthead's right end.
    static constexpr int kBadgeWidth     = 132;
    static constexpr int kBadgeHeight    = 26;
    // The brand block at the masthead's left end: the sodium bar, the product
    // name, then the company. kMarkWidth is what the rig readout starts after,
    // so it must cover the whole block including the gaps between its parts.
    static constexpr int kNameWidth      = 132;
    static constexpr int kCompanyWidth   = 104;
    static constexpr int kMarkWidth      = 3 + 10 + kNameWidth + 12 + kCompanyWidth;
    // Window-edge padding. Wider than the 8 px grid gap on purpose: the app
    // frame needs a margin the inner grid does not.
    static constexpr int kEdgePad        = 12;
    // Share of the floor's width the notch table takes; the rig column gets
    // the rest.
    //
    // The design study splits these 1.25 : 1 -- the NOTCH TABLE is the wider
    // column, because it is the answer and the rig is the setup. The first
    // implementation inverted it and gave the table 38 %.
    //
    // 0.44 rather than the study's 0.556. The study's rig column held two
    // summarised rows; the shipped one holds a full 8-lane routing table, and
    // at 0.52 that table scrolled sideways while the notch column sat half
    // empty beside it -- the notch table is only ever as tall as the number of
    // notches, so extra WIDTH there buys nothing. The width goes where the
    // controls are. See docs/spec-ui-mockup.md section 5.
    static constexpr float kNotchColumnFraction = 0.44f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
