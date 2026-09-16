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
#include "app/SessionLogger.h"
#include "app/SlotConfig.h"
#include "app/SoundcheckController.h"
#include "dsp/ClockSource.h"
#include "gui/DeviceDrawer.h"
#include "gui/DevicePanel.h"
#include "gui/ModeBar.h"
#include "gui/ModeRail.h"
#include "gui/NotchListPanel.h"
#include "gui/SlotPanel.h"
#include "gui/SlotTabs.h"
#include "gui/SoundcheckPanel.h"
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

    // Gathers the CURRENT published notch state (what the user sees) across all
    // routing slots and writes it as a preset file. Returns the saveToFile
    // result: PresetManager validates FIRST and writes nothing if invalid, so a
    // false here means the file was NOT created -- an app that saves a preset it
    // cannot reopen is worse than one that refuses.
    //
    // The sample rate written is the one the notches were PUBLISHED at
    // (SnapshotBuffer::sampleRate), never 0: saveToFile checks each notch's freq
    // against that rate's Nyquist, and a notch detected at rate R is valid at R
    // by construction. One PresetNotch per (slot, lane, index) -- lane S ended
    // the old "one entry per slot, lanes deduped" shape; each lane of a
    // stereo slot now detects and places its own notches independently.
    bool savePreset (const juce::File& file);

    // Lane D (data loop): the session log. Never started in the constructor --
    // 26 tests and the snapshot tool construct MainComponent, and a ctor start
    // would write to (and prune) the developer's real %APPDATA% logs. main.cpp
    // calls setAppVersion() once at startup (the CMake project() version is the
    // one true source, D-8) and startSessionLog() AFTER startAudio() so the
    // header names the device actually in use.
    void setAppVersion (const juce::String& version);
    bool startSessionLog (const juce::File& directory);

    // Puts one line in the status bar and the masthead readout -- the SAME
    // route devicePanel_.onMessage takes. Public because main.cpp is the one
    // caller that has something to say the component itself cannot know: that
    // startSessionLog() refused (I-3).
    void showMessage (const juce::String& message);
    void stopSessionLog();
    [[nodiscard]] juce::File sessionLogFileForTest() const;
    [[nodiscard]] int lastLoadSkippedNotchesForTest() const;

    // Injectable for tests: the default opens a native async FileChooser and
    // invokes onPicked with the file the user picked (a cancel is a no-op). A
    // headless test replaces these with a fake that hands onPicked a known temp
    // file. Mirrors ModeRail::confirmHook -- the only way to exercise the
    // button->chooser->load/save route with JUCE_MODAL_LOOPS_PERMITTED off.
    std::function<void (std::function<void (const juce::File&)> onPicked)> presetLoadChooser;
    std::function<void (std::function<void (const juce::File&)> onPicked)> presetSaveChooser;

    // LANE M. THE CONSOLE HAS THREE LOCK STATES, NOT TWO (spec 4.3, review
    // I-3). "Locked" as a bool put the operator's emergency control behind a
    // results strip: BYPASS is how a show is saved when something goes wrong,
    // and it must come back the moment the sweep stops.
    enum class SoundcheckLock
    {
        // Idle with nothing on the strip. Everything is live.
        None,

        // A run is in flight. Everything that could move the chain the run is
        // measuring is dead: the mode switches, CLEAR ALL, DO, the preset row,
        // the device controls, the routing table, the DETECTION strip and the
        // notch table's verdict buttons (a FALSE verdict is a one-click
        // clearNotch -- a partial CLEAR ALL by another name).
        Measuring,

        // Results, and the Applied report that follows an AP DUNG. The sound
        // has stopped, so the MODE SWITCHES AND CLEAR ALL COME BACK. What stays
        // dead is everything that would invalidate or silently consume the
        // proposals still on screen: DO itself, PRESET LOAD (adoptPreset
        // overwrites without checking n.active), the routing table (Task 7's
        // stale-linked ruling), the DETECTION strip (RunParams were frozen at
        // Arm, so a DEPTH move here would have APPLY writing against a ceiling
        // the run never saw) and the verdict buttons.
        Pending
    };

    // LANE M Task 10. The confirmation the operator answers before ONE SAMPLE
    // is emitted (spec 4.3). Injectable exactly as presetLoadChooser is, and for
    // the same reason: JUCE_MODAL_LOOPS_PERMITTED is off, so a headless test
    // cannot answer a native box. `text` is the honest level statement plus the
    // total duration; the callback is invoked ONCE, true only on OK.
    //
    // A null hook means NO RUN, never a silent run: beginSoundcheck() refuses
    // rather than arming an unconfirmed sweep.
    std::function<void (const juce::String& text,
                        std::function<void (bool)> onAnswer)> soundcheckConfirmHook;

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

    // Lane M Task 9. The rail itself, not just its bounds: a headless caller
    // and tools/snapshot.cpp have to put the transport into its LOCKED state
    // (F10) to render it, and that is three setters on the rail.
    [[nodiscard]] gui::ModeRail& getModeRailForTest() { return modeRail_; }
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

    // Lane M Task 9. The strip is a plain-data component: it is laid out and
    // rendered by this console, but nothing in Task 9 CONNECTS it to a
    // SoundcheckController -- that wiring (the confirm dialog, the poll, APPLY) is
    // Task 10. Exposed now so a headless caller and tools/snapshot.cpp can put
    // the console into a Running or a Results state and look at it, which is
    // the only way a GUI change gets reported (CLAUDE.md, 2026-08-25).
    [[nodiscard]] gui::SoundcheckPanel& getSoundcheckPanelForTest() { return soundcheckPanel_; }
    [[nodiscard]] gui::NotchListPanel& getNotchListPanelForTest() { return notchListPanel_; }

    // TEST ACCESSOR ONLY -- lets a headless test reach ONE slot's detector
    // (pump runOnce(), read the soundcheck timer). Null for an out-of-range
    // slot; never null for [0, kMaxSlots).
    NotchController* getNotchControllerForTest (int slot);

    //==========================================================================
    // LANE M Task 10 -- TEST ACCESSORS ONLY. Modelled on getSlotPanelForTest
    // above; every one of them is a member the wiring owns and nothing else can
    // reach. getModeRailForTest() already existed (Task 9) and is NOT
    // re-declared here.

    // The device panel carries onBeforeRestart / onAfterRestart, and the whole
    // point of this task is what those two hooks now do FIRST (F15).
    [[nodiscard]] gui::DevicePanel& getDevicePanelForTest() { return devicePanel_; }

    [[nodiscard]] SoundcheckController& getSoundcheckControllerForTest() { return soundcheck_; }

    // The message the status strip is currently holding -- panelMessage_,
    // written by showMessage. A refusal has to be VISIBLE, not merely
    // not-a-crash, so a test can read the sentence the operator would.
    [[nodiscard]] juce::String lastMessageForTest() const { return panelMessage_; }

    // true == Measuring (the full lock), false == None. Kept as it was so the
    // tests written against the two-state lock still say what they said.
    void setSoundcheckControlsLockedForTest (bool locked)
        { setSoundcheckLock (locked ? SoundcheckLock::Measuring : SoundcheckLock::None); }
    void setSoundcheckLockForTest (SoundcheckLock lock) { setSoundcheckLock (lock); }
    [[nodiscard]] SoundcheckLock soundcheckLockForTest() const { return soundcheckLock_; }

    // Drives the confirmation step on its own, which is the only way a headless
    // test can hold a dialog open: preflight refuses before the dialog whenever
    // no device is running, and no test may open one (this engine passes input
    // to output -- a device opened in a test suite is a feedback path).
    void askSoundcheckConfirmationForTest (const std::vector<SoundcheckController::Target>& targets)
        { askForSoundcheckConfirmation (targets); }
    [[nodiscard]] bool soundcheckConfirmPendingForTest() const { return soundcheckConfirmPending_; }

    // The targets a press of DO would measure, and the RunParams it would
    // freeze. Both are exposed because neither is observable anywhere else:
    // arm() copies RunParams into a private member and no getter reports it, so
    // "the running preset's ceiling really is fed" is otherwise unprovable --
    // and a ceiling left at its 0.0 default is FINITE, so arm() would not
    // refuse it either (SoundcheckController.h RunParams::ceilingDb).
    [[nodiscard]] std::vector<SoundcheckController::Target> soundcheckTargetsForTest() const
        { return buildSoundcheckTargets(); }
    [[nodiscard]] SoundcheckController::RunParams soundcheckRunParamsForTest() const
        { return buildSoundcheckRunParams(); }

    // THE COUNTDOWN SOURCE. It FORWARDS to the one private function
    // syncSoundcheckUi() feeds the panel from -- deliberately not a second copy
    // of the expression, so a test that asserts on this is asserting on what
    // the operator actually sees (F12).
    [[nodiscard]] double soundcheckCountdownMsForTest() const { return soundcheckCountdownMs(); }

    // The exact sentence the operator is asked to agree to. Exposed because
    // the honest level statement (spec 4.3 / F13) is a REQUIREMENT, and a
    // requirement nothing asserts is a requirement that quietly goes missing.
    [[nodiscard]] static juce::String soundcheckConfirmTextForTest (int passCount,
                                                                    int distinctOutputs)
        { return soundcheckConfirmText (passCount, distinctOutputs); }

    // One slot's apply ledger. It lives for this component's whole lifetime and
    // CLEAR ALL does not reset it -- see SoundcheckController.h, which calls
    // that out as a property of the design rather than a leak.
    [[nodiscard]] const SoundcheckApplyLedger& soundcheckLedgerForTest (int slot) const
        { return soundcheckLedgers_[(std::size_t) slot]; }

    // TEST ACCESSOR ONLY -- the serialiser is otherwise reachable only through
    // a live detector thread and a real log file. Public here, next to the
    // controller accessor, because notchEventToVar itself is private (below)
    // and stays that way.
    static juce::var notchEventToVarForTest (const NotchController::NotchEvent& e)
        { return notchEventToVar (e); }

    // LANE S -- LINK/INDEP flows from here. SlotPanel's per-slot control
    // (Task 7) calls setSlotLinked; the runtime source of truth is
    // slotLinked_, not the controller's own atomic, so a query never has to
    // reach into notchControllers_ just to read a flag the UI already knows.
    void setSlotLinked (int slotIndex, bool linked);
    [[nodiscard]] bool isSlotLinked (int slotIndex) const;
    [[nodiscard]] const NotchController& getControllerForTest (int slotIndex) const
        { return *notchControllers_[(std::size_t) slotIndex]; }

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

    //==========================================================================
    // LANE M Task 10 -- the DO flow. Every one of these runs on the MESSAGE
    // THREAD and nowhere else; the lane M thread reaches this object only
    // through the three injected std::functions wired in the constructor.

    // The DO press: preflight, then the confirmation, then arm.
    void beginSoundcheck();
    // The confirmation step alone. Separate so there is ONE place that puts DO
    // out of reach for the life of its own dialog and ONE place that gives it
    // back (review I-2).
    void askForSoundcheckConfirmation (const std::vector<SoundcheckController::Target>& targets);
    // The OK half of the confirmation. Separate because it crosses an async
    // boundary and re-reads everything the dialog could have invalidated.
    void armSoundcheck (const std::vector<SoundcheckController::Target>& targets);

    // Every enabled slot's every lane, in slot order. NOT deduplicated by
    // output channel: two slots feeding one channel are two different loops
    // (different mics), and the spec's worst case -- 8 stereo slots, 16
    // outputs, ~72 s -- is exactly this count.
    [[nodiscard]] std::vector<SoundcheckController::Target> buildSoundcheckTargets() const;
    [[nodiscard]] SoundcheckController::RunParams buildSoundcheckRunParams() const;

    // The sentence the operator reads before anything is emitted. TWO counts,
    // because they differ and the difference is the operator's time: a PASS is
    // one (slot, lane) measurement, and two slots feeding one output produce
    // two passes on ONE channel. The duration follows the passes; the channel
    // count is what the operator recognises on their patch (review M-1).
    [[nodiscard]] static juce::String soundcheckConfirmText (int passCount, int distinctOutputs);
    // Distinct outputChannel values in `targets`.
    [[nodiscard]] static int distinctOutputCount (const std::vector<SoundcheckController::Target>& targets);
    // Each refusal its OWN sentence: "no device" and "the room is ringing" are
    // different problems with different answers (inv 19, F26).
    [[nodiscard]] static juce::String soundcheckRefusalMessage (SoundcheckController::Refusal r);

    // F10 / I-3. The ONE place the console's three lock states are applied.
    void setSoundcheckLock (SoundcheckLock lock);
    // DO's enabled state under the CURRENT lock, so the confirmation path can
    // hand it back without having to know which state that is.
    void restoreMeasureEnabled();

    // Message-thread mirror of the machine's state, driven by the status timer.
    void syncSoundcheckUi();

    // THE COUNTDOWN, from lane M's OWN ClockSource. NEVER
    // NotchController::getSoundcheckRemainingMs(), which reports the PASSIVE
    // 15 s window measured in liveMs_ -- frozen while lane M has the taps
    // suspended, so it would show a number that stands still or reads 0 for the
    // whole run (F12, SoundcheckPanel.h:27-33). ONE definition, fed to the panel
    // and read by the test, so the two cannot drift apart.
    [[nodiscard]] double soundcheckCountdownMs() const;
    // The Results-strip Model built from the run's own results.
    [[nodiscard]] gui::SoundcheckPanel::Model soundcheckResultsModel() const;
    // Points the analyser's overlay at the DISPLAYED slot and lane's result.
    void refreshSoundcheckOverlay();
    // AP DUNG. The ONE place a preventive notch is written.
    void applySoundcheckProposals();
    // Unlock, hand detection back to the MODE, and settle the strip. Shared by
    // BO, APPLY, the abort path and the device-restart hook so no two of them
    // can tear a run down differently.
    void endSoundcheckSession (gui::SoundcheckPanel::Mode panelMode);

    // Declared BEFORE every child component: the look and feel must outlive
    // anything that might query it during teardown. The constructor installs
    // it once with setLookAndFeel(); children inherit it through the normal
    // Component::getLookAndFeel() chain.
    az::theme::AzLookAndFeel azLookAndFeel_;

    AudioEngine engine_;

    // Lane D (data loop). Declared before systemClock_ and the controllers:
    // declaration order is destruction order, so the logger outlives every
    // detector thread that can still hand it an event while it joins.
    SessionLogger sessionLogger_;
    juce::String  appVersion_ { "0.0.0-unset" };   // main.cpp sets the real one (D-8)
    int           lastLoadSkipped_ = 0;

    juce::var sessionHeader() const;
    static juce::var notchEventToVar (const NotchController::NotchEvent& e);

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

    // lane S: runtime source of truth; lane P reads it when SAVE exists
    std::array<bool, kMaxSlots> slotLinked_ {};

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
    // Lane M Task 9. An OVERLAY over the analyser, so it is declared AFTER the
    // view it covers: JUCE paints children in the order they were added, and
    // a strip that has to be opaque over a live trace must be added second.
    gui::SoundcheckPanel soundcheckPanel_;
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

    //==========================================================================
    // LANE M Task 10 state. All of it is declared BEFORE soundcheck_ below,
    // which is the last member in this class -- see the comment there.

    // One ledger per slot, alive for this component's whole lifetime: it
    // records what the PREVIOUS apply placed, so the next one replaces only its
    // own work and never a notch the operator locked in with the SOUNDCHECK
    // mode switch (SoundcheckController.h, C-2/Q7). CLEAR ALL does NOT reset
    // it, deliberately -- the entries simply stop matching anything.
    std::array<SoundcheckApplyLedger, kMaxSlots> soundcheckLedgers_;

    // Written by onStateChanged, which is invoked from the LANE M THREAD (and
    // from whatever thread calls stop()/abortAndJoin()/the destructor). A
    // relaxed store is ALL it may do: every GUI touch happens later, on the
    // message thread, in syncSoundcheckUi().
    std::atomic<bool> soundcheckDirty_ { false };

    // What syncSoundcheckUi last saw, so an edge is an edge. Message thread.
    SoundcheckController::State lastSoundcheckState_ = SoundcheckController::State::Idle;
    SoundcheckLock soundcheckLock_ = SoundcheckLock::None;

    // True while a confirmation is still unanswered. A second DO must not stack
    // a second dialog -- two OKs are two arms, and the later one would arm
    // against targets the first already consumed. ModeRail::handleClearAllClicked
    // carries the same guard for the same reason (review I-2).
    bool soundcheckConfirmPending_ = false;

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

    //==========================================================================
    // THE LAST MEMBER IN THIS CLASS, and that is the whole safety argument.
    //
    // SoundcheckController::stop(), abortAndJoin() AND ITS DESTRUCTOR invoke all
    // three injected lambdas on the calling thread as the last thing they do --
    // they stand a live run down, and standing one down means restoring
    // detection and logging the abort (SoundcheckController.h, I-10/N-2/C-3).
    // Those lambdas reach engine_, systemClock_, sessionLogger_,
    // notchControllers_ and soundcheckDirty_. Members die in REVERSE
    // declaration order, so declaring this last is what keeps every one of them
    // alive while it runs.
    //
    // The task brief said "after engine_ and before notchControllers_". That is
    // wrong against this header's own rule -- notchControllers_ would already be
    // dead when setDetectionActiveOnAllSlots fired from ~SoundcheckController.
    // The header's declaration-order rule wins; the brief's line does not.
    SoundcheckController soundcheck_ { engine_, systemClock_ };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
