# Task 6 report — `SoundcheckController`

**Status:** DONE_WITH_CONCERNS (nothing half-finished; the concerns are handoffs to Tasks 7/10/11).
**Commit:** `93a7253` on `feat/lane-m-active-soundcheck`, BASE `e0b8dc9`.
**Suite:** `627/627` (603 + 24). Model actually used: **Opus 5 (1M context)**.

## What landed

- `src/app/SoundcheckController.h` / `.cpp` — the lane M state machine on a
  `ClockSource`, driving the eight `AudioEngine` soundcheck atomics and the pure
  DSP of Tasks 1-3. Holds **no** `NotchController` pointer (inv 17); detection is
  restored through the injected `setDetectionActiveOnAllSlots` lambda,
  synchronously, **before** the state reaches `Results` (inv 12).
- `tests/test_soundcheckcontroller.cpp` — 23 tests on a fake clock and a
  hand-driven callback; no test starts a thread.
- `tests/test_notchcontroller.cpp` — `APreventiveNotchNeitherWritesNorConsumesRoomMemory`
  appended (N-4).
- `CMakeLists.txt`, `tests/CMakeLists.txt` — registration.

## Thread map (every public method)

| Method | Thread |
|---|---|
| `preflight` | message (also records the risk snapshot for `soundcheck_start`) |
| `arm` | message — every allocation of the run happens here |
| `applyRequested` / `dismissRequested` | message |
| `requestStop` | message — **also stops the sound itself** (see D5) |
| `abortAndJoin` | message — `requestStop` + one `runOnce()` + `stop(1000)` |
| `start` / `stop` | message |
| `runOnce` | lane M (or the test's thread) — the whole machine |
| `run` | lane M — `runOnce()` + `wait(kPollMs)` and nothing else |
| `getState`, `getCurrentTargetIndex`, `getTargetCount`, `getElapsedMsInRun`, `getRemainingMsInRun`, `copyResults`, `copyResultsForSlot`, `worstPeakinessForTest` | any |
| `setDetectionActiveOnAllSlots`, `logEvent`, `onStateChanged` | **invoked** on lane M; **assigned** on message before `start()`, never after (I-10) |
| audio callback | not entered here at all |

## Brief-vs-header divergences (header/reality wins)

1. **`kNoiseFloorGuardMs = 20.0` added.** The gate must decide *before* the first
   sample, and the callback starts the sweep the instant `scSampleIndex_` reaches
   0. The index starts one guard earlier than the gate deadline. Cost: 4.52 s per
   channel, ~72.3 s for 16. Pinned by `NoiseFloorWithARingingToneAborts`, which
   asserts the measured channel never left zero. **The first implementation added
   the guard to the deadline as well, and the fixture emitted 1.9 mV of sweep —
   that test caught it.**
2. **`arm()` validates.** Refuses a non-finite `ceilingDb` (Task 3's NaN = unset
   contract), `noiseFloorGate <= 0`, `sampleRate <= 0`, empty targets, non-positive
   channel counts, and a run already in flight. Each is a caller bug that would
   otherwise reach the operator dressed as data about the room.
3. **`OutputResult` gained `ceilingMissing` / `ladderMissing`,** surfaced from
   `pick()` and written into `soundcheck_output`. The brief's struct had no field
   for the error state the task instruction requires.
4. **`preflight()` records the risk snapshot; `arm()` logs it.** `arm(targets,
   params)` takes no snapshot, so `ring_risk` could otherwise only ever be null.
   Two `mutable std::atomic` members hold the last preflight's `(valid, score)`.
5. **`requestStop()` stops the sound on the message thread.** The brief treats it
   as a flag serviced by `runOnce()`, but `AbortRampsDownInTheCallbackAlone` never
   polls after the request.
6. **The abort backstop is chosen by the SAMPLE INDEX, not the phase.**
   `index >= 0` → `requestSoundcheckRampOut()` and the channel is left alone
   (touching it would discard the ramp, C-2). `index < 0` → `setSoundcheckOutputChannel(-1)`,
   which is silent because nothing is being emitted, and which avoids the
   NoiseFloor deferral the brief's "wait for scOutChannel_ to reach -1" would hang on.
7. `AudioEngine::getCurrentSampleRateHz()` is the accessor (default `48000.0` with
   no device — which is what makes `SampleRateChangeAborts` meaningful).
8. **The noise window's spectrum is the MEAN power over its frames,** not one
   frame. A ring is sustained by definition; averaging removes the single-frame
   excursion that crossed 10.0 at 13.99 in `memory/peakiness-sweep-2048-2026-09-04.md`.
   The brief specified `noiseMagnitudes_` with no construction rule.
9. **`liveNotchHz` is passed empty** — see concern C1.
10. **Test corrections.**
    - `ResultsTimeoutIsTwentySeconds`: the brief's own arithmetic overran the
      timeout it asserted had *not* fired (`pump(12000)` = 3000 ms into Results,
      `+19000` = 22000 > 20000). Replaced with `pumpUntil(Results)`.
    - `SequencesOneOutputChannelAtATime`, `AbortRampsDownInTheCallbackAlone`:
      **silent room.** `peakOn()` also sees programme through the *un-muted* lane,
      so with the briefed noisy fixture "both channels non-zero" holds all run,
      and `EXPECT_FLOAT_EQ(peakOn(0), 0.0f)` after an abort is false because the
      lane is un-muted again once the callback releases the channel.
    - `NoiseFloorGateIsReadAtArm`: the briefed amplitude `0.004` gives peakiness
      about 74 (both gates abort). Derived `5.8e-4` from
      `peakiness = sqrt(1 + 295.5*(A/sigma)^2)` for a target of 10 — a factor of 2
      clear of both gates — with the derivation in the test comment (B-5).
    - `NoNotchCommandIsEmittedDuringARun`: **queue depth alone cannot see a
      command** — the callback drains every queue every block. Mutation-confirmed:
      writing a `NotchCommand` inside `analyseCurrentTarget` left the briefed
      assertions green. Added a per-slot/lane `NotchChain::NotchInfo::state`
      assertion, which kills it.
    - `APreventiveNotchNeitherWritesNorConsumesRoomMemory`: the brief's step 1
      used `probeMemoryAt` to *build* the memory entry, but that helper clears
      **manually** precisely so it leaves none; and its step 3 re-probe would find
      the entry consumed by step 3's own detector placement. Rebuilt: a real -24
      **detector** notch auto-released to Clear builds the entry (the only path
      that writes one, `NotchController.cpp:843`), then the not-consumed half at
      that bin **and** a not-written half at a fresh bin.
11. **Counts.** The brief said 21 tests and a 608-test suite; actual is 24 new and
    **627**.
12. **Attribution.** The task named `Co-Authored-By: Claude Fable 5.1`. The model
    that actually ran is Opus 5 (1M context) and global rule 5 says record the
    model actually used, so the commit carries the Opus line. Amend if the
    coordinator wants otherwise.

## Commands

RED (Step 2), with only the test file registered:

```
cmake --build build --config Release --target HandsFreeTests
tests\test_soundcheckcontroller.cpp(10,10): error C1083:
    Cannot open include file: 'app/SoundcheckController.h': No such file or directory
```

GREEN:

```
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
cd build && ctest -C Release -R SoundcheckController --output-on-failure
    100% tests passed, 0 tests failed out of 23
cd build && ctest -C Release
    100% tests passed, 0 tests failed out of 627
    Total Test time (real) =  48.70 sec
```

Pristine build: a re-run of `cmake --build build --config Release` produces no
`error` and no `warning C` lines.

## Mutation results — 8 of 8 died

| # | Mutation | Covering test | Result |
|---|---|---|---|
| M1 | `NotchCommand` written mid-run | `NoNotchCommandIsEmittedDuringARun` | **SURVIVED** with queue-depth assertions only → test strengthened with chain state → **RED** |
| M2 | drop `kRiskFreezeFraction *` (gate becomes `>= threshold`) | `RefusesWhenRingRiskIsRising` | RED |
| M3 | gate compared against `0.7f` (kConfirmScore) | `NoiseFloorOfAQuietRoomDoesNotAbort` | RED |
| M4 | `noiseWindowIsRinging()` returns false | `NoiseFloorWithARingingToneAborts` | RED |
| M5 | `setDetectionActiveOnAllSlots(true)` removed from `finishRun` | `DetectionIsRestoredBeforeResults` | RED |
| M6 | hot-mic hold removed (abort on the first hot poll) | `HotMicAbortsOnlyAfterTheHold` | RED |
| M7 | sample-rate comparison removed from `checkDeviceUnchanged` | `SampleRateChangeAborts` | RED |
| M8b | `clearNotch(SoundcheckReplace)` writes room memory | `APreventiveNotchNeitherWritesNorConsumesRoomMemory` | RED |

Also measured: removing the `origin != Origin::Soundcheck` guard at
`NotchController.cpp:1116` is already caught by lane G's own
`ASoundcheckPlacementDoesNotSpendTheRoomMemory` (full-suite run: exactly that one
test red). The new N-4 test is complementary — it covers the
`setNotch`/`clearNotch` path lane M actually uses, which never reaches
`placeConfirmed`.

## Concerns

- **C1 (hands off to Task 7).** `SoundcheckCandidates::Input::liveNotchHz` is
  passed **empty**: inv 15 (a proposal must not land on a bin a notch already
  holds) cannot be enforced on the controller thread, because reading the live
  notch list means calling `NotchController` (inv 17). It must be enforced inside
  `applySoundcheckResults`, on the message thread, which has both the controller
  and the model. **Until Task 7 does it, inv 15 is unenforced.**
- **C2.** `applySoundcheckResults` is declared in the header and **not defined**
  (Task 7). Nothing calls it, so nothing fails to link.
- **C3.** `applyRequested()` does not log `soundcheck_apply` — its
  `placed`/`refused`/`cleared_previous` counts only exist after Task 7's call, so
  Task 10 must log it beside that call.
- **C4 (docs, Task 11).** The gate's mean-power ruler is **more conservative than
  the live detector's per-frame one**: a room the detector would call ringing on a
  single frame can pass lane M's gate. Deliberate — but it is a real difference
  between two numbers the spec presents as the same threshold, and it belongs in
  the docs.
- **C5.** `NoiseFloorGateIsReadAtArm`'s peakiness is asserted only as a bracket
  (`5 < x < 20`); the exact value is not printed. A drifting fixture therefore
  fails as a fixture, with a message that says so.
- **C6.** Nothing tests `abortAndJoin()` or the real thread (`start()` / `run()`);
  every test drives `runOnce()` by hand. The device-restart contract that Task 5's
  `audioDeviceAboutToStart` depends on — abort **and join** before restart — is
  therefore implemented but never exercised. **Task 10 should test it against the
  real thread.**
- **C7 (docs, Task 11).** Per-channel time is **4.52 s**, not 4.5 s, and 16
  channels are ~72.3 s. `kPerTargetMs` is exported so Task 9/10 derive the dialog
  string instead of repeating a literal; spec section 3's prose still says
  4.5 s / 72 s.
- **C8.** `micIsHot()` uses the RMS of the samples drained in **one poll**
  (~5 ms at 256 frames) plus the `kMicAbortHoldMs` hold, not the brief's prose
  "20 ms sliding RMS". Chosen deliberately: a true 20 ms window would reject the
  briefed 5.33 ms burst on its own, and `HotMicAbortsOnlyAfterTheHold` would then
  pass for an implementation with no hold at all (M6 proves the hold is what the
  test kills).
- **C9.** A poll that drains **zero** samples leaves the hot-mic timer untouched
  (neither armed nor cleared), so a device delivering late cannot look like a mic
  that went quiet. Worth a reviewer's eye.

---

# Task 6 — fix round 1/5

**Status:** all 10 findings addressed (3 Critical, 2 spec gaps, 5 Important) plus every
minor. **Model actually used: Opus 5 (1M context)** — the commit carries the lane
convention trailer `Claude Fable 5.1` as instructed; this line is the real record.

**Suites:** `-R "SoundcheckController|NotchController"` **159/159**; full `ctest -C
Release` **637/637** (was 627). Pristine build: an up-to-date rebuild emits no
`error C####` and no `warning C####`.

## Changes

| # | Change | Where |
|---|---|---|
| C-1 | The output channel is **not armed during the noise floor at all**. `enterTarget()` opens the capture gate only; `scOutChannel_` stays −1, so no audio clock runs toward sample 0 and a late poll is not in a race. A new `armSweepForCurrentTarget()` runs only after the gate says quiet: it stamps `noiseFloorEndSample_`, publishes `scSampleIndex_ = -sweepLeadInSamples_` (relaxed) and then `setSoundcheckOutputChannel()` (release). `kNoiseFloorGuardMs` renamed **`kSweepLeadInMs`** and re-documented as the sweep run-up. The N/Y split no longer derives from the sweep index: `noiseFloorEndSample_` is `INT64_MAX` until the gate passes. | `SoundcheckController.{h,cpp}` |
| C-2 | `abortAndJoin()` is now **request → `stop(1000)` (join) → `runOnce()` on the caller thread**, returns `bool`, `jassert`s Idle. Header states the poll thread is stopped afterwards and Task 10 must `start()` again. | `.h`, `.cpp` |
| C-3 | `stop(int)` stands the **run** down, not just the thread: request (if none pending), join, then `beginAbort()` synchronously on the caller thread. The destructor inherits it through `stop(2000)`. Header and a comment beside the destructor state that an owner must declare its `AudioEngine` and `ClockSource` **before** its `SoundcheckController`. | `.cpp` |
| S-1 | `arm (targets, params, const NotchController::SnapshotBuffer&)` — a fresh ring-risk read **at Arm**, same identity as preflight, and it is that read `soundcheck_start` logs. `preflight()` is now truly `const`; the `mutable` cache is gone. | `.h`, `.cpp` |
| S-2 / I-3 | `stopEmissionSafely()` takes the **backstop unconditionally when `! engine_.isRunning()`** — a flag no block will ever read is not a stop. Every abort test now ends in the shared `expectEngineStoodDown(r)` helper, which asserts the channel released, no fade left pending, the capture gate shut and **the taps live again**. | `.cpp`, tests |
| I-1 | The Rig records the state the detection restore **ran in**; the test asserts it was not `Results`. Sampling after `runOnce()` returns cannot see the order of two statements. | tests |
| I-2 | The gate maxes `peakinessAt` only over `[kSweepLowHz, kTrustedHighHz]` via `LoopGainEstimator::hzToBin`. An LED whine at 17 kHz or mains hum would otherwise refuse every run in the venue while saying nothing about the measured band. | `.cpp` |
| I-4 | `arm()` re-runs the full target validation (shared `validateTargets()`), because a slot can be disabled or re-patched while the Confirm dialog is up. | `.cpp` |
| I-5 | `requestStop()` also closes the capture gate and **lifts `scSuspendTaps_` itself** — both safe-direction, neither waiting for a poll; a suspended tap is a deaf feedback killer. Detection stays with the lane M thread (I-10). | `.cpp` |
| min | Gate **fails closed**: `computeNoiseSpectrum()` returns `bool`, and zero frames aborts with the new reason `noise_floor_unmeasured`. | `.cpp` |
| min | `arm()` returns `Refusal`; three enumerators **appended** (`InvalidParams`, `AlreadyRunning`, `RampOutPending`). | `.h` |
| min | `arm()` refuses while a fade is still running, via a new additive `AudioEngine::isSoundcheckRampOutPending()`. | `AudioEngine.{h,cpp}` |
| min | `NoiseFloorGateIsStableWithinARun` deleted (could not fail). | tests |
| min | New `ProgressReadsTrackTheRun` covers `getTargetCount/getCurrentTargetIndex/getElapsedMsInRun/getRemainingMsInRun` across a 2-target run. | tests |
| min | Header states `State::Preflight/Confirm/Arm` are **GUI-owned and never entered by this machine** — Task 9 must not drive the dialog from `getState()`. | `.h` |
| min | `ladderMissing` documented as a tautology today, and why it is kept. | `.h` |

## New and changed tests (11 added, 1 deleted, net +10 in the file: 23 → 33)

`SweepIsNotArmedUntilTheGateSaysQuiet`, `GateIgnoresBinsOutsideTheSweepBand`,
`AnUnmeasurableNoiseFloorFailsClosed`, `ArmRefusesWhileAFadeIsStillRunning`,
`AbortAndJoinJoinsBeforeItRuns` (the **only** test that drives the real poll
thread — it closes round 1's concern C-6), `DestroyingMidRunStandsTheEngineDown`,
`EngineStoppedAbortReleasesTheChannelWithNoCallback`, `DeviceErrorAborts`,
`ArmRefusesWhenRingRiskRoseDuringConfirm`, `ArmRefusesATargetDisabledDuringConfirm`,
`ProgressReadsTrackTheRun`.

## Commands and output

```
cmake --build build --config Release
cd build && ctest -C Release -R "SoundcheckController|NotchController" --output-on-failure
    100% tests passed, 0 tests failed out of 159
    Total Test time (real) =  12.27 sec
cd build && ctest -C Release
    100% tests passed, 0 tests failed out of 637
    Total Test time (real) =  52.90 sec
```

## Mutation results — 11 of 11 died

| # | Mutation | Covering test | Result |
|---|---|---|---|
| N1 | arm the output channel during the noise floor (the C-1 defect, restored) | `SweepIsNotArmedUntilTheGateSaysQuiet` | RED |
| N2 | `abortAndJoin()` never joins | `AbortAndJoinJoinsBeforeItRuns` | RED |
| N3 | `stop()` stops the thread only | `DestroyingMidRunStandsTheEngineDown` | RED |
| N4 | `arm()` ignores its risk snapshot | `ArmRefusesWhenRingRiskRoseDuringConfirm` | RED |
| N5 | no backstop for a dead callback | `EngineStoppedAbortReleasesTheChannelWithNoCallback` | RED |
| N6 | detection restored **after** `Results` is published | `DetectionIsRestoredBeforeResults` | RED |
| N7 | gate scores every bin | `GateIgnoresBinsOutsideTheSweepBand` | RED |
| N8 | `arm()` trusts preflight's routing | `ArmRefusesATargetDisabledDuringConfirm` | RED |
| N9 | `requestStop()` leaves the taps suspended | `AbortRampsDownInTheCallbackAlone` | RED |
| N10 | gate fails **open** on an unmeasured floor | `AnUnmeasurableNoiseFloorFailsClosed` | RED |
| N11 | arm allowed on top of a running fade | `ArmRefusesWhileAFadeIsStillRunning` | RED |

N6 and N9 both needed the test strengthened first: N6 survives any assertion
sampled after `runOnce()` returns, and N9 survives any assertion sampled after
the first poll, because `beginAbort()` lifts the taps too.

## Abort-path table — all ten rows

| Reason | Raised by | How the sound stops | Covering test |
|---|---|---|---|
| `UserStop` | `requestStop()` from the GUI STOP button | idx ≥ 0 → ramp-out, callback releases the channel; idx < 0 → backstop | `AbortRampsDownInTheCallbackAlone`, `ArmRefusesWhileAFadeIsStillRunning` |
| `Esc` | `requestStop()` from Task 9's key handler | identical code path to `UserStop` — the reason is the only difference | `EveryAbortReasonHasItsOwnName` (it has no behaviour of its own to test) |
| `EngineStopped` | `runOnce()`: `! engine_.isRunning()` | **backstop unconditionally** — no callback exists to run a fade | `EngineStoppedAbortReleasesTheChannelWithNoCallback` |
| `DeviceError` | `runOnce()`: `getLastDeviceError()` non-empty | ramp while running, backstop once the engine is down | `DeviceErrorAborts` |
| `DeviceChanged` | `runOnce()`: `checkDeviceUnchanged()` false; also every `abortAndJoin()` | per index; `abortAndJoin()` joins the poll thread first | `SampleRateChangeAborts`, `ChannelCountChangeAborts`, `AbortAndJoinJoinsBeforeItRuns` |
| `MicHot` | `runOnce()`: `micIsHot()` sustained past `kMicAbortHoldMs` | ramp-out (always mid-sweep by construction) | `HotMicAbortsOnlyAfterTheHold` |
| `RoomRinging` | the noise-floor gate | backstop — the channel was **never armed** (C-1) | `NoiseFloorWithARingingToneAborts`, `GateIgnoresBinsOutsideTheSweepBand` |
| `CaptureDrop` | `runOnce()`: `getMicCaptureDropCount()` moved | per index | `CaptureDropAborts` |
| `NoiseFloorUnmeasured` | the gate, zero frames — **fail closed** | backstop — never armed | `AnUnmeasurableNoiseFloorFailsClosed` |
| stand-down | `stop(int)` and the destructor (C-3) | join, then `beginAbort()` with whatever reason was pending, else `UserStop` | `DestroyingMidRunStandsTheEngineDown` |

## Concerns after this round

- **Still open, unchanged: inv 15.** `liveNotchHz` is empty; Task 7's
  `applySoundcheckResults` must enforce "no proposal on a bin a notch already
  holds". Nothing in this round changed that.
- **`Esc` has no fixture of its own** — deliberately: it is `UserStop`'s code path
  with a different string, and a test would assert the string twice.
- **`AbortAndJoinJoinsBeforeItRuns` proves the post-conditions, not the race.**
  It pins that the thread is joined, the state is Idle and the engine is stood
  down; the wrong order (run-then-join) fails it only via the missing join (N2),
  not by reproducing the Gap/`enterTarget` interleaving, which is not
  deterministically reachable from a test.
- **`DeviceError` leaves `lastDeviceError_` set** for the rest of the engine's
  life (it is only cleared by a successful `start()`), so a run armed after a
  device error and before a restart aborts immediately. That is the safe
  direction, but Task 10 should make the GUI say so rather than letting the
  operator press START into an instant abort.
- **C4/C7 from round 1 still stand** (the gate's mean-power ruler is more
  conservative than the detector's per-frame one; 4.52 s per channel, ~72.3 s for
  16) — both are docs work for Task 11.

---

# Task 6 — fix round 2/5

**Status:** all four items applied (N-1, N-2, the C-3 and S-2 partials) plus the four
lower-severity ones. **Model actually used: Opus 5 (1M context)**; the commit carries
the lane convention trailer `Claude Fable 5.1`.

**Suites:** `-R "SoundcheckController|NotchController"` **160/160**; full `ctest -C
Release` **638/638** (was 637). Pristine build: no `error C####`, no `warning C####`.

**Correction to the round-1 report:** the "10 added" label was wrong. Round 1 added
**11** tests and deleted 1 — net +10, taking the file from 23 to 33. Round 2 adds one
more (34 in the file, 160 with `NotchController`).

## Changes

| # | Change | Where |
|---|---|---|
| N-1 | The Sweep deadline is **re-stamped from the gate poll** (`phaseEndsAtMs_ = clock_.nowMs() + kSweepLeadInMs + kSweepSeconds*1000.0`), not accumulated from the noise-floor deadline. After C-1 the audio is anchored to the instant the channel is published, so a poll that is L late left the deadline L short: under 700 ms it silently under-measured the tail; past 700 ms the Gap arrived while the sweep was at full amplitude. Tail and Gap keep accumulating from the re-stamped value, which is correct **because** it is now anchored to the audio. | `.cpp` gate branch |
| N-1 | New `releaseOutputChannelSafely()`, used by **both** `enterGap()` and `finishRun()`: if the callback could still be producing a non-zero sample (`soundcheckIsEmitting() && 0 <= idx < sweepTotalSamples_`) the release is handed to the **ramp-out** exactly as an abort would hand it over (inv 9), instead of a bare store that would be a hard cut at whatever amplitude the sweep had reached. `sweepTotalSamples_` is recorded at `arm()`. | `.h`, `.cpp` |
| N-2 | The header thread map now says that **`stop()`, `abortAndJoin()` and the destructor invoke all three lambdas on the CALLING thread** as their last act, and the I-10 lifetime contract says so too. | `.h` |
| N-2 | **Documented, not `jassert`ed — and the reason is stated in the header.** The last of the three callers is a *destructor*, which runs on whatever thread owns the object, and a Debug-only assertion fires long after the dangling reference it is meant to prevent has been formed. The property that actually prevents it is declaration order, which is checkable by reading the owner. | `.h` |
| C-3 (partial) | That declaration-order rule now lives **in the header**, on the lambda members where a caller will read it: the owner must declare `SoundcheckController` **after** its `AudioEngine`, `ClockSource`, `SessionLogger` and every GUI member the lambdas reach. The `.cpp` comment is reduced to a pointer at it. | `.h`, `.cpp` |
| S-2 (partial) | `expectEngineStoodDown(r)` now ends **every** abort test: added to `EngineStoppedAbortReleasesTheChannelWithNoCallback` (the rig is put back to running first, since the helper drives blocks), `DeviceErrorAborts` and both halves of `GateIgnoresBinsOutsideTheSweepBand`. | tests |
| low | `arm()` check order: `validateTargets()` before the `RampOutPending` check, with the reason in a comment — "the device is gone" outranks "a fade is still finishing", because a stopped engine cannot finish a fade at all and Task 9 would otherwise tell the operator to wait for something that will never happen. | `.cpp` |
| low | `[[nodiscard]]` on `abortAndJoin()`. | `.h` |
| low | `jassert` on `armSweepForCurrentTarget()`'s out-of-range early return, with why it matters: the silent version arms no sweep and the run then measures a channel it never drove and reports it as unmeasurable. | `.cpp` |
| low | Round-1 report label corrected (above). | report |

## New test

`LateGatePollDoesNotShortenTheTailOrCutTheSweep` — drives 800 ms **unpolled** past
the gate deadline (past `kTailSeconds`, which is what makes the bug audible), then:

- (a) polls to `armedAt + 3000 ms`, just before the sweep audio ends, and asserts the
  machine is still in `Sweep`, the channel is still armed, **no ramp-out is pending**
  (i.e. nothing asked for an emergency fade) and something is still being emitted.
  The old arithmetic entered Gap at `armedAt + 2920 ms`, 100 ms early;
- (b) runs the rest of both targets and asserts that **every release performed by the
  controller landed on a silent channel** — the peak of the block immediately before
  each release is exactly 0.

## Commands and output

```
cmake --build build --config Release
cd build && ctest -C Release -R "SoundcheckController|NotchController"
    100% tests passed, 0 tests failed out of 160
    Total Test time (real) =  12.35 sec
cd build && ctest -C Release
    100% tests passed, 0 tests failed out of 638
    Total Test time (real) =  49.54 sec
```

## Mutation results

New this round, plus **every round-1 mutation re-run against the refactored code**
(the `arm()` reorder and `releaseOutputChannelSafely()` moved the ground under them):

| # | Mutation | Covering test | Result |
|---|---|---|---|
| P1a | sweep deadline accumulated instead of re-stamped (the N-1 defect) | `LateGatePollDoesNotShortenTheTailOrCutTheSweep` | RED |
| P1b | defensive ramp removed from `releaseOutputChannelSafely()`, re-stamp kept | — | **SURVIVED, and that is the honest result** — see below |
| N1 | arm the output channel during the noise floor | `SweepIsNotArmedUntilTheGateSaysQuiet` | RED |
| N2 | `abortAndJoin()` never joins | `AbortAndJoinJoinsBeforeItRuns` | RED |
| N3 | `stop()` stops the thread only | `DestroyingMidRunStandsTheEngineDown` | RED |
| N4 | `arm()` ignores its risk snapshot | `ArmRefusesWhenRingRiskRoseDuringConfirm` | RED |
| N5 | no backstop for a dead callback | `EngineStoppedAbortReleasesTheChannelWithNoCallback` | RED |
| N6 | detection restored after `Results` is published | `DetectionIsRestoredBeforeResults` | RED |
| N7 | gate scores every bin | `GateIgnoresBinsOutsideTheSweepBand` | RED |
| N8 | `arm()` trusts preflight's routing | `ArmRefusesATargetDisabledDuringConfirm` | RED |
| N9 | `requestStop()` leaves the taps suspended | `AbortRampsDownInTheCallbackAlone` | RED |
| N10 | gate fails open on an unmeasured floor | `AnUnmeasurableNoiseFloorFailsClosed` | RED |
| N11 | arm allowed on top of a running fade | `ArmRefusesWhileAFadeIsStillRunning` | RED |

**P1b survives on purpose, and no test can kill it.** With the deadline correctly
re-stamped, `releaseOutputChannelSafely()` is only ever reached 700 ms after the
sweep audio has ended, so the defensive branch is unreachable from any input — the
only way to enter it is a *future* timing bug. It is kept for the same reason the
NaN guards and the output clamp are kept: this is the last store before a PA, the
failure mode is a hard cut at full amplitude, and "by construction it cannot
happen" is precisely the claim a later change breaks. Removing it to gain a
mutation score would be weakening a safety guard to save a line (global rule 10).
P1a is the mutation that proves the branch's *reason* is real: revert the
re-stamp and the machine does reach Gap mid-sweep.

## Concerns after this round

- **inv 15 is still open** (unchanged since round 1): `liveNotchHz` is empty; Task 7's
  `applySoundcheckResults` must enforce "no proposal on a bin a notch already holds".
- **The `arm()` check order is not mutation-covered.** Swapping `validateTargets()`
  back behind the `RampOutPending` check needs a fixture with a stopped engine AND a
  pending fade; the difference is only which of two refusals Task 9 displays, so no
  test was written for it.
- **N-2 is documentation.** Nothing in the test suite can fail if a future owner
  declares `SoundcheckController` before its `AudioEngine`; the guard is a header
  contract and a code review. Task 10's `MainComponent` is the first real caller and
  should be reviewed against it explicitly.
- **`DeviceError` still latches** — `lastDeviceError_` is cleared only by a successful
  `start()`, so every `arm()` after a device error aborts at once until the device is
  restarted. Safe direction; Task 10 should say so in the GUI.
- Round-1 C4/C7 stand: the gate's mean-power ruler is more conservative than the
  detector's per-frame one, and a channel costs 4.52 s (~72.3 s for 16) against spec
  §3's 4.5 s / 72 s. Both are Task 11 docs work.
