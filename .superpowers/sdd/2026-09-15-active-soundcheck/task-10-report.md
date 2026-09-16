# Task 10 report — `MainComponent` wiring, the control lock, the device-restart abort

**Model actually used:** Claude Opus 5 (1M context) — `claude-opus-5[1m]`.
**Worktree:** `.claude/worktrees/lane-m-soundcheck-0915`, branch `feat/lane-m-active-soundcheck`, from `c6dd05c`.

---

## What was built

`MainComponent` now owns a `SoundcheckController` and is the only thing that
drives it. The DO press goes preflight → confirmation → arm; the panel is fed
from the machine on the status timer; APPLY writes through
`applySoundcheckResults` with one ledger per slot; the console locks while a run
is in flight; and a device restart aborts and joins lane M **before** anything
touches a ring.

**New in `src/app/MainComponent.h`**

- `SoundcheckController soundcheck_` — **the last member in the class** (see
  divergence D1).
- `soundcheckConfirmHook` — injectable, beside the preset choosers.
- `soundcheckLedgers_` (one `SoundcheckApplyLedger` per slot, alive for the
  component's lifetime), `soundcheckDirty_` (atomic), `lastSoundcheckState_`,
  `soundcheckLocked_`.
- Private: `beginSoundcheck`, `armSoundcheck`, `buildSoundcheckTargets`,
  `buildSoundcheckRunParams`, `soundcheckConfirmText`,
  `soundcheckRefusalMessage`, `setSoundcheckControlsLocked`, `syncSoundcheckUi`,
  `soundcheckCountdownMs`, `soundcheckResultsModel`, `refreshSoundcheckOverlay`,
  `applySoundcheckProposals`, `endSoundcheckSession`.
- Test accessors (`// TEST ACCESSOR ONLY`, modelled on `getSlotPanelForTest`):
  `getDevicePanelForTest`, `getSoundcheckControllerForTest`,
  `lastMessageForTest`, `setSoundcheckControlsLockedForTest`,
  `soundcheckTargetsForTest`, `soundcheckRunParamsForTest`,
  `soundcheckCountdownMsForTest`, `soundcheckConfirmTextForTest`,
  `soundcheckLedgerForTest`.

**New in `src/app/MainComponent.cpp`**

- The three injected callbacks, assigned **once in the constructor before any
  `start()`** and never again (I-10/N-2): `setDetectionActiveOnAllSlots` (one
  relaxed store per slot and nothing else), `logEvent` → `sessionLogger_.log`,
  `onStateChanged` → **a relaxed atomic store only**.
- `modeRail_.onMeasure`, `soundcheckPanel_.onStop / onApply / onDismiss`, and a
  default async `NativeMessageBox` confirmation.
- `devicePanel_.onBeforeRestart`: `abortAndJoin()` **first**, then
  `endSoundcheckSession(Hidden)`, then the existing detector `stop(1000)` loop.
  `onAfterRestart`: the existing width/start loop **plus `soundcheck_.start()`**,
  because `abortAndJoin()` leaves the poll thread stopped and nothing inside the
  controller restarts it.
- `startAudio()` starts the poll thread; the destructor stands the run down
  explicitly (`soundcheck_.stop(2000)`) as its first act after `stopTimer()`.
- `loadPreset` refuses while locked, **with a visible sentence**.
- `timerCallback` → `syncSoundcheckUi()`.
- Every operator-facing sentence as **explicit UTF-8 bytes** (no `/utf-8` in this
  build), each refusal its own sentence.

**Session log.** Four events come from the controller through the injected
`logEvent`. The fifth, `soundcheck_apply`, is emitted by the APPLY lambda through
`makeSoundcheckApplyEvent(stats)` — **one per slot applied**, with `slot` added
to the returned object so a reader can tell which chain the counts describe.
No second event shape was inlined.

---

## Divergences from the brief (the real headers won)

| # | Brief said | Header/reality | What I did |
|---|---|---|---|
| D1 | declare `soundcheck_` **after `engine_` and before `notchControllers_`** | `SoundcheckController.h` (I-10/N-2/C-3): "THE OWNER MUST DECLARE SoundcheckController AFTER its AudioEngine, its ClockSource, its SessionLogger **and every GUI member these lambdas reach**" | Declared **last in the class**. The brief's position would have `notchControllers_` already destroyed when `~SoundcheckController` fired `setDetectionActiveOnAllSlots`. Stated in a comment at the declaration. |
| D2 | `soundcheck_.arm (targets, params)` | `arm (std::vector<Target>, const RunParams&, const NotchController::SnapshotBuffer&)` — arm takes **its own** risk snapshot (S-1) | A fresh `copySnapshot` is taken in `armSoundcheck`, after the dialog. |
| D3 | `applySoundcheckResults (controller, results)` | `applySoundcheckResults (controller, slot, results, ledger&)` | Called with the slot and that slot's ledger. |
| D4 | five new test accessors, `getModeRailForTest` among them | `getModeRailForTest` **already exists** (`MainComponent.h:151`, Task 9) | Added the other four, plus five more the mutation checks needed (see D8). |
| D5 | `soundcheck_.onStateChanged = [this] { refreshStatus(); };` | `onStateChanged` is invoked **from the lane M thread** and from whatever thread calls `stop()`/`abortAndJoin()`/the destructor | It stores **one relaxed atomic**. All component work happens in `syncSoundcheckUi()` on the message thread, off the 200 ms status timer. `refreshStatus()` touches `statusBar_` and calls `repaint()` — a component touch off the message thread. |
| D6 | lock **before** `arm()` | `arm()` can refuse (inv 19: not one sample emitted, state still Idle) | Lock **after** a successful arm. Locking first would shut the operator out of their own mode rail because of a refusal. |
| D7 | `params.peak = SoundcheckSignal::kSoundcheckMaxPeak` | aliased on the controller | Used `SoundcheckController::kSoundcheckMaxPeak` (one definition, lane G m-D). |
| D8 | — | `RunParams` is copied into a private member with no getter | Added `soundcheckRunParamsForTest` / `soundcheckTargetsForTest` / `soundcheckConfirmTextForTest`, because the brief's "drop the ceiling ⇒ arm refuses" mutation **does not hold**: a defaulted `ceilingDb` of `0.0` is finite and passes `arm()`'s `isfinite` check. The ceiling is asserted by value instead. |
| D9 | spec §4.3 "Results: unlock everything except PRESET LOAD"; task prompt "locked incl. Results/**Applied**" | — | Locked for the whole of `state != Idle` (Results included, which is the stricter of the two). **Unlocked at `Mode::Applied`** — see concern C1. |
| D10 | Esc = `requestStop` with `AbortReason::Esc` | `SoundcheckPanel::keyPressed` routes Esc through the **same `onStop`** as the DUNG button (Task 9) | Both log `AbortReason::UserStop`. `AbortReason::Esc` is unreachable from the GUI as Task 9 built it — see concern C2. |

---

## Commands and output

```
cmake -B build -G "Visual Studio 18 2026" -A x64
-- ASIO SDK found at .../external/asiosdk
-- Configuring done (5.3s) / Generating done (0.9s)

cmake --build build --config Release
  HandsFree.vcxproj -> .../AZ Soundtech Hands-free.exe
  HandsFreeSnapshot.vcxproj -> .../HandsFreeSnapshot.exe
  HandsFreeTests.vcxproj -> .../HandsFreeTests.exe
(one pre-existing warning C4996 juce::Displays::Display::userArea; no new warnings)

cd build && ctest -C Release -R "MainComponentSoundcheck" --output-on-failure
100% tests passed, 0 tests failed out of 13

cd build && ctest -C Release
100% tests passed, 0 tests failed out of 702
Total Test time (real) =  58.67 sec
```

**702 = 689 + 13.** The brief's estimate was "+5"; thirteen tests were written
because four of the brief's five were not sufficient on their own (see the
mutation table) and four more requirements — the confirmation's honest level
statement, the countdown source, the RunParams contents and the target set — had
no assertion at all otherwise.

The 13: `DeviceRestartAbortsAndJoinsTheSoundcheckFirst`,
`DeviceRestartUnlocksTheConsoleAndSaysWhatHappened`,
`PresetLoadIsRefusedWhileASoundcheckIsPending`,
`ModeAndClearAllAreLockedWhileRunning`,
`RestoreDetectionCallbackOnlyTouchesAtomics`,
`MeasureWithNoDeviceRefusesAndSaysSo`, `NoConfirmHookMeansNoRun`,
`TheCountdownComesFromLaneMsOwnClockNotThePassiveWindow`,
`RunParamsCarryTheRunningPresetsCeilingAndTheLiveGate`,
`TargetsAreEveryLaneOfEveryEnabledSlot`,
`TheConfirmationSaysHaMasterTruocAndTheRealDuration`,
`StopAndDismissAreWiredAndNeitherEmits`,
`EverySlotHasItsOwnApplyLedgerAndClearAllDoesNotResetIt`.

---

## Mutation checks (all four applied, built, run, reverted)

| Mutation | Test | Result |
|---|---|---|
| countdown fed `notchControllers_[0]->getSoundcheckRemainingMs()` (the frozen passive window) | `TheCountdownComesFromLaneMsOwnClockNotThePassiveWindow` | **FAILED** ✔ |
| `onBeforeRestart` restarts without `abortAndJoin()` (`const bool idle = true;`) | `DeviceRestartAbortsAndJoinsTheSoundcheckFirst` | **FAILED** ✔ |
| one lock removed (`slotPanel_.setEnabled(! locked)` dropped) | `ModeAndClearAllAreLockedWhileRunning` | **FAILED** ✔ |
| `params.ceilingDb` never assigned | `RunParamsCarryTheRunningPresetsCeilingAndTheLiveGate` | **FAILED** ✔ |

After reverting all four: full rebuild, `702/702 passed`.

Note on the countdown mutation: `soundcheckCountdownMs()` is **one private
function**, called by `syncSoundcheckUi()` and forwarded by the test accessor, so
the mutation changes the number the panel is actually fed — not a second copy of
the expression.

---

## Renders (read back, per CLAUDE.md)

`build/tools/Release/HandsFreeSnapshot.exe shots 1440 920 --fast` — five scenes
written.

- **`shots/console-live.png`** — masthead, five rail controls (SOUNDCHECK / AUTO
  / BYPASS / **ĐO** / CLEAR ALL) with the ĐO cell reading `ĐO` over
  `phát tín hiệu` in correct diacritics (no mojibake), analyser with three notch
  markers, RING RISK CRITICAL chip, notch table and the full rig column. Nothing
  is pushed off the bottom and no control is greyed — the snapshot tool never
  calls `startAudio()`, so the poll thread never starts and the console is
  unlocked.
- **`shots/console-idle.png`** — empty state intact: `WAITING FOR SIGNAL`,
  `NOTHING RINGING`, RING RISK `N/A`, rail and drawer legible, layout unchanged
  from Task 9.
- **`shots/console-soundcheck-results.png`** (checked as the scene closest to
  this change) — the margin curve, the marked-bin rake along the plot floor, the
  dimmed low-confidence band with `độ tin cậy thấp`, the four-line results strip
  and `ÁP DỤNG` / `BỎ`. The ĐO cell is correctly greyed in this staged state.

**No emission is possible from the snapshot tool or from any test**: neither
opens a device, and `arm()` refuses with `Refusal::EngineNotRunning` while the
engine is not running. `soundcheckIsEmitting()` is asserted false on the DO path,
the stop path and the restart path.

---

## Self-review

- **Declaration order** — `soundcheck_` is the last member; `soundcheckDirty_`,
  `soundcheckLedgers_`, `sessionLogger_`, `systemClock_`, `engine_` and
  `notchControllers_` are all declared above it, so all are alive while
  `~SoundcheckController` stands a run down. The destructor also stops it
  explicitly first.
- **Every lambda's thread** — `setDetectionActiveOnAllSlots`: one relaxed
  `setDetectionActive` per slot, nothing else, no allocation, no lock.
  `logEvent`: `SessionLogger::log` is the thread-safe sink the detectors already
  use. `onStateChanged`: a single relaxed store. None of the three touches a
  `juce::Component`.
- **Every abort and refusal reaches the operator** — preflight refusals, arm
  refusals, the latched device error, the missing confirm hook, the preset-load
  refusal and the restart abort each call `showMessage`, and four tests assert
  `lastMessageForTest().isNotEmpty()`.
- **Logger not started in the ctor** — unchanged; the ctor only assigns
  `logEvent`, and `SessionLogger::log` drops events while inactive.
- **Detection after a run** — the controller's own restore is
  `setDetectionActive(true)` on *every* slot, which would leave BYPASS detecting
  and arm a disabled slot. `endSoundcheckSession()` re-runs `applyModeGating` for
  every slot on the message thread, exactly once per session end, so the MODE
  decides — not lane M.

---

## Concerns

1. **The console unlocks at `Mode::Applied` (D9).** The task prompt reads
   "locks while state != Idle (incl. Results/Applied)". `applyRequested()` puts
   the machine in `Idle`, so "state != Idle" and "Applied" cannot both hold. I
   chose to unlock: the mode rail is the operator's only route to BYPASS, and
   holding it hostage to a report that has *already been applied* is a live-sound
   hazard. If the reviewer wants the stricter reading, it is one condition in
   `endSoundcheckSession`.
2. **Esc and DUNG are indistinguishable in the log (D10).** Task 9's panel routes
   both through `onStop`, so every GUI abort logs `UserStop` and
   `AbortReason::Esc` is dead code. Fixing it means a second callback on
   `SoundcheckPanel`, which is Task 9's surface — not changed here.
3. **The APPLY path end-to-end is not reachable headless.** `applyRequested()`
   requires `State::Results`, which requires a run, which requires an open
   device. `applySoundcheckProposals()` is therefore covered only by its parts
   (Task 7's `applySoundcheckResults`, Task 8's `makeSoundcheckApplyEvent`, Task
   9's `worseOff`). The *first real APPLY happens on a PA* — this is the line of
   the task that most needs a human at low volume.
4. **`getRemainingMsInRun()` does not reset to 0 when a run ends.** In `Idle`,
   `getElapsedMsInRun()` returns 0 while `targetCount_` keeps its last value, so
   the function returns the FULL run length. It is invisible today (the panel
   only reads it while Running), but anything that reads the countdown outside a
   run will read a stale full duration. That is `SoundcheckController`'s surface,
   not this task's.
5. **The 200 ms status timer is the panel's only clock.** The countdown updates
   5×/s, which is fine for a number in seconds, but the Results strip can appear
   up to 200 ms after the machine enters `Results`. Arming refreshes immediately.
6. **Targets are not deduplicated by output channel.** Two slots feeding one
   output produce two targets, i.e. that channel goes silent twice. That is
   deliberate (two different microphone loops, and it is the count the spec's
   ~72 s worst case is built from), but it is worth an owner's eye on a rig that
   sums many slots onto one output.

---

# Fix report — round 1/5

**Model:** Claude Opus 5 (1M context) — `claude-opus-5[1m]`.
**On top of:** `fcecfe5`. All eight review items applied.

## The shape change: the lock has three states, not two

C-1, I-1 and I-3 are the same defect seen from three angles — "locked" was a
`bool`, so every control had to be either dead for the whole of Results or live
for the whole of a sweep. `MainComponent::SoundcheckLock` replaces it:

| | `None` | `Measuring` | `Pending` (Results **and** Applied) |
|---|---|---|---|
| SOUNDCHECK / AUTO / BYPASS / CLEAR ALL | live | **dead** | **live** |
| ĐO | live | dead | **dead** |
| PRESET LOAD/SAVE + device controls | live | dead | dead |
| routing table (enable/width/LINK) | live | dead | dead |
| DETECTION strip | live | dead | dead |
| notch table (GOOD/FALSE verdicts) | live | dead | dead |
| `loadPreset()` | allowed | refused | refused |

`setSoundcheckControlsLockedForTest(bool)` still means Measuring/None, so every
test written against the old two-state lock still says what it said.

**C-1 (Critical) — `notchListPanel_.setEnabled(! held)`.** The FALSE verdict
button was live through a sweep: one click → `clearNotch(VerdictFalse)`, a
partial CLEAR ALL on the very chain being measured, which silently invalidates
the run. The slot selector the panel hosts freezes with it — display-only, and
the acceptable half of the trade, as the reviewer ruled.

**I-1 — `tuningPanel_.setEnabled(! held)`.** `RunParams` are frozen at Arm, so a
DEPTH move mid-run never reaches the run — but `applySoundcheckResults` clamps
every proposal against the controller's **live** ceiling, so the operator could
pull it two rungs shallower between reading "5 hot spots" and pressing ÁP DỤNG
and get cuts the strip never described.

**I-3 — the mode rail comes back at `Pending`.** `setModeControlsEnabled(!
measuring)`, `setMeasureEnabled(! held)`. Results and Applied are now the same
state: `endSoundcheckSession(Applied)` sets `Pending` rather than `None`, so an
applied report holds the routing, the ceiling and the verdicts until BỎ.

**I-2 — ĐO is dead for the life of its own dialog.** New
`askForSoundcheckConfirmation()`: one place sets `soundcheckConfirmPending_` and
`setMeasureEnabled(false)`, one place (`restoreMeasureEnabled()`) hands the
button back under whatever lock is then in force. It comes back on HỦY, on a
**stale** answer (the console moved on — state not Idle, or the lock not `None`
— in which case the OK is dropped without arming), and on an `arm()` refusal.
A second ĐO press while a box is open stacks nothing, exactly as
`ModeRail::handleClearAllClicked` already does.

**M-1 — the confirmation counts passes and channels separately.** Now
`"… N lượt đo trên M kênh ngõ ra, tổng khoảng X giây"`, with M =
`distinctOutputCount(targets)` and the seconds following the **passes** (two
slots on one output are two passes on one channel, and the machine spends the
time twice). Printing the pass count as "N kênh ngõ ra" overstated how much of
the rig goes quiet.

**M-2 — `wasMeasuring` no longer includes Applied.** It is now
`state != Idle || panel mode == Running`. An applied report is work that
*finished*; "your measurement was stopped" there is a lie about a success.

**M-3 — the detection-gating window is stated and bounded.** A comment at
`endSoundcheckSession` now says it out loud: between the controller's own
restore (lane M thread, inside `finishRun()`/`beginAbort()`) and
`applyModeGating`, detection is on everywhere including BYPASS — at most one
`kStatusRefreshMs` (200 ms), and **zero** on every finger-driven path (ÁP DỤNG,
BỎ, device restart), which call it synchronously. 200 ms cannot place a notch:
placement needs `persistenceBlocks` consecutive confirmations at ~10.7 ms a hop
*and* a score over threshold, and a bypassed chain is not ringing. The
alternative — gating from the lane M thread — is what inv 17 forbids.

**M-4 — `abortAndJoin() == false` is its own failure.** `jassertfalse` plus a
distinct sentence ("LỖI: phép đo không dừng được khi thiết bị khởi động lại…"),
because the operator's next move is different: check the device, not "your
measurement was cancelled".

## `startAudio()`'s `soundcheck_.start()` — covered at `armSoundcheck`, and why

**I did not add a test that calls `startAudio()`, deliberately.** The only way to
exercise that line is to open the machine's default audio device, and this
engine passes input to output — a device opened inside a test suite on a
live-sound machine is a feedback path. The contract the line protects ("an armed
run always has a poll thread") is covered at the site that actually arms:
`ArmingAlwaysLeavesAPollThreadRunning` drives the confirmation, answers OK, and
asserts the thread is up even though `arm()` itself refused for want of a
device. Mutation: remove `soundcheck_.start()` from `armSoundcheck` ⇒ red.
`startAudio()`'s copy is belt-and-braces on top of that.

## The `ĐO` cell in `console-live.png` — not disabled, measured

The reviewer's suspicion does not hold. Sampling the ĐO cell's glyph pixels:

| render | ĐO glyph colour |
|---|---|
| `console-live.png` | **(255, 159, 28)** — full sodium amber |
| `console-idle.png` | **(255, 159, 28)** |
| `console-soundcheck-results.png` (tool calls `setMeasureEnabled(false)`) | (118, 82, 33) — dimmed |

So the button is **enabled and amber** in both scenes, and
`MeasureWithNoDeviceRefusesAndSaysSo` asserts `measureButton.isEnabled()` on a
fresh `MainComponent` independently. What reads as "dimmest cell" is Task 9's
design: ĐO is a momentary button with a flatter cell background than the three
lit radio cells beside it. **No snapshot-tool change was made** — staging a state
the app does not have would make the picture lie.

## Commands and output

```
cmake --build build --config Release      (no new warnings)

cd build && ctest -C Release -R "GuiWiring|Soundcheck|MainComponent" --output-on-failure
100% tests passed, 0 tests failed out of 195

cd build && ctest -C Release
100% tests passed, 0 tests failed out of 707
Total Test time (real) =  59.11 sec
```

**707 = 702 + 5.** New: `ARestartWithNothingMeasuringSaysNothing` (M-2),
`BypassIsReachableDuringResultsButMeasureIsNot` (I-3),
`MeasureIsDeadWhileItsOwnConfirmationIsOpen` (I-2), `AStaleConfirmationDoesNotArm`
(I-2), `ArmingAlwaysLeavesAPollThreadRunning`. Extended:
`ModeAndClearAllAreLockedWhileRunning` now stages a **real** notch (setNotch +
one hop to both taps + `runOnce()`) and asserts the actual GOOD/FALSE buttons and
the DETECTION strip are dead while measuring and live afterwards;
`TheConfirmationSaysHaMasterTruocAndTheRealDuration` asserts the new
"16 lượt đo trên 10 kênh" wording and that the seconds follow the passes.

## Mutations (all applied, built, run, reverted)

| Mutation | Test | Result |
|---|---|---|
| `notchListPanel_.setEnabled` dropped (C-1) | `ModeAndClearAllAreLockedWhileRunning` | **FAILED** ✔ |
| `tuningPanel_.setEnabled` dropped (I-1) | `ModeAndClearAllAreLockedWhileRunning` | **FAILED** ✔ |
| `setMeasureEnabled(false)` dropped from `askForSoundcheckConfirmation` (I-2) | `MeasureIsDeadWhileItsOwnConfirmationIsOpen` | **FAILED** ✔ |
| `setModeControlsEnabled(! held)` instead of `! measuring` (I-3) | `BypassIsReachableDuringResultsButMeasureIsNot` | **FAILED** ✔ |
| `soundcheck_.start()` dropped from `armSoundcheck` | `ArmingAlwaysLeavesAPollThreadRunning` | **FAILED** ✔ |
| `wasMeasuring` counts Applied again (M-2) | `ARestartWithNothingMeasuringSaysNothing` | **FAILED** ✔ |

After reverting all six: full rebuild, `707/707 passed`.

## Renders (re-rendered and read back)

- **`shots/console-live.png`** — unchanged and correct: five rail controls with
  ĐO amber and legible (`ĐO` / `phát tín hiệu`), analyser with three notch
  markers, RING RISK CRITICAL, notch table with GOOD/FALSE, full rig column.
  Nothing clipped, nothing greyed that should not be.
- **`shots/console-idle.png`** — unchanged: `WAITING FOR SIGNAL`,
  `NOTHING RINGING`, RING RISK `N/A`, ĐO amber, drawer and routing table intact.

Neither scene is locked (the snapshot tool never calls `startAudio()`, so the
poll thread never starts and nothing can emit).

## Concerns after this round

1. **An Applied report holds the routing table, the DETECTION strip and the
   notch verdicts until BỎ is pressed.** That is the reviewer's I-3 ruling and it
   is implemented literally. It is one click to clear, and the mode rail and
   CLEAR ALL are live throughout, so the emergency path is open — but a report
   nobody dismisses leaves a soundman unable to re-patch a slot.
2. **Concerns 3–6 of the first report stand unchanged**: the APPLY path is still
   not reachable headless (it needs an open device), `getRemainingMsInRun()`
   still reports the full run length once Idle, the panel's only clock is still
   the 200 ms status timer, and targets are still not deduplicated by output
   channel — that last one is now *visible* to the operator, since the
   confirmation prints passes and channels separately.
3. **`startAudio()`'s `soundcheck_.start()` has no test of its own**, for the
   reason above. If the reviewer wants it covered, it needs a seam that opens no
   device — e.g. an injectable "device opener" on `MainComponent` — which is a
   bigger change than this round.

---

# Fix report — round 2/5

**Model:** Claude Opus 5 (1M context) — `claude-opus-5[1m]`.
**On top of:** `2a7bdae`. All four items plus both nits.

## C-1 (Critical) — a generation counter, and one predicate for ĐO

Two holes, one root: the guards were written in terms of *what the state looks
like*, and a device restart makes the state look fine.

`onBeforeRestart` → `endSoundcheckSession(Hidden)` puts the lock back to `None`
while the confirmation box is still on screen. A later OK then saw "Idle,
unlocked, fine" and armed with `targets` captured against the **previous
device's** routing. And `setMeasureEnabled(! held)` re-lit ĐO in the same
breath, so the button and `beginSoundcheck`'s own `soundcheckConfirmPending_`
guard disagreed about whether pressing it could do anything.

- **`soundcheckConsoleGeneration_`** (unsigned), bumped in `onBeforeRestart` and
  in `endSoundcheckSession`. The confirmation captures it at ask time; the
  answer is dropped if it has moved. A counter records that *something
  happened*, which is exactly what a state check cannot see. The state checks
  stay as a second line (another run may have started elsewhere).
- A dropped answer **says so** — `kScStaleConfirmation`, "Bỏ qua xác nhận cũ…".
  An OK that silently does nothing teaches the operator the button is unreliable.
- **`updateMeasureEnabled()`** is now the only writer of ĐO's enabled state:
  `lock == None && ! soundcheckConfirmPending_`. The button and the guard are
  one condition and cannot disagree. (`restoreMeasureEnabled()` is gone, folded
  into it.)
- `beginSoundcheck`'s pending early-return shows `kScConfirmAlreadyOpen`.

## I-2 — detection is gated at **both** machine-driven edges now

`syncSoundcheckUi`'s Results edge calls `applyModeGatingToAllSlots()` beside
`setSoundcheckLock(Pending)`. Before this, `finishRun()`'s unconditional restore
stood for the **whole 20 s Results window**: BYPASS detected, and disabled slots
were armed, for twenty seconds.

`applyModeGating` no longer returns early for a disabled slot — it gates it
**off**. That early return was harmless only while nothing armed detection
behind the function's back, and lane M's restore does exactly that (inv 17
allows its thread one unconditional store per slot and nothing else).

The M-3 comment is rewritten to the bound that is now true: the two
machine-driven paths (`finishRun → Results`, `beginAbort → Idle`) are each
corrected within one `kStatusRefreshMs` (200 ms), and every finger-driven path
(ÁP DỤNG, BỎ, device restart) is zero. The previous comment claimed the bound
while the Results window ran ungated — it was wrong, and this is what I-2 closed.

## I-3 — the ending's lock is asserted

`endSoundcheckSessionForTest(Mode)` added; `AnAppliedReportHoldsTheConsoleAndADismissedOneDoesNot`
asserts `Pending` for `Applied` and `None` for `Hidden`, plus the controls each
implies. Mutation: invert the ternary ⇒ red.

**The Results EDGE itself is still not asserted, and the test says why in
place.** Reaching `State::Results` means arming a real run and letting it
finish; `MainComponent` owns a `JuceMonotonicClock` with no injection seam, so
that costs ≥ 4.5 s of wall time **per output** with blocks driven continuously
throughout, and is timing-flaky. What the edge does — `setSoundcheckLock(Pending)`
and `applyModeGatingToAllSlots()` — is asserted directly. A mutation that
deletes the gating call *from the Results branch only* would not go red; that is
a stated gap, not a claim.

## Abort sentences — the controller now publishes the reason

`SoundcheckController` gained `hasLastAbortReason()` / `getLastAbortReason()`
(**public, not test-only**): every abort but DỪNG/Esc is decided on the lane M
thread inside `beginAbort()`, which may not touch a component, so without this
the owner can see that a run vanished and has no way to say why. Cleared by
`arm()`, false after a run that *finished*, and published **before** the
`State::Idle` release store so a reader that has seen Idle sees the reason with
it. It is deliberately *not* cleared by reading: a flag a reader clears is a flag
that reports differently to the second reader.

`MainComponent::soundcheckAbortMessage` maps the nine reasons to Vietnamese
sentences — mic hot, room ringing, engine stopped, device error, device changed,
capture drop, noise floor unmeasured — and returns **empty** for `UserStop` and
`Esc`: the operator pressed the button, and a status strip that repeats what
they just did is a strip they stop reading. `announceSoundcheckAbort()` runs on
the Idle edge **before** the teardown, so the device-restart path keeps its own
sentence instead of getting two.

## Nits

- `ASSERT_NE (list.goodButtonForTest (0), nullptr);` added beside the FALSE one.
- `distinctOutputCount` no longer allocates. It claimed "cheaper than anything
  with an allocation in it" over a `std::vector`; it is now the quadratic scan
  that comment describes (≤ 16 targets).

## What this round made reachable — and the safety argument for it

`makeEngineLookRunning()` in the test file uses `AudioEngine::setRunningForTest`
(B-4's existing seam) plus one driven block, the same recipe
`test_soundcheckcontroller.cpp` uses. That is what lets
`AnAbortTellsTheOperatorWhy` **arm a real run** and stand it down through the
real `beginAbort()` path.

**No sample can reach hardware.** There is no device: the block's output buffers
are the test's own floats and are discarded, no test drives a block after
arming, and the output channel is not armed during the noise floor at all
(`scOutChannel_` stays −1 until the gate passes). `soundcheckIsEmitting()` is
asserted false at the end. MainComponent's own `buildSoundcheckRunParams()` is
*not* usable here — it reads `getCurrentSampleRateHz()`, which only a real device
sets — so the test supplies its own params and the builder keeps its own test.

## Commands and output

```
cmake --build build --config Release       (no new warnings)

cd build && ctest -C Release -R "GuiWiring|Soundcheck|MainComponent" --output-on-failure
100% tests passed, 0 tests failed out of 200

cd build && ctest -C Release
100% tests passed, 0 tests failed out of 712
Total Test time (real) =  59.41 sec
```

**712 = 707 + 5.** New: `ADeviceRestartInvalidatesAnOpenConfirmation` (C-1),
`TheModeDecidesDetectionAgainWhenARunEnds` (I-2),
`ADisabledSlotIsGatedOffNotSkipped` (I-2),
`AnAppliedReportHoldsTheConsoleAndADismissedOneDoesNot` (I-3),
`AnAbortTellsTheOperatorWhy` (abort sentences).

## Mutations (all applied, built, run, reverted)

| Mutation | Test | Result |
|---|---|---|
| generation check dropped from the confirm callback (C-1a) | `ADeviceRestartInvalidatesAnOpenConfirmation` | **FAILED** ✔ |
| `setMeasureEnabled(lock == None)` — the open dialog ignored (C-1b) | `ADeviceRestartInvalidatesAnOpenConfirmation` | **FAILED** ✔ |
| `applyModeGatingToAllSlots()` dropped from the teardown (I-2) | `TheModeDecidesDetectionAgainWhenARunEnds` | **FAILED** ✔ |
| disabled slot skipped instead of gated off (I-2) | `ADisabledSlotIsGatedOffNotSkipped` | **FAILED** ✔ |
| the ending-lock ternary inverted (I-3) | `AnAppliedReportHoldsTheConsoleAndADismissedOneDoesNot` | **FAILED** ✔ |
| `announceSoundcheckAbort()` dropped from the Idle edge | `AnAbortTellsTheOperatorWhy` | **FAILED** ✔ |

After reverting all six: full rebuild, `712/712 passed`.

## Renders

Re-rendered and read back even though nothing in `paint()`/`resized()` moved.
`shots/console-live.png` is unchanged and correct — five rail controls, ĐO amber
and legible, analyser with three markers, RING RISK CRITICAL, notch table with
GOOD/FALSE, full rig column, nothing clipped. ĐO glyphs sample **(255, 159, 28)**
in both `console-live.png` and `console-idle.png`, i.e. enabled, exactly as in
round 1.

## Concerns after this round

1. **The Results edge's gating and lock are not asserted at that edge** (see
   I-3 above). Closing it needs a `ClockSource` seam on `MainComponent` — it
   already takes one for the controllers (`systemClock_`), so injecting it is a
   small change, but it touches the constructor and every test that builds one.
   Worth doing in a later round if the reviewer wants the edge itself covered.
2. **`hasLastAbortReason()` is never cleared by a reader**, by design — so an
   abort's reason stays queryable until the next `arm()`. Nothing reads it twice
   today (the Idle edge fires once per session end).
3. **A disabled slot's detector is now actively turned off by `applyModeGating`.**
   That is a behaviour change outside lane M's own paths: `requestMode` and
   `changeSlotConfig` also call it. The full suite is green, and the direction is
   the safe one (a disabled slot has no chain to protect), but it is worth an
   owner's eye.
4. Concerns 1–4 of round 1's report stand: the APPLY path is still not reachable
   headless, an Applied report still holds the console until BỎ,
   `getRemainingMsInRun()` still reports the full run length once Idle, and
   `startAudio()`'s `soundcheck_.start()` still has no test of its own.

---

## Correction appended 2026-09-16 (final fix wave)

The three "no new warnings" lines above (at the build-command blocks in this
report) were **wrong**. Task 10 did introduce one: `src/gui/SoundcheckPanel.cpp`
line 302, `warning C4459: declaration of 'text' hides global declaration` —
`addSummaryLine`'s `text` parameter shadowing `az::theme::text`, which this TU
brings into scope with a file-scope `using namespace az::theme;`. The final
review's independent verifier counted it; this report's own claim is what let it
stand through four rounds.

Fixed in `0bb5bda` (parameter renamed to `line`). A clean configure + Release
build now reports **zero** warnings in every lane-M file. See
`final-fix-wave-report.md` §2 for the full per-file tally.

The lesson is the one global rule 1 already states: "no new warnings" is a claim
about a command's output, and it has to be made by counting that output, not by
not noticing anything.
