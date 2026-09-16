# Task 5 report — `AudioEngine` emits the soundcheck sweep

**Status: DONE.** Commit: **`47d324c`** on `feat/lane-m-active-soundcheck`. Suite **595/595**, `AudioEngine`-filtered **48/48**
(37 pre-existing + 10 new + 1 `MainComponent` name match).

Worktree: `D:/DEV CAVE EP3/PROJECT005-AZ-handsfree/.claude/worktrees/lane-m-soundcheck-0915`,
branch `feat/lane-m-active-soundcheck`, base `40818df`.

---

## 1. What was built

All four insertion points landed where the brief put them; every anchor in the
brief matched the real file **verbatim**, so nothing had to be guessed. The diff
is **741 insertions, 0 deletions** across the three files — no existing line of
the callback was rewritten or moved.

| Point | Where | What |
|---|---|---|
| snapshot | directly under `const bool bypass = ...` | nine relaxed loads: seven soundcheck atomics + `currentSampleRate_` (I-9) + the test gain seam (B-5), then the invariant-3 bounds check producing `scOut` / `scInCh` |
| 1 (capture hoist) | immediately after the bounds check, **outside** the lane loop (B-6) | `const float* capSource = (scInCh >= 0) ? inputChannelData[scInCh] : nullptr;` |
| 2 (mute) | inside the lane loop, after the existing index check, **before** `lanes[numLanes++]` and `tapSource[slot][lane] = out` | `if (scOut >= 0 && outIdx == scOut) continue;` — per **output channel**, not per (slot, lane) |
| 3 (inject) | between the close of the `else` DSP block and `// Final output guard` | builds `SoundcheckSignal` on the stack, `out[n] += v * scGainUnclamped`, advances `scSampleIndex_`, and self-clears `scOutChannel_`/`scRampOutAtSample_` when the ramp-out reaches zero |
| 4 (tap suspension) | head of the tap loop | `if (scSuspendTaps) { capture } else <the original loop, untouched>` |
| restart drain | after the `tapBuffers_` drain in `audioDeviceAboutToStart` | `micCapture_.clear();` |

Header: seven atomics + `scGainUnclampedForTest_` + `kCaptureCapacity = 65536`
+ `micCapture_` + `micCaptureDrops_`, placed straight after
`numOutputChannels_`; eleven accessors plus the two test seams near
`getTapDropCount`. `dsp/SoundcheckSignal.h` is included from the **.cpp only**
(m-20).

---

## 2. RED

`cmake --build build --config Release` with only the tests added:

```
test_audioengine.cpp(1362,12): error C2039: 'setRunningForTest': is not a member of 'AudioEngine'
test_audioengine.cpp(1368,12): error C2039: 'setSoundcheckPeak': is not a member of 'AudioEngine'
test_audioengine.cpp(1369,12): error C2039: 'setSoundcheckSampleIndex': is not a member of 'AudioEngine'
test_audioengine.cpp(1370,12): error C2039: 'setSoundcheckTapsSuspended': is not a member of 'AudioEngine'
test_audioengine.cpp(1371,12): error C2039: 'setSoundcheckOutputChannel': is not a member of 'AudioEngine'
test_audioengine.cpp(1403,12): error C2039: 'setSoundcheckGainUnclampedForTest': is not a member of 'AudioEngine'
test_audioengine.cpp(1470,12): error C2039: 'requestSoundcheckRampOut': is not a member of 'AudioEngine'
test_audioengine.cpp(1477,5):  error C2039: 'getSoundcheckOutputChannel': is not a member of 'AudioEngine'
test_audioengine.cpp(1479,5):  error C2039: 'soundcheckIsEmitting': is not a member of 'AudioEngine'
```
(truncated — the same nine names repeat once per test.)

## 3. GREEN

```
$ cmake -B build -G "Visual Studio 18 2026" -A x64
-- ASIO SDK found at .../external/asiosdk
-- Build files have been written to: .../build
$ cmake --build build --config Release     # 0 errors; one pre-existing C4996 in MainComponent.cpp
$ cd build && ctest -C Release -R "AudioEngine" --output-on-failure
38/48 Test #140: AudioEngineSoundcheck.SweptChannelCarriesOnlyTheSweep ........   Passed  0.08 sec
39/48 Test #141: AudioEngineSoundcheck.OutputClampStillCoversTheSweepPath .....   Passed  0.09 sec
40/48 Test #142: AudioEngineSoundcheck.SweepTouchesOnlyTheMeasuredChannel .....   Passed  0.09 sec
41/48 Test #143: AudioEngineSoundcheck.AbortRampsDownInTheCallbackAlone .......   Passed  0.09 sec
42/48 Test #144: AudioEngineSoundcheck.TapsStaySuspendedAcrossTheGap ..........   Passed  0.09 sec
43/48 Test #145: AudioEngineSoundcheck.NoiseFloorCapturesWithoutEmitting ......   Passed  0.08 sec
44/48 Test #146: AudioEngineSoundcheck.OutOfRangeChannelIsIgnored .............   Passed  0.10 sec
45/48 Test #147: AudioEngineSoundcheck.AtomicsAreSnapshottedOnce ..............   Passed  0.10 sec
46/48 Test #148: AudioEngineSoundcheck.IdleEngineEmitsNoSweepAndMutesNoLane ...   Passed  0.09 sec
47/48 Test #149: AudioEngineSoundcheck.DeviceRestartDrainsTheCaptureRing ......   Passed  0.09 sec

100% tests passed, 0 tests failed out of 48
```

Full gate:

```
$ cd build && ctest -C Release
100% tests passed, 0 tests failed out of 595
Total Test time (real) =  46.54 sec
```

585 before + 10 new = **595**. (The brief's Step 11 estimated 587 from a stale
577 baseline; the real baseline at `40818df` was 585.)

---

## 4. Mutation results — and two tests that did not work as written

### 4a. `OutputClampStillCoversTheSweepPath` — **the brief's version proved nothing**

Mutation: delete the output clamp entirely
(`out[n] = std::isfinite(v) ? jlimit(...) : 0.0f;` → `out[n] = v;`).

**First run: PASSED.** The test could not see the clamp being removed.

Cause, measured: the brief sets `setSoundcheckSampleIndex (0)`, which starts the
block inside the sweep's own 30 ms raised-cosine **ramp-in**. Over the first 512
samples the window holds the signal to `0.01557`, so the ×50 seam reaches only
**0.778** — above the test's `> 0.5` "sawSomething" bar, but **never above full
scale**. Nothing was ever clamped, so deleting the clamp changed nothing. This is
exactly the failure the brief itself documents for plan rev 1's peak seam
("the test asserted nothing at all"), reproduced one level up: the seam was fixed,
the *operating point* was not.

Fix (two changes, both strictly strengthening — nothing removed):

1. `setSoundcheckSampleIndex (1440)`. `rampSamples = 30 ms × 48 kHz / 1000 =
   1440`, and from there `w == 1`, so the sweep reaches `0.0999999` and ×50 is
   **5.0** — five times full scale. The arithmetic is in the comment beside the
   number (lane G B-4).
2. A second assertion, `sawTheClampEngage`: some sample must reach the ±1.0
   bound. Without it the test still cannot distinguish "the signal happened to
   stay under full scale" from "the clamp caught it", and only the second proves
   invariant 4.

**After the fix: real code PASSES, mutant FAILS** —
`AudioEngineSoundcheck.OutputClampStillCoversTheSweepPath (Failed)`.

### 4b. `AtomicsAreSnapshottedOnce` — **caught the mutant ~2 % of the time**

Two mutations tried, both legitimate "re-read an atomic mid-block":

- `reread`: the mute decision loads `scOutChannel_` fresh instead of using `scOut`.
- `reread3`: insertion point 3 loads `scOutChannel_` fresh instead of using `scOut`.

At the brief's **200** callbacks both mutants passed the test on the first run.
`--gtest_repeat=60` against `reread` produced exactly **one** failure, at callback
192. A guard that fires 2 % of the time is not a guard.

The reason is structural, and worth recording: `tapped && emitted` can only
happen when the **mute** decision and the **injection** decision disagree — a
muted lane never sets `tapSource`, so an "emitting" callback cannot tap no matter
what the suspend flag says. The first disagreement typically lands near callback
~200, i.e. once the flipper thread has actually been scheduled, so the old bound
sat exactly on the edge of ever working.

Fix: **20000** callbacks instead of 200, with the measurement written into the
comment. Measured over separate processes:

| | detection |
|---|---|
| mutant `reread3` (re-read at injection) | **10 / 10** runs failed |
| mutant `reread` (re-read at the mute decision) | **9 / 10** runs failed |
| real code | **0 / 15** runs failed |

The test still runs in **0.10 s** — 20000 callbacks of 64 samples costs nothing.

### 4c. `TapsStaySuspendedAcrossTheGap` — red as written

Mutation: `if (scSuspendTaps)` → `if (scOut >= 0)`.

**FAILED, as required**, with the test's own message:
`a tap was written during the Gap`.

After every mutation the pristine source was restored from a byte copy and the
suite re-run; the 595/595 above is the restored tree.

---

## 5. Invariant-16 self-audit of the callback diff

Every mention of shared state inside `audioDeviceIOCallbackWithContext`, code
lines only:

- **Atomic loads: nine, all in the snapshot block, none below it.**
  `scOutChannel_`, `scSuspendTaps_`, `scCaptureInChannel_`, `scCaptureActive_`,
  `scSampleIndex_`, `scPeak_`, `scRampOutAtSample_`, `currentSampleRate_`,
  `scGainUnclampedForTest_` — one `.load` each, consecutive lines, relaxed.
  A grep for every one of those names below the snapshot returns only comments.
- **Atomic stores: three, all inside point 3's `if (scOut >= 0)`** —
  `scSampleIndex_` (advance), and `scOutChannel_` / `scRampOutAtSample_` set to
  −1 when the ramp-out has run out. All three are computed from the snapshot
  locals, never from a fresh read.
- **`micCapture_.write()`**: one bulk lock-free SPSC write per callback.
  **`micCaptureDrops_.fetch_add`**: one relaxed RMW, only on a short write.
- **No `new`, no `malloc`, no growing container, no `juce::String`, no
  `Logger`/`DBG`, no `lock`** anywhere in the added lines.
- **`SoundcheckSignal` on the stack**: its members are four `double`, two
  `std::int64_t` and one `float` (`src/dsp/SoundcheckSignal.h`) — nothing that
  allocates. Its constructor is `noexcept` and does one `std::log`; `sampleAt`
  does one `exp`, one `cos`, one `sin`.
- **No unchecked index reaches an array.** `scOutChannel` and `scCaptureIn` (the
  raw snapshot values) appear on exactly four lines: their two loads and the two
  lines of the bounds check. Everything below uses `scOut` / `scInCh`, which are
  −1 unless the index is in range for **this** callback's counts *and* the
  matching channel-pointer array is non-null.
- **Idle path.** With `scOut == -1`, `scSuspendTaps == false`,
  `scCaptureActive == false`: the mute condition short-circuits on `scOut >= 0`
  and never runs; point 3 is skipped entirely; the tap loop runs through the
  `else` as **the original loop, character for character** (the diff is
  `226 insertions, 0 deletions` in `AudioEngine.cpp` — nothing in the tap loop
  was rewritten). The only cost is nine relaxed loads and two comparisons.
  `IdleEngineEmitsNoSweepAndMutesNoLane` plus the 37 pre-existing `AudioEngine`
  tests pin this.

**Conclusion: no lock, no allocation, no logging, and nothing touched outside the
seven soundcheck atomics + `currentSampleRate_` + the test gain seam +
`micCaptureDrops_` + `micCapture_`.**

---

## 6. Concerns for the reviewer

1. **The two test weaknesses in §4a and §4b are the finding of this task.** Both
   tests were green against broken code as the brief wrote them. Any future task
   that copies a "RED IF" test out of a brief should mutate it before trusting
   it — a green test is not evidence that it can go red.
2. **Nothing drives the engine yet.** `SoundcheckController` is Task 6. Per the
   brief: **do not release from this commit.** `installer\release-alpha.ps1` must
   not be run on this state.
3. **`else` without braces** in front of the tap loop is the brief's shape, kept
   deliberately so the existing loop keeps zero deleted lines. If a reviewer
   prefers braces, that is a pure-reindent follow-up, not a behaviour change.
4. **`setSoundcheckGainUnclampedForTest` ships in the release build** (like lane
   G's `setRingRiskOverrideForTest`). Production default is `1.0f` and nothing
   outside the test calls it, but it is a real multiplier on the signal path and
   Task 6 must never touch it.
5. **`micCaptureDrops_` has no consumer yet.** Spec §4.3 makes a non-zero count a
   self-abort condition; wiring it belongs to Task 6.

---
---

# Fix report — review round 1/5

All five defects fixed, plus M-1, M-2, M-3, M-5. Suite **600/600**
(595 + 4 new tests + 1 from the `I-1` split). Commit: **`28dbd19`**.

## 7. Changes

### C-1 — the soundcheck atomics survive a device stop
`audioDeviceAboutToStart`, beside `micCapture_.clear()`: six relaxed stores put
the soundcheck back to idle — `scOutChannel_ = -1`, `scRampOutAtSample_ = -1`,
`scRampOutRequested_ = false`, `scCaptureActive_ = false`,
`scSuspendTaps_ = false`, `scCaptureInChannel_ = -1`. `scSampleIndex_` and
`scPeak_` are deliberately left alone (neither can produce a sample without
`scOutChannel_`, and the controller sets both at Arm) — stated in the comment so
the omission reads as a decision, not an oversight. Same precondition as every
other clear in that function: JUCE inserts the callback only after it returns.

### C-2 + I-2 — one mechanism: the request is a flag, the anchor is latched by the callback
- New `std::atomic<bool> scRampOutRequested_ { false }` — the **eighth**
  soundcheck atomic, the **tenth** load in the snapshot.
- `requestSoundcheckRampOut()` now sets **only the flag**.
- Point 3 computes `std::int64_t rampAt = scRampOutAt;` and, if `rampAt < 0 &&
  scRampOutReq`, latches `rampAt = scSampleIndex` and stores it. The envelope is
  then evaluated at `rampOut(anchor, anchor, R)` on the first ramped sample =
  **exactly 1.0**, at any buffer size. The whole block, the ramp application and
  the self-clear all use the local `rampAt`, never the raw snapshot value.
- The self-clear also clears `scRampOutRequested_`.
- `setSoundcheckOutputChannel()` clears `scRampOutAtSample_` **and**
  `scRampOutRequested_` on **any** value, before publishing the channel, so a
  callback that sees the new channel cannot still see the old anchor. That makes
  `setSoundcheckOutputChannel(-1)` the controller's one-call abort backstop.

### C-3 — a muted lane's filter state is reset every block
The mute is still a `continue`, but now preceded by
`notchChains_[slot][lane].reset()` — the same allocation-free call the
per-sample NaN heal already makes on this thread. The comment carries the
reviewer's level statement verbatim:

> EXPECTED LEVEL CHANGE: mute onset = declared silence (spec §3); un-mute =
> programme resumes with the notch chains at zero state, no free response from
> stale state; an in-flight lane-G depth ramp on a muted lane is stranded and
> self-heals on the next Set (same as the NaN heal).

`NotchChain::reset()` clears filter state only — the stored notch designs and
coefficients survive, so the lane resumes notching at the same frequencies.

### I-1, M-1, M-2, M-3, M-5
- **I-1**: `TapsStaySuspendedAcrossTheGap` split in two. Both now route a second
  slot to an **unmeasured** output — the only lane that can actually tap during
  a run, since the measured lane is muted and has no `tapSource` either way.
  `TapsAreSuspendedDuringARun` (the spec's name) asserts the in-run phase and
  carries a **control**: with the suspension lifted the same lane does tap, so a
  broken route cannot make the test pass for the wrong reason.
- **M-1**: `AtomicsAreSnapshottedOnce` rewinds `setSoundcheckSampleIndex(0)`
  every 1000 iterations. Without it the index passes `totalSamples_` = 144000
  after ~2250 emitting callbacks, `sampleAt()` returns 0 for the rest of the run
  and `emitted` is never true again — the assertion was vacuous for most of its
  20000 iterations. Detection of the re-read mutant went from 9/10 to **10/10**.
- **M-2**: `MultiDriver` gained `declaredChannels`. `OutOfRangeChannelIsIgnored`
  now allocates **three** channels and tells the engine about **two**: channel 2
  is a real, readable guard buffer, so an unchecked write lands somewhere the
  test asserts on instead of in undefined behaviour. It also asserts that the
  sweep clock does not advance.
- **M-3**: `scCaptureActive_` comment (member and setter) now states that
  capture happens only while it is true **and** the taps are suspended.
- **M-5**: `currentSampleRate_` is read with `memory_order_acquire`, matching
  the release store in `audioDeviceAboutToStart` and every other reader, with a
  comment saying why the symmetry is worth the nothing it costs on x86.

**Deferred, as instructed:** M-4 (braces around the `else` before the tap loop)
and M-6 (seam validation on `setSoundcheckGainUnclampedForTest`).

## 8. Updated atomic table

| # | Atomic | Written by | Cleared by | Snapshot load |
|---|---|---|---|---|
| 1 | `scOutChannel_` | message thread; callback on ramp end | restart, callback self-clear | relaxed |
| 2 | `scSuspendTaps_` | message thread | restart | relaxed |
| 3 | `scCaptureInChannel_` | message thread | restart | relaxed |
| 4 | `scCaptureActive_` | message thread | restart | relaxed |
| 5 | `scSampleIndex_` | message thread at Arm; **callback owns it** in flight | — | relaxed |
| 6 | `scPeak_` | message thread (clamped) | — | relaxed |
| 7 | `scRampOutAtSample_` | **callback only** (latched) | restart, `setSoundcheckOutputChannel`, self-clear | relaxed |
| 8 | `scRampOutRequested_` | message thread | restart, `setSoundcheckOutputChannel`, self-clear | relaxed |
| — | `currentSampleRate_` | device thread (release) | — | **acquire** |
| — | `scGainUnclampedForTest_` | tests only | — | relaxed |

**Ten loads, all in the snapshot block; nothing below it reads any of them
again.** Stores inside the callback: `scRampOutAtSample_` (the latch),
`scSampleIndex_` (the advance), and `scOutChannel_` / `scRampOutAtSample_` /
`scRampOutRequested_` (the self-clear) — all inside `if (scOut >= 0)`, all
computed from snapshot locals.

**Invariant 16, re-read after the fixes.** One new touch:
`notchChains_[slot][lane].reset()` on a muted lane. `notchChains_` is not new
state for this thread — the callback already processes it every block and
already calls `reset()` on it from the per-sample NaN heal — and `reset()` only
zeroes pre-allocated biquad state: no lock, no allocation, no logging. Nothing
else was added. The idle path is unchanged: `scOut == -1` short-circuits the
mute branch before the reset, skips point 3 entirely, and runs the original tap
loop through the `else`.

## 9. Commands and output

```
$ cmake -B build -G "Visual Studio 18 2026" -A x64        # AudioEngine.h changed
-- Build files have been written to: .../build
$ cmake --build build --config Release                     # 0 errors
$ cd build && ctest -C Release -R "AudioEngine" --output-on-failure
42/53 Test #144: AudioEngineSoundcheck.TapsAreSuspendedDuringARun .............   Passed  0.08 sec
43/53 Test #145: AudioEngineSoundcheck.TapsStaySuspendedAcrossTheGap ..........   Passed  0.09 sec
45/53 Test #147: AudioEngineSoundcheck.OutOfRangeChannelIsIgnored .............   Passed  0.09 sec
46/53 Test #148: AudioEngineSoundcheck.AtomicsAreSnapshottedOnce ..............   Passed  0.11 sec
49/53 Test #151: AudioEngineSoundcheck.DeviceRestartClearsTheSoundcheckAtomics    Passed  0.09 sec
50/53 Test #152: AudioEngineSoundcheck.RampOutStartsAtFullScaleAtAnyBufferSize    Passed  0.09 sec
51/53 Test #153: AudioEngineSoundcheck.StaleRampAnchorDoesNotTruncateTheNextRun   Passed  0.09 sec
52/53 Test #154: AudioEngineSoundcheck.UnmuteResumesFromZeroFilterState .......   Passed  0.09 sec

100% tests passed, 0 tests failed out of 53
```
```
$ cd build && ctest -C Release
100% tests passed, 0 tests failed out of 600
Total Test time (real) =  45.40 sec
```

## 10. Mutation results

Every fix was mutated back out and the covering test watched fail. Each mutant
was applied to a byte copy of the fixed file, built, run, and the pristine file
restored; the 600/600 above is the restored tree, and `grep -c MUTANT` on the
committed source is 0.

| Mutant | Covering test | Result |
|---|---|---|
| the six restart clears deleted | `DeviceRestartClearsTheSoundcheckAtomics` | **FAILED** (0% of 1) |
| `rampAt = scSampleIndex - numSamples` (the one-block-stale anchor the fix exists to prevent) | `RampOutStartsAtFullScaleAtAnyBufferSize` | **FAILED** (0% of 1) |
| `setSoundcheckOutputChannel` no longer clears the ramp state | `StaleRampAnchorDoesNotTruncateTheNextRun` | **FAILED** (0% of 1) |
| the muted-lane `chain.reset()` deleted | `UnmuteResumesFromZeroFilterState` | **FAILED** (0% of 1) |
| upper bound dropped from the `scOut` check | `OutOfRangeChannelIsIgnored` | **FAILED** (0% of 1) — caught by the guard buffer, deterministically, not by a crash |
| `if (scSuspendTaps)` → `if (false)` (no suspension at all) | `TapsAreSuspendedDuringARun` **and** `TapsStaySuspendedAcrossTheGap` | **both FAILED** |
| `if (scSuspendTaps)` → `if (scOut >= 0)` (N3, keyed on the channel) | `TapsStaySuspendedAcrossTheGap` | **FAILED**; `TapsAreSuspendedDuringARun` passes, correctly — the channel IS armed during a run, so this mutant only misbehaves in the Gap |
| re-read of `scOutChannel_` at the mute decision | `AtomicsAreSnapshottedOnce` | **FAILED 10/10** separate process runs (was 9/10 before M-1; the rewind is what closed the gap) |
| output clamp deleted | `OutputClampStillCoversTheSweepPath` | **FAILED** (0% of 1) |

Real code, same binary, 15 consecutive runs of `AtomicsAreSnapshottedOnce`:
**0 failures**.

## 11. Concerns after round 1

1. **The `no_suspend` / `suspendkey` pair is worth keeping in mind.** The two tap
   tests kill different mutants and neither is redundant: `DuringARun` is the
   only one that asserts the in-run phase on a lane that can actually tap, and
   `AcrossTheGap` is the only one that catches suspension keyed on the channel.
2. **`setSoundcheckOutputChannel` now has a side effect** (it discards a pending
   ramp-out). That is deliberate and documented at the declaration, but Task 6
   must not use it as a plain "which channel" setter mid-ramp — during a
   ramp-out the controller must leave the channel alone and let the callback
   release it.
3. **C-3's reset strands an in-flight lane-G depth ramp** on a muted lane, the
   same way the NaN heal does. It self-heals on the next `Set`. Called out in
   the code comment; if lane G ever gains a ramp that must survive a mute, this
   is the line to revisit.
4. **`scSampleIndex_` is not cleared on device restart.** Safe today because
   emission requires `scOutChannel_ >= 0` and the controller sets the index at
   Arm; if Task 6 ever reads the index to decide anything before arming, that
   assumption needs re-checking.
5. **M-4 and M-6 deferred**, as instructed.
6. Still: **nothing drives the engine yet — do not release from this commit.**

---
---

# Fix report — review round 2/5

Both Important items fixed. Suite **603/603**; `-R "AudioEngine|Biquad|NotchChain"`
**104/104**.

## 12. Changes

### N-1 — a state-only entry point, and the mute uses it
`reset()` also zeroes `rampRemaining_`, so calling it on every block of a mute
that lasts up to 4.5 s cancelled any in-flight lane-G depth ramp on that lane
over and over — and command draining is **not** suspended during a run, so a
retune landing mid-mute is the normal case, not a corner. The filter stranded at
the intermediate depth while `NotchInfo.depthDB` already read the target: the
lane-G GAP the NaN heal documents as rare, made routine.

- **`Biquad::clearState()`** — `z1_ = z2_ = 0.0`; coefficients and
  `rampRemaining_ ` untouched. The header states the distinction: `reset()` says
  "the past is gone" and therefore cancels a ramp designed against that past;
  `clearState()` says only "this filter is about to be handed a discontinuity in
  its INPUT", which is exactly what a mute is.
- **`NotchChain::clearState()`** — loops it over all 16 filters.
- **`NotchChain::getFilterForTest (int index)`** — TEST ACCESSOR ONLY, clamping,
  added because no black-box measurement can tell a preserved ramp from a
  cancelled one (the same reason `Biquad`'s own `...ForTest` accessors exist).
- The mute branch calls `clearState()`. `reset()` is untouched and still what
  the NaN heal and the device-restart path use.
- The EXPECTED LEVEL CHANGE comment now ends: *"NO depth ramp is stranded — an
  in-flight lane-G ramp is frozen for the duration of the mute (the lane is not
  processed at all) and completes normally once the lane is un-muted."*

### N-2 — the ordering the comment claimed is now the ordering the model gives
Three relaxed stores to three distinct atomics carry no ordering between them;
"cleared before the channel is published" was a statement about source order
only. Now:

- `setSoundcheckOutputChannel`: the two clears stay relaxed,
  `scOutChannel_.store (channel, std::memory_order_release)`.
- The callback's snapshot load is `std::memory_order_acquire`.
- `audioDeviceAboutToStart`'s clear block: the five other stores stay relaxed
  and the **channel store moved to last**, under release.
- Both comments reworded to say what the model guarantees rather than what the
  source order looks like.

### Header comment on the atomics
Added, with the mechanism and the two tests that pin it:

> An abort request raised while `scSampleIndex_ < 0` (NoiseFloor) is honoured
> only when the index reaches 0 — the controller's backstop is
> `setSoundcheckOutputChannel(-1)`.

**I initially believed this was wrong and wrote a test to prove it; the test
failed and the reviewer was right.** The mechanism, now in the comment: the
anchor latched during the noise floor is itself negative, and `-1` is also the
"no anchor" sentinel, so `rampAt >= 0` is false — the envelope is neither
applied nor completed, and the request is simply re-latched each block until the
index turns non-negative. Measured: from an index of −23744 with 256-sample
blocks, the run ended after **99 blocks** (43 blocks of remaining noise floor,
then one 1440-sample ramp), not after 6. Nothing is emitted in the meantime, but
the channel stays muted and the taps stay suspended for the rest of the noise
floor.

## 13. Commands and output

```
$ cmake -B build -G "Visual Studio 18 2026" -A x64     # Biquad.h/NotchChain.h/AudioEngine.h changed
$ cmake --build build --config Release                 # 0 errors
$ cd build && ctest -C Release -R "AudioEngine|Biquad|NotchChain"
100% tests passed, 0 tests failed out of 104
$ cd build && ctest -C Release
100% tests passed, 0 tests failed out of 603
Total Test time (real) =  46.09 sec
```

600 + 3 new = 603: `MuteDoesNotCancelAnInFlightDepthRamp`,
`AbortDuringTheNoiseFloorIsDeferredUntilTheSweepStarts`,
`SettingTheChannelToMinusOneIsTheAbortBackstop`.

## 14. Mutation results

| Mutant | `MuteDoesNotCancelAnInFlightDepthRamp` | `UnmuteResumesFromZeroFilterState` |
|---|---|---|
| `clearState()` → `reset()` | **FAILED** | passed (reset clears state too) |
| the call removed entirely | passed (the ramp is untouched) | **FAILED** |

Neither test alone pins `clearState()`; together they pin it exactly — one
proves the state IS cleared, the other proves the ramp is NOT. That is the
discrimination N-1 asked for.

**N-2 cannot be mutation-tested, and I am not going to claim otherwise.** x86-64
is TSO: `memory_order_release`/`acquire` on these stores compile to the same
plain `mov` instructions as relaxed, so no test on this machine can distinguish
the fixed code from the unfixed code. The change is a correctness-of-the-model
fix, verified by reading: `grep` confirms the release store at
`AudioEngine.cpp:362` (setter) and `:1061` (restart, last in its block) and the
acquire load at `:658`. It would matter on ARM, which the repo's CI does build
for macOS.

## 15. Concerns after round 2

1. **The negative-anchor conflation is real and still there**, now documented
   and contained by the backstop rather than fixed: `scRampOutAtSample_` uses
   `-1` as "no anchor" while a legitimate noise-floor anchor is also negative.
   Fixing it properly means either a separate "anchor valid" bit or moving the
   sentinel to `INT64_MIN`, and it changes what an abort does mid-run — a
   decision that belongs with Task 6's abort path, not smuggled into a review
   round. Worst case today: a cancelled run holds a channel muted and the taps
   suspended for up to ~0.53 s longer than intended, emitting nothing.
   **Recommend this as a round-3 item or an explicit owner decision.**
2. **`NotchChain::getFilterForTest` is new public API on a DSP class.** Const,
   clamping, test-only by name and comment — but it does hand out a reference to
   a `Biquad` that a caller could copy. Nothing in production calls it.
3. **`Biquad::clearState()` is now reachable from anywhere `reset()` was.** The
   two are easy to confuse at a call site; the header comment on each says which
   statement it makes. Any future caller in the audio path should be reviewed for
   which of the two it actually means.
4. M-4 (braces) and M-6 (seam validation) still deferred.
5. Still: **nothing drives the engine yet — do not release from these commits.**
