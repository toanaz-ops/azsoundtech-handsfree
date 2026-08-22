# Lane report — GUI device & status (Tasks 16, 17, 18, 23 partly)

**Branch:** `claude_desk/gui-device-status-ec94cd` (harness worktree, not the
`D:/hf-lanes/` path the runbook assumes). **Base:** `main` @ `01db1f0`.
**Date:** 2026-08-22.

## Measured, not asserted

| | Command | Result |
|---|---|---|
| Baseline, before any edit | `ctest --test-dir build -C Release` | **75/75 passed**, exit 0 |
| After this lane | `ctest --test-dir build -C Release` | **107/107 passed**, exit 0 |
| Both targets | `cmake --build build --config Release` | exit 0 |
| Clean configure | `cmake -S . -B build -G "Visual Studio 18 2026" -A x64` | 183.8 s, exit 0 |

**The app was run, not just built.** It opens a device and the status line shows
live values:

```
Audio Device: [Windows Audio v]   Device: [Headphones (Realtek USB Audio) v]
Sample Rate:  [48000 v]    Buffer: [480 samples v]
Status:  * 48 kHz / 2 in / 2 out / Latency 10.0 ms      (indicator green)
Mode:  [Run Soundcheck (15s)]  [Auto]  [Bypass]         (Bypass highlighted)
```

`2 in / 2 out` is the proof that audio actually flowed: those counts are
published from the audio callback and read 0 until a block has been delivered.
The type shown is **Windows Audio**, not ASIO — the fallback working, on a
machine where CMake reports `ASIO SDK not found`.

## What was built

```
src/gui/DeviceViewModel.h/.cpp   NEW  pure decisions + formats, juce_core only
src/gui/DevicePanel.h/.cpp       NEW  Tasks 16 + 17, four combos
src/gui/StatusBar.h/.cpp         NEW  Task 18
src/gui/ModeBar.h/.cpp           NEW  Task 23 (partial, see below)
src/app/MainComponent.h/.cpp     MOD  was a 25-line stub; now owns AudioEngine
src/main.cpp                     MOD  calls startAudio() after the window shows
CMakeLists.txt                   MOD  new sources
tests/CMakeLists.txt             MOD  new tests, GUI sources, juce_gui_basics
tests/test_deviceviewmodel.cpp   NEW  21 tests
tests/test_gui_wiring.cpp        NEW  11 tests
```

**Nothing under `src/dsp/`, `src/app/AudioEngine.*` or `installer/` was
touched.** No `AudioEngine` method was added — the eleven from `194a090` were
sufficient, with one gap noted below.

### The first job was not Task 16

The brief was right: **nothing in the program constructed an `AudioEngine`.**
The shipped binary was an 800x600 window that drew a string and processed no
audio. `MainComponent` now owns one **by value** — not a singleton, not a global
— matching bridge design section 6, which has it owning the future
`NotchController` too.

Opening the device is `MainComponent::startAudio()`, called from `main.cpp`
**after** `setVisible(true)`. It is deliberately not a constructor side effect:
that is what lets a test construct the whole application object on a machine
with no audio hardware, and it means a device that refuses to open leaves a
visible window showing why rather than nothing at all.

## New tests, and the production change that breaks each

32 new, all in `HandsFreeTests`.

**`test_deviceviewmodel.cpp` (21)** — pure presentation logic, no window, no
device, links only `juce_core`.

| Test | Breaks if |
|---|---|
| `DefaultDeviceTypePrefersAsioWhenItIsRegistered` | the ASIO preference is dropped |
| `DefaultDeviceTypeFallsBackToTheFirstTypeWhenAsioIsAbsent` | anyone reinstates the plan's "ASIO only" filter |
| `DefaultDeviceTypeIsEmptyWhenNoTypeIsRegisteredAtAll` | the empty case indexes `[0]` |
| `SampleRateIsShownInKilohertz` | the status line switches to `getCurrentSampleRate()`'s `"48000 Hz"` |
| `AFractionalKilohertzRateKeepsItsDecimals` | 44100 is rounded to `"44 kHz"` |
| `AThreeDecimalRateIsNotRoundedAway` | the formatter is capped at one decimal |
| `SampleRateShowsAPlaceholderWhenNoDeviceIsOpen` | `0.0` is formatted as `"0 kHz"` |
| `AChannelCountOfZeroRendersAsAPlaceholderNotAsTwo` | anyone "fixes" the 0 counts by assuming stereo |
| `LatencyIsConvertedFromSecondsToMilliseconds` | the `* 1000` is dropped (the accessor returns SECONDS) |
| `TheStatusLineMatchesTheExampleInSpecSection61` | the format drifts from spec 6.1 |
| `TheStatusLineReportsZeroChannelsHonestly` | as above, at the line level |
| `AStoppedEngineSaysStoppedRatherThanShowingStaleNumbers` | a stopped engine renders its last device's numbers |
| `ASampleRateComboItemIsThePlainNumberAsInSpecSection61` | combo items switch to the kHz form |
| `ABufferSizeComboItemIsLabelledInSamples` | the unit is dropped |
| `TheRunningSampleRateIsMatchedDespiteFloatingPointDrift` | the tolerant match becomes `==` |
| `AnUnsupportedSampleRateMatchesNothing` | the lookup returns 0 instead of -1 |
| `AHealthyEngineShowsNoBanner` | an empty error still paints a banner |
| `ADeviceErrorReachesTheScreenVerbatim` | the driver's text is replaced by a generic string |
| `ARefusedSampleRateSaysWhatIsActuallyRunning` | the refusal stops naming the value still in force |
| `ARefusedBufferSizeSaysWhatIsActuallyRunning` | as above |
| (plus `ANonZeroChannelCountRendersAsTheNumber`) | |

**`test_gui_wiring.cpp` (11)** — real `juce::Component`s, no window.

| Test | Breaks if |
|---|---|
| `MainComponent.TheApplicationActuallyOwnsAnAudioEngine` | **the engine is removed from the app again — this is the test that would have caught the shipped defect** |
| `MainComponent.ConstructionOpensNoAudioDevice` | `start()` is moved into the constructor |
| `MainComponent.RequestingAModeReachesTheEngine` | the mode request stops reaching `setMode()` |
| `ModeBar.ClickingAutoRequestsAutoMode` | the Auto button's handler is unwired |
| `ModeBar.ClickingBypassRequestsBypassMode` | ditto, Bypass |
| `ModeBar.ClickingSoundcheckRequestsSoundcheckMode` | ditto, Soundcheck |
| `ModeBar.ReflectingTheEnginesModeDoesNotRequestAModeChange` | `setDisplayedMode()` uses a sending notification |
| `ModeBar.TheActiveModeIsVisuallyDistinguishableFromTheInactiveOnes` | the explicit on-colour is removed |
| `DevicePanel.WithNoDeviceOpenTheRateAndBufferCombosAreEmptyRatherThanGuessed` | someone fills them from a hardcoded list of "standard" rates |
| `DevicePanel.TheTypeShownIsOneTheManagerHasRatherThanTheOneRequested` | the panel echoes `desiredDeviceType_` instead of the actual one |
| `DevicePanel.RefreshOffersTheDriverTypesTheMachineActuallyHas` | `refresh()` stops populating the type combo |

**Two of these were verified by mutation, not by assertion:**

- `ReflectingTheEnginesModeDoesNotRequestAModeChange` — changed
  `dontSendNotification` to `sendNotificationSync` in production, rebuilt, and
  watched it fail (`1 FAILED TEST`), then reverted.
- `TheActiveModeIsVisuallyDistinguishableFromTheInactiveOnes` — written before
  the fix and observed failing.

Every other new test was watched failing as a compile error (missing header,
missing symbol, missing source file) before its implementation existed.

## Wrong in the plan or the brief

1. **The brief's status-line source is wrong.** It says *"Rate:
   `getCurrentSampleRate()` (display form)"*. That returns `"48000 Hz"`, but
   **spec 6.1 shows `48 kHz`**. Spec > brief, so the status line formats kHz
   from `getCurrentSampleRateHz()`. `getCurrentSampleRate()` is now unused by
   the GUI.

2. **`setSampleRate()` / `setBufferSize()` give no reason for a refusal.** The
   brief says to "surface why". The engine returns only `bool`;
   `getLastDeviceError()` is written by `audioDeviceError()` and by `start()`,
   **not** by a refused `setAudioDeviceSetup()`. So the most the GUI can
   honestly say is the fact plus the value still in force —
   *"Device refused 96000 Hz - still running at 48000 Hz"*. Reported rather than
   acted on, because adding an out-parameter to `AudioEngine` would make its
   header a four-lane merge conflict, which the brief forbids.

3. **Plan Task 16's filename.** `DeviceSelector` became `DevicePanel`, because
   it owns the buffer size too. Tasks 16 and 17 share one component on purpose:
   the rate and buffer lists are empty until a device is open, so they must be
   repopulated by the very act of opening one. Splitting them would mean
   inventing a callback between two components purely to say "refill yourself".

4. **The mode buttons were unreadable, and only running the app showed it.**
   Under the default LookAndFeel on a dark scheme, `buttonOnColourId` is close
   enough to the off state that all three buttons looked identical — the live
   mode was invisible. Bypass versus Auto is the difference between notches
   being applied to a live PA and not. Fixed with an explicit on-colour, and
   pinned by a test. This is the concrete reason the brief demands a screenshot
   instead of "it compiles".

5. **The harness worktree needs the runbook's setup too.** This lane ran in
   `.claude/worktrees/gui-device-status-ec94cd`, created by the desktop harness
   rather than by the runbook's recipe. Its `external/JUCE` was **empty** (0
   entries) and nothing configured until the junction and the
   `git config --worktree submodule."external/JUCE".{active,ignore}` pair from
   `docs/superpowers/runbooks/parallel-lanes.md` were applied by hand. Both
   sides were verified afterwards: lane `git status` clean, main repo
   `git submodule status` still shows a leading space.

   > ⚠ **The junction is still in place.** Per the runbook's third trap,
   > **delete `external/JUCE` inside this worktree BEFORE the worktree is
   > removed**, or the removal follows the junction and deletes the shared JUCE
   > checkout. That has already happened once on this project.

6. **Confirmed, not overturned:** ASIO really is absent here — the configure
   prints `ASIO SDK not found`, and the running app selected Windows Audio. An
   ASIO-only combo would have been empty, exactly as the brief said.

## What was not done, and why

- **The Soundcheck countdown.** The button sets `Mode::Soundcheck` and nothing
  else. Owner decision **D-06** freezes the detector's timers while the tap is
  dead, so a GUI-side wall-clock `juce::Timer` would disagree with a frozen
  detector and show a countdown that did not match what the app was doing. The
  remaining time has to come from `getSoundcheckSecondsRemaining()` (bridge
  design section 4), which does not exist yet. The reason is written into
  `ModeBar.h` and `MainComponent::requestMode()` so nobody adds it by reflex.

- **Spec section 8's "no ASIO device found -> show driver install guidance".**
  In scope for the error-handling task, not for 16/17/18/23, and the brief does
  not ask for it. It is a genuine gap on a machine with no ASIO driver, which is
  every machine this project currently builds on.

- **Tasks 19, 20, 22, 24** (spectrum, notch overlay, notch list, clear
  controls). Blocked on the bridge design, as the brief says. The empty area
  below the mode row in `MainComponent::resized()` is where they go.

- **Interactive verification of the combo boxes.** The population and the
  displayed values were confirmed on screen, and every decision behind them is
  unit-tested. What was **not** driven end-to-end is a human clicking a combo
  and watching the device restart — the app is not a Start-menu-installed
  application, so the desktop-control tooling would not target it, and driving
  synthetic clicks around that would have been circumventing a permission
  prompt. Treat "changing sample rate at runtime restarts the device correctly"
  as reasoned-and-unit-tested, not as observed.

- **`DevicePanel`'s component wiring was not written test-first.** Its decision
  logic was (all of `DeviceViewModel`); the `juce::Component` around it got
  characterization tests afterwards. Recorded here rather than glossed over.
