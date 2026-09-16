### Task 10: `MainComponent` wiring, the control lock, and the device-restart abort

**Mức level dự kiến:** **0 dB** by itself — but this is the task that connects a button to a loudspeaker, so after it the app really can emit. Everything in spec §3's table becomes reachable here.

**The device-restart hole this task closes** (F15). `devicePanel_.onBeforeRestart` (`src/app/MainComponent.cpp:351-358`) today only stops the `NotchController`s; it has never heard of a `SoundcheckController`. And `audioDeviceAboutToStart` clears every ring (`src/app/AudioEngine.cpp:713-738`) under the precondition "no producer and no consumer is running" (`src/dsp/LockFreeRingBuffer.h:131-141`). So:

- `onBeforeRestart` must **abort and join** the `SoundcheckController` **before** it stops the `NotchController`s;
- `micCapture_.clear()` joins the same drain block (done in Task 5);
- `Preflight` records `getCurrentSampleRateHz()`, `getNumInputChannels()`, `getNumOutputChannels()`, and every later phase re-checks them each poll and **aborts** on a change (done in Task 6). A sample-rate change makes `T`, the reference `X` and every bin→Hz mapping wrong at once, and `getLastDeviceError()` reports **nothing** in that case.

**Files:**
- Modify: `src/app/MainComponent.h` — a `SoundcheckController soundcheck_;` member (declared **after** `engine_` and **before** `notchControllers_`, so destruction runs in the reverse order and the controller is torn down while the engine still exists), plus `void applyModeGating (int)` unchanged and a new `void setSoundcheckControlsLocked (bool)`
- Modify: `src/app/MainComponent.cpp`:
  - the `ĐO` lambda, next to the existing rail lambdas (anchor: `modeRail_.onSoundcheck = [this] { requestMode (AudioEngine::Mode::Soundcheck); };`, `:235`)
  - the `onBeforeRestart` hook (anchor: `devicePanel_.onBeforeRestart = [this]`, `:351`)
  - the preset chooser lambdas (anchor: `presetLoadChooser = [] (std::function<void (const juce::File&)> onPicked)`, `:258`)
  - `loadPreset` (`:830`) — refuse while `state != Idle`
- Test: `tests/test_gui_wiring.cpp`

**Interfaces:**
- Consumes: everything from Tasks 6, 7 and 9. Of the `MainComponent` members the tests below use, these **already exist** and are used as they are: `getNotchControllerForTest` (`src/app/MainComponent.h:188`), `getAudioEngine` (`:67`), `loadPreset` (`:89`), `showMessage` (`:118`), `notchEventToVarForTest` (`:194`).
- Produces — **five NEW test accessors** (I-5: rev 1 used all five without checking, and none of them existed). Each is modelled on the existing pair `getSlotPanelForTest` (`src/app/MainComponent.h:176`) and `getSpectrumViewForTest` (`:182`), and each carries a `// TEST ACCESSOR ONLY` comment:
  ```cpp
  [[nodiscard]] gui::DevicePanel&         getDevicePanelForTest()        { return devicePanel_; }
  [[nodiscard]] gui::ModeRail&            getModeRailForTest()           { return modeRail_; }
  [[nodiscard]] SoundcheckController&     getSoundcheckControllerForTest() { return soundcheck_; }
  // The message the status strip is currently holding -- panelMessage_,
  // written by showMessage (src/app/MainComponent.cpp:688-692). A refusal has to
  // be VISIBLE, not just not-a-crash.
  [[nodiscard]] juce::String              lastMessageForTest() const     { return panelMessage_; }
  void setSoundcheckControlsLockedForTest (bool locked) { setSoundcheckControlsLocked (locked); }
  ```

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_gui_wiring.cpp`:

```cpp
// RED IF: onBeforeRestart forgets lane M. audioDeviceAboutToStart clears every
// ring (AudioEngine.cpp:713-738) under "no producer, no consumer running"
// (LockFreeRingBuffer.h:131-141); a live soundcheck thread draining micCapture_
// across that call violates the precondition. F15.
TEST (MainComponentSoundcheck, DeviceRestartAbortsAndJoinsTheSoundcheckFirst)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent app;

    auto& sc = app.getSoundcheckControllerForTest();
    ASSERT_NE (app.getDevicePanelForTest().onBeforeRestart, nullptr);

    app.getDevicePanelForTest().onBeforeRestart();

    EXPECT_EQ (sc.getState(), SoundcheckController::State::Idle);
}

// RED IF: PRESET LOAD stays live while proposals are pending. adoptPreset uses
// the FILE's indices and overwrites without checking n.active
// (NotchController.cpp:487-506 -> setNotchImpl :226-245), so it would silently
// erase every pending proposal. F10.
TEST (MainComponentSoundcheck, PresetLoadIsRefusedWhileASoundcheckIsPending)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent app;

    app.setSoundcheckControlsLockedForTest (true);

    juce::File tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("lane-m-lock-test.json");
    tmp.replaceWithText ("{\"version\":\"1.0\",\"notches\":[]}");

    EXPECT_FALSE (app.loadPreset (tmp)) << "a pending soundcheck must refuse a preset load";

    tmp.deleteFile();
}

// RED IF: the mode buttons and CLEAR ALL stay live during a run. A mode change
// mid-run re-arms detection under a suspended tap; CLEAR ALL mid-run removes
// notches nobody asked about.
TEST (MainComponentSoundcheck, ModeAndClearAllAreLockedWhileRunning)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent app;

    app.setSoundcheckControlsLockedForTest (true);
    auto& rail = app.getModeRailForTest();

    EXPECT_FALSE (rail.soundcheckButton.isEnabled());
    EXPECT_FALSE (rail.autoButton.isEnabled());
    EXPECT_FALSE (rail.bypassButton.isEnabled());
    EXPECT_FALSE (rail.clearAllButton.isEnabled());
    EXPECT_FALSE (rail.measureButton.isEnabled());

    app.setSoundcheckControlsLockedForTest (false);
    EXPECT_TRUE (rail.measureButton.isEnabled());
}

// RED IF: the restore-detection lambda grows past a relaxed atomic store per
// slot. It runs on the LANE M THREAD, and that is only defensible because
// NotchController::setDetectionActive is exactly one relaxed store
// (NotchController.cpp:936-939). inv 17.
TEST (MainComponentSoundcheck, RestoreDetectionCallbackOnlyTouchesAtomics)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent app;

    auto& sc = app.getSoundcheckControllerForTest();
    ASSERT_NE (sc.setDetectionActiveOnAllSlots, nullptr);

    sc.setDetectionActiveOnAllSlots (false);
    for (int i = 0; i < kMaxSlots; ++i)
        EXPECT_FALSE (app.getNotchControllerForTest (i)->detectionActiveForTest()) << "slot " << i;

    sc.setDetectionActiveOnAllSlots (true);
    for (int i = 0; i < kMaxSlots; ++i)
        EXPECT_TRUE (app.getNotchControllerForTest (i)->detectionActiveForTest()) << "slot " << i;
}

// RED IF: pressing ĐO with no device open emits anything, or shows nothing at
// all. A refusal must SAY which refusal it was. inv 19, F26.
TEST (MainComponentSoundcheck, MeasureWithNoDeviceRefusesAndSaysSo)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent app;

    ASSERT_NE (app.getModeRailForTest().onMeasure, nullptr);
    app.getModeRailForTest().onMeasure();

    EXPECT_EQ (app.getSoundcheckControllerForTest().getState(),
               SoundcheckController::State::Idle);
    EXPECT_FALSE (app.getAudioEngine().soundcheckIsEmitting());
    EXPECT_TRUE (app.lastMessageForTest().isNotEmpty());
}
```

**I-5 settled this by opening the file.** `getNotchControllerForTest` (`:188`), `getAudioEngine` (`:67`), `loadPreset` (`:89`), `showMessage` (`:118`) and `notchEventToVarForTest` (`:194`) exist. The other five — `getDevicePanelForTest`, `getModeRailForTest`, `getSoundcheckControllerForTest`, `lastMessageForTest`, `setSoundcheckControlsLockedForTest` — **do not**, and are declared by this task (see Interfaces). Rev 1 used all five without checking; that is the lane G M-1 defect, and it is caught by opening the file, not by reasoning.

- [ ] **Step 2: Wire it**

**First, the callback contract** (I-10). `setDetectionActiveOnAllSlots`, `logEvent` and `onStateChanged` are bare public `std::function`s invoked from the lane M thread; assigning one while it is being invoked is a data race, and there is no lock. They are therefore assigned **in `MainComponent`'s constructor, before the controller is ever started**, and never again:

```cpp
    // I-10: assigned ONCE, here, before any soundcheck_.start(). Never reassigned
    // while the thread can run -- abortAndJoin() must have returned first.
    soundcheck_.setDetectionActiveOnAllSlots = [this] (bool on)
    {
        // One relaxed atomic store per slot and nothing else
        // (NotchController::setDetectionActive, NotchController.cpp:936-939).
        // Safe from the lane M thread; this is the ONLY NotchController call
        // anywhere on that thread's stack, and it lives HERE, in MainComponent,
        // not in SoundcheckController, which holds no pointer at all (inv 17).
        for (auto& c : notchControllers_)
            c->setDetectionActive (on);
    };
    soundcheck_.logEvent = [this] (const juce::var& v) { sessionLogger_.log (v); };
    soundcheck_.onStateChanged = [this] { refreshStatus(); };
```

Then the `ĐO` lambda, beside the other rail lambdas:

```cpp
    modeRail_.onMeasure = [this]
    {
        // Message thread. Everything a run needs is read HERE, once, and handed
        // over frozen: a threshold that moved mid-run would score channel 1 and
        // channel 9 of one measurement on two different rulers (spec §4.3).
        NotchController::SnapshotBuffer risk {};
        notchControllers_[(std::size_t) displayedSlot_]->copySnapshot (risk);

        const auto targets = buildSoundcheckTargets();
        const auto refusal = soundcheck_.preflight (targets, risk);
        if (refusal != SoundcheckController::Refusal::None)
        {
            showMessage (soundcheckRefusalMessage (refusal));   // each refusal its OWN sentence
            return;
        }

        SoundcheckController::RunParams params;
        // A PEAKINESS RATIO, read live from the detector so lane M's gate cannot
        // drift away from the detector's own (N1). Never kConfirmScore.
        params.noiseFloorGate    = notchControllers_[0]->getPeakinessThreshold();
        params.peak              = SoundcheckSignal::kSoundcheckMaxPeak;
        params.sampleRate        = engine_.getCurrentSampleRateHz();
        params.numInputChannels  = engine_.getNumInputChannels();
        params.numOutputChannels = engine_.getNumOutputChannels();
        params.ceilingDb         = notchControllers_[0]->getNotchDepthDb();
        params.notchQ            = notchControllers_[0]->getNotchQ();

        confirmSoundcheck (targets.size(), [this, targets, params]
        {
            setSoundcheckControlsLocked (true);
            soundcheck_.arm (targets, params);
        });
    };
```

`confirmSoundcheck` shows the dialog spec §4.3 requires: **"HẠ MASTER TRƯỚC"** and the total duration — `targets.size() * 4.5 s`, printed as a real number, up to ~72 s. It is **injectable**, like the preset choosers (`src/app/MainComponent.cpp:258-290`), so a headless test can answer it without a native dialog (`memory/gui-console-lessons-2026-08-24.md`: an async confirm dialog must be injectable and hold a `SafePointer`).

`onBeforeRestart` gains **two lines, first** (and `abortAndJoin()` returning is also what makes it safe to touch the callbacks again, per I-10's contract):

```cpp
    devicePanel_.onBeforeRestart = [this]
    {
        // Lane M FIRST (F15): audioDeviceAboutToStart clears every ring
        // (AudioEngine.cpp:713-738) under "no producer, no consumer running"
        // (LockFreeRingBuffer.h:131-141), and micCapture_ is in that block now.
        soundcheck_.abortAndJoin();
        setSoundcheckControlsLocked (false);

        // §6.5, unchanged: every detector thread joins before the rings clear.
        for (auto& controller : notchControllers_)
            controller->stop (1000);
    };
```

`setSoundcheckControlsLocked (bool)` disables `modeRail_.soundcheckButton`, `autoButton`, `bypassButton`, `clearAllButton`, `measureButton`, both preset buttons, and every slot enable/width/routing control. `loadPreset` (`src/app/MainComponent.cpp:830`) returns `false` early while locked **or while the state is `Results`**, with a message saying why.

The `ÁP DỤNG` / `BỎ` lambdas on the panel:

```cpp
    soundcheckPanel_.onApply = [this]
    {
        // MESSAGE THREAD. This is the ONLY place a preventive notch is written.
        SoundcheckApplyStats total;
        for (int slot = 0; slot < kMaxSlots; ++slot)
        {
            const auto forSlot = soundcheck_.copyResultsForSlot (slot);
            if (forSlot.empty()) continue;
            const auto s = applySoundcheckResults (*notchControllers_[(std::size_t) slot], forSlot);
            total.placed += s.placed; total.refused += s.refused;
            total.clearedPrevious += s.clearedPrevious;
        }
        logSoundcheckApply (total);
        soundcheck_.applyRequested();
        setSoundcheckControlsLocked (false);
    };
```

- [ ] **Step 3: Build, run, gate**

```bash
cmake -B build -G "Visual Studio 18 2026" -A x64
```
```bash
cmake --build build --config Release
```
```bash
cd build && ctest -C Release
```
Expected: `100% tests passed (638)` — 633 + 5. ESTIMATE.

- [ ] **Step 4: Render and read back**

```bash
build/tools/Release/HandsFreeSnapshot.exe shots 1440 920 --fast
```
Read `shots/console-live.png` and `shots/console-idle.png` back and confirm the rail still lays out with five controls instead of four, and that nothing was pushed off the bottom. Send both to the owner.

- [ ] **Step 5: Commit**

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add src/app/MainComponent.h src/app/MainComponent.cpp tests/test_gui_wiring.cpp
```
```bash
git commit -m "feat(lane-m): wire DO/APPLY/BO, lock the controls while measuring, abort+join before a device restart"
```

---

