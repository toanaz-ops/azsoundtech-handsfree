# Task 2 report — `NotchChain::setNotch` routes a depth-only change to the ramp

**Commit:** `403dd47` `feat(dsp): NotchChain routes a depth-only Set to Biquad::rampNotchDepth`
**Branch:** `claude_desk/lane-g-brainstorm-sdd-f3c568`
**Status:** DONE_WITH_CONCERNS (the concerns are documentation-level, not code — see §7)

---

## 1. What was implemented

### `src/dsp/NotchChain.h`

- New public constant, right after `MAX_NOTCHES`:
  ```cpp
  static constexpr double kRampMs = 10.0;
  ```
  with the brief's rationale comment verbatim.
- The `setNotch` doc comment gained the DEPTH-ONLY RETUNE paragraph and the
  paragraph explaining why the freq/Q comparison is a bare `==` on doubles
  (the controller resends the stored values, so they are bit-identical by
  construction; a near-miss falls through to the reset path, which is the safe
  direction to fail in). Brief text, verbatim.

### `src/dsp/NotchChain.cpp`

- `#include <cmath>` added (for `std::lround`).
- `setNotch` gained a branch BEFORE the existing `setNotchFilter` path:

  ```cpp
  if (notchInfo_[index].state == NotchState::Active
      && freq == notchInfo_[index].frequency
      && Q    == notchInfo_[index].Q)
  {
      const int rampSamples = static_cast<int>(std::lround(kRampMs * sampleRate_ / 1000.0));
      if (! filters_[index].rampNotchDepth(freq, Q, sampleRate_, depthDB, rampSamples))
      {
          return;
      }
      notchInfo_[index].depthDB = depthDB;
      return;
  }
  ```

  Deliberate properties:
  - The index bounds check still runs first, unchanged.
  - A rejected design (`rampNotchDepth` returns false — positive depth, above
    Nyquist, `Q <= 0`, `sampleRate <= 0`) returns without touching
    `notchInfo_`, exactly like the reset path below it. No half-application on
    either branch.
  - Only `depthDB` is written on the ramp branch: `frequency`, `Q` and `state`
    are already equal to what would be written, by the guard's own condition.
  - `rampSamples` is computed against `sampleRate_`, so the ramp is 10 ms of
    real time at any device rate (480 at 48 kHz, 441 at 44.1 kHz, 960 at 96 kHz).
  - `static_cast<int>` around `std::lround` instead of the brief's C-style
    `(int)` cast — the only syntactic deviation inside the brief's code block,
    chosen to match the cast style used elsewhere in this file and its tests.

- Everything else keeps today's behaviour bit for bit: an Idle slot, a new
  frequency, a new Q, and an out-of-range index all take the
  `setNotchFilter` + `reset()` path.

### Where this is reachable from

`AudioEngine::drain` calls `setNotch` for every `NotchCommand::Set`, so this is
the first Lane G code on the audio thread. The new branch adds one enum compare
and two double compares to a path that already existed; no allocation, no
logging, no lock.

---

## 2. TDD evidence

### RED — the six brief tests

Command:
```
cmake --build build --config Release
./build/tests/Release/HandsFreeTests.exe --gtest_filter='NotchChain.DepthOnlyRetuneOfARunningNotchKeepsTheFilterState:...'
```

Output (abridged):
```
[ RUN      ] NotchChain.DepthOnlyRetuneOfARunningNotchKeepsTheFilterState
tests\test_notchchain.cpp(476): error: Expected: (chain.processSample(0.0)) != (0.0), actual: 0 vs 0
state was cleared: this was a reset, not a ramp
[  FAILED  ] NotchChain.DepthOnlyRetuneOfARunningNotchKeepsTheFilterState (0 ms)
...
[  PASSED  ] 5 tests.
[  FAILED  ] 1 test
```

Why only one of the six failed, and why that is correct: five of the brief's
tests are *guards on the paths that must NOT change* (frequency change, Q
change, Idle slot, rejected depth, and the measured ladder, which today is
produced by the reset path). They are green before and after by design. The
sixth is the one that discriminates the new behaviour, and it went red for
exactly the stated reason — after a `setNotchFilter` + `reset()`, feeding 0.0
into a charged chain returns exactly 0.0.

### RED — the seventh test (added beyond the brief)

The six brief tests all stay green if `setNotch` passes `rampSamples = 0`:
`rampNotchDepth(..., 0)` installs the target in one step *while still
preserving the state*, so the state-preservation assertion, the ladder, and
every guard pass — and the click the ramp exists to prevent comes back
undetected. Nothing in the brief asserted that `kRampMs` is actually plumbed
through, so I added `ADepthOnlyRetuneMovesOverkRampMsRatherThanInOneStep`.

It is an exact probe, not a statistical one. Two chains are driven with
identical input to the same state (so their `z1` agree bit for bit); one is
then retuned -6 to -24 dB. `Biquad::processSample` steps the coefficients
BEFORE computing the output, and the transposed form makes that output
`b0*x + z1`, so the first sample after the command differs by exactly
`|db0| * x`, where `db0` is one ramp STEP if it ramps and the FULL coefficient
distance if it does not. Two standalone `Biquad`s supply that distance via
`coeffsForTest()`, so the test calibrates itself instead of hard-coding a
design. The probe sample sits at the tone's peak (index 3612 is a whole number
of quarter-cycles, `tone[3612] == 1.0`) rather than at a zero crossing, where
any coefficient difference would be multiplied by nothing.

Verified RED by sabotage — `rampSamples` temporarily forced to `0`, rebuilt,
run, then restored:
```
first step was 0.0065163553567605392, one kRampMs step is 1.3575740326584456e-05,
the whole distance is 0.0065163553567605392
[  FAILED  ] NotchChain.ADepthOnlyRetuneMovesOverkRampMsRatherThanInOneStep
```
The measured first step equalled the whole coefficient distance — a 480x
separation from the ramped value, so the assertion has an enormous margin.

An earlier draft of this test compared `firstDiff` against `0.05 * maxDiff`
over the ramp window; it **passed under the same sabotage** and was thrown
away. Recorded here because it is precisely the failure mode the self-review
was told to look for: a test that would still pass if the code silently took
the wrong path is not a test.

### GREEN

```
cmake --build build --config Release
./build/tests/Release/HandsFreeTests.exe --gtest_filter='NotchChain*'
```
```
[ RUN      ] NotchChain.DepthOnlyRetuneOfARunningNotchKeepsTheFilterState
[       OK ] NotchChain.DepthOnlyRetuneOfARunningNotchKeepsTheFilterState (0 ms)
[ RUN      ] NotchChain.ChangingFrequencyStillResetsTheFilterState
[       OK ] NotchChain.ChangingFrequencyStillResetsTheFilterState (0 ms)
[ RUN      ] NotchChain.ChangingQStillResetsTheFilterState
[       OK ] NotchChain.ChangingQStillResetsTheFilterState (0 ms)
[ RUN      ] NotchChain.SetNotchOnAnIdleSlotStillTakesTheResetPath
[       OK ] NotchChain.SetNotchOnAnIdleSlotStillTakesTheResetPath (0 ms)
[ RUN      ] NotchChain.MeasuredAttenuationMatchesEveryLadderRungWithinHalfADecibel
[ LADDER   ] requested -6 dB -> measured -6.000000 dB
[ LADDER   ] requested -12 dB -> measured -12.000000 dB
[ LADDER   ] requested -18 dB -> measured -18.000000 dB
[ LADDER   ] requested -24 dB -> measured -24.000000 dB
[       OK ] NotchChain.MeasuredAttenuationMatchesEveryLadderRungWithinHalfADecibel (3 ms)
[ RUN      ] NotchChain.RejectedDepthOnARunningNotchLeavesTheSlotUnchanged
[       OK ] NotchChain.RejectedDepthOnARunningNotchLeavesTheSlotUnchanged (0 ms)
[ RUN      ] NotchChain.ADepthOnlyRetuneMovesOverkRampMsRatherThanInOneStep
[       OK ] NotchChain.ADepthOnlyRetuneMovesOverkRampMsRatherThanInOneStep (0 ms)
[----------] 24 tests from NotchChain (7 ms total)
[  PASSED  ] 24 tests.
```

### Full gate

```
cd build && ctest -C Release
```
```
100% tests passed, 0 tests failed out of 470

Total Test time (real) =  41.35 sec
```
463 to 470: the 7 new tests, no regressions.

### Warnings

A full `cmake --build build --config Release` emits exactly one warning, which
is pre-existing and in an unrelated file:
`src/app/MainComponent.cpp(1288,53): warning C4996: 'juce::Displays::Display::userArea'`.
`NotchChain.cpp` and `test_notchchain.cpp` compile clean.

---

## 3. Measured attenuation per rung

Printed by `MeasuredAttenuationMatchesEveryLadderRungWithinHalfADecibel`
(1 kHz, Q 30, 48 kHz, measured as the RMS ratio over samples 24000-47999 of a
48000-sample window, i.e. after the 480-sample ramp plus a long settle):

| Requested | Measured | Error | Tolerance |
|---|---|---|---|
| -6 dB  | **-6.000000 dB**  | < 1e-6 dB | +-0.5 |
| -12 dB | **-12.000000 dB** | < 1e-6 dB | +-0.5 |
| -18 dB | **-18.000000 dB** | < 1e-6 dB | +-0.5 |
| -24 dB | **-24.000000 dB** | < 1e-6 dB | +-0.5 |

Each rung is reached by a *ramped* retune from the rung before it (the chain is
never re-designed between rungs), so this measures the ramp's landing point,
not `setNotchFilter`'s. The agreement is exact to the printed precision because
`Biquad::processSample` lands ON the target coefficients on the ramp's last
sample rather than on the accumulated sum of deltas.

**Expected level change in the shipped app: 0 dB broadband, and 0 dB at the
notch bin too.** `NotchController` does not yet send a second `Set` onto a live
index (that is Task 6), so no production call reaches the new branch. Once it
does, the depth at f0 moves by at most one ladder rung (6 dB) per command,
linearly in the coefficients, over 10 ms.

---

## 4. The NaN-heal decision (`NotchChain::reset()`)

**Decision: out of scope for this task. `reset()` is NOT changed; a comment
naming the gap was added instead.**

The gap is real. `AudioEngine.cpp:617` calls `chain.reset()` per sample as a
NaN self-heal. `NotchChain::reset()` resets every `Biquad`, and
`Biquad::reset()` cancels an in-flight ramp. Unlike the device-restart path,
this one is not followed by `setSampleRate()`'s coefficient replay, so the
filter is stranded at whatever intermediate depth the ramp had reached, while
`NotchInfo.depthDB` already reads the target.

Three reasons I did not fix it here:

1. **Scope.** The brief's file list names `src/dsp/NotchChain.cpp:1-53` — the
   include and `setNotch`. `reset()` is at line 87 and is outside that range.
   The parent's licence to touch `reset()` was conditional on the brief allowing
   it; the brief's line range does not.

2. **It would put trig and `pow` into the audio thread's worst path.** The
   proposed heal re-applies each Active slot's design after resetting.
   `designPeaking` evaluates `cos`, `sin` and `pow` per slot; with 16 slots that
   is up to 48 transcendental calls **per sample** during a NaN event, and a NaN
   event is a burst, not a single sample. Adding that cost to the one path that
   only runs when the DSP is already degraded is a new real-time hazard traded
   for an accuracy gap. That is the wrong direction for live-sound code.

3. **The stranded state is inaccurate, not unsafe.** By the convexity argument
   already written into `Biquad.h`, any interpolated coefficient set between two
   depth <= 0 designs is itself a valid peaking filter with `|H| <= 1` everywhere
   and pole radius < 1. A stranded ramp cannot boost and cannot diverge; the
   worst case is a notch sitting at, say, -8 dB while the GUI reads -12 dB. No
   clamp, NaN guard or bounds check was removed or weakened.

What I did instead — a comment in `reset()` naming the gap, so the next session
finds it at the call site rather than only in `Biquad.h`:

```cpp
void NotchChain::reset()
{
    // GAP (spec 4.7, m-5): this cancels every in-flight depth ramp and does
    // NOT re-apply the stored design, so a reset that is not followed by
    // setSampleRate()'s coefficient replay -- AudioEngine's per-sample NaN
    // self-heal is the one such caller -- strands the filter at the ramp's
    // intermediate depth while NotchInfo.depthDB already reads the target.
```

**Recommendation for whoever owns the heal:** do it in `AudioEngine`, not in
`NotchChain::reset()` — the heal should re-arm the affected chain ONCE after the
NaN burst subsides, not per sample. That is an `AudioEngine.cpp` change, which
this task was explicitly forbidden to make.

---

## 5. Files changed

| File | Change |
|---|---|
| `src/dsp/NotchChain.h` | `kRampMs` constant + `setNotch` doc (+21 lines) |
| `src/dsp/NotchChain.cpp` | `<cmath>`, ramp branch in `setNotch`, gap comment in `reset()` (+28 lines) |
| `tests/test_notchchain.cpp` | `<iomanip>` / `<iostream>` includes + 7 tests (+173 lines) |

222 insertions, 0 deletions. Nothing under `.superpowers/` staged;
`.superpowers/sdd/.gitignore` was removed before `git add`. `git add` used the
three explicit paths and `git diff --cached --name-only` confirmed only those
three.

---

## 6. Self-review

- **Completeness vs the brief:** all seven steps done. `kRampMs` = 10.0 exactly
  as specified; the `setNotch` doc and the ramp branch are the brief's text and
  logic. All six brief tests are present with their assertions verbatim.
- **Deviations, all deliberate and listed:**
  1. `static_cast<int>` instead of `(int)` for the `lround` result.
  2. Added `#include <iomanip>` / `<iostream>` to the test file and a
     `std::cout` line inside the ladder test, so the four measured numbers are
     printed rather than only asserted (the parent asked for them by value). No
     assertion changed.
  3. Added a seventh test (§2) that the brief did not have.
  4. Added the gap comment in `reset()` (§4).
  5. Commit message follows the parent task's wording
     (`feat(dsp): NotchChain routes a depth-only Set to Biquad::rampNotchDepth`)
     rather than the brief's variant, per the task instruction, and carries the
     required `Co-Authored-By` trailer.
- **The "not a test" check:** `DepthOnlyRetuneOfARunningNotchKeepsTheFilterState`
  feeds a real sine, charges the state, and was demonstrated RED against the
  reset path. `ADepthOnlyRetuneMovesOverkRampMsRatherThanInOneStep` uses
  `Biquad::coeffsForTest()` and was demonstrated RED against a sabotaged
  `rampSamples = 0`. Both discriminate; the discarded first draft of the latter
  did not, and is documented above rather than quietly dropped.
- **YAGNI:** no accessor added to `NotchChain` (the seventh test gets the
  coefficient distance from standalone `Biquad`s instead), no new public API
  beyond the `kRampMs` the brief asked for, no change to `reset()`'s behaviour.
- **Comment style:** matches `NotchChain.h` — sentence-case prose, `--` for the
  dash, CAPS for the one word that carries the warning, spec section cited. No
  Unicode punctuation introduced; files written UTF-8 and normalised to the
  repo's CRLF working-copy convention (verified: the diffstat is 222 insertions
  / 0 deletions, i.e. no line-ending churn).
- **Safety:** no clamp, limiter, NaN guard or bounds check removed or relaxed.
  The index bounds check and the rejected-design early return are intact on both
  paths.

---

## 7. Concerns

1. **The `kRampMs` comment contains an arithmetic claim that is wrong.** The
   brief's verbatim text says 10 ms "is one block at a 2048 buffer and seven at
   64, so it is always at least one callback". At 48 kHz, 10 ms is 480 samples:
   that is **7.5 blocks at 64** (fine) but **less than a quarter of one block at
   2048** — so the ramp is NOT "always at least one callback"; at a 2048 buffer
   it starts and finishes inside a single callback. I left the text as the brief
   specified rather than silently diverging the header comment from spec 4.7,
   but **spec 4.7 and this comment should both be corrected** before Task 6
   reasons about callback boundaries. The `kRampMs` *value* is unaffected.

2. **`std::lround` on an unvalidated `sampleRate_`.** `NotchChain`'s constructor
   accepts any double (only `setSampleRate` guards `> 0`). An absurd constructor
   argument would overflow the `int` cast. Not reachable in production
   (`AudioEngine` builds chains from the device rate) and the pre-existing reset
   path has the same exposure, so I added no guard — and a negative or zero rate
   reaches `rampNotchDepth`, which rejects it, so the failure mode is a no-op
   rather than a bad filter.

3. **The exact-`==` contract is now load-bearing across a module boundary.**
   Task 6's `NotchController` must resend the *stored* freq and Q, not
   recomputed ones. A recomputation that differs in the last bit silently
   converts every retune into a reset — i.e. a click at every ladder step, with
   all tests still green. The header says so; a test on the controller side
   should enforce it when Task 6 lands.

4. **Nothing exercises the ramp branch at a non-48 kHz rate.** `rampSamples`
   scales with `sampleRate_`, and only 48 kHz is covered. A 44.1 / 96 kHz
   variant of the seventh test would be cheap if the verifier wants it.

5. **Not verified by ear.** Per CLAUDE.md's 2026-08-27 owner decision the listen
   happens in alpha, and this task ships no behaviour change to the app (§3), so
   there is nothing audible to check yet.

## Fix round 1

**Finding addressed (Important 1):** `src/dsp/NotchChain.h:35-38` — the
`kRampMs` rationale comment was arithmetically false ("10 ms is one block at a
2048 buffer ... so it is always at least one callback"). At 48 kHz, 10 ms is
480 samples against a 2048-sample buffer (42.7 ms) — the ramp is under a
quarter of one block there, the opposite of the claim. This is the same defect
already flagged in §7.1 above; this round fixes the comment text itself
(comment-only, no code or value change), which §7.1 had left unfixed per the
brief's verbatim instructions.

**Diff summary (`src/dsp/NotchChain.h`, lines 35-38 → 35-42, comment only):**

```diff
-    // Depth-only retune ramp (spec 4.7, decision Q5). 10 ms is one block at a
-    // 2048 buffer and seven at 64, so it is always at least one callback and
-    // never long enough to be heard as a slew. 6 dB over 10 ms is 0.6 dB/ms --
-    // the invariant the controller's ladder is written against.
+    // Depth-only retune ramp (spec 4.7, decision Q5). 10 ms is 441 samples at
+    // 44.1 kHz, 480 at 48 kHz, 960 at 96 kHz. The ramp advances per SAMPLE,
+    // not per callback, so both buffer sizes in this app are correct: at a
+    // buffer >= 512 samples (e.g. 2048) the ramp starts and finishes inside a
+    // single callback; at a 64-sample buffer it straddles roughly 7-8
+    // callbacks. 10 ms is the value it is because 6 dB over 10 ms is
+    // 0.6 dB/ms -- the invariant the controller's ladder is written against
+    // (spec 4.7).
     static constexpr double kRampMs = 10.0;
```

No code changed; `setNotch` behaviour, `kRampMs`'s value, and every other line
are untouched.

**Commands run and their output:**

```
cmake --build build --config Release
...
HandsFree.vcxproj -> ...\AZ Soundtech Hands-free.exe
HandsFreeSnapshot.vcxproj -> ...\HandsFreeSnapshot.exe
HandsFreeTests.vcxproj -> ...\HandsFreeTests.exe
PeakinessSweep.vcxproj -> ...\PeakinessSweep.exe
(build succeeded, no errors)
```

```
build/tests/Release/HandsFreeTests.exe --gtest_filter=NotchChain*
[==========] Running 24 tests from 1 test suite.
...
[  PASSED  ] 24 tests.
```

```
cd build && ctest -C Release
...
100% tests passed, 0 tests failed out of 470
Total Test time (real) =  44.62 sec
```

## Fix round 2

Round 1 (commit `80da819`) rewrote the `kRampMs` comment but two claims were
false at 96 kHz:

- "at a buffer >= 512 samples ... inside a single callback" — at 96 kHz a
  10 ms ramp is 960 samples; 960 / 512 = 1.875 callbacks, so a 512-sample
  buffer does NOT contain the ramp in one callback at 96 kHz.
- "at 64 samples ... roughly 7-8 callbacks" — at 96 kHz, 960 / 64 = 15
  callbacks, not 7-8.

### Arithmetic verified

- 44100 Hz x 0.010 s = 441 samples
- 48000 Hz x 0.010 s = 480 samples
- 96000 Hz x 0.010 s = 960 samples
- 960 / 1024 = 0.9375 < 1 -> at a 1024-sample buffer (or larger) the ramp
  fits inside a single callback at all three rates (441/1024 and 480/1024
  are smaller still).
- 441 / 64 = 6.89 -> rounds to "about 7" callbacks at 44.1 kHz.
- 480 / 64 = 7.5 -> also "about 7" callbacks at 48 kHz (matches the
  44.1 kHz number, so the comment states one shared figure for both rates).
- 960 / 64 = 15 -> "15" callbacks at 96 kHz.

### Change made

`src/dsp/NotchChain.h` lines ~35-42, comment only (no code token changed):

```cpp
    // kRampMs: a depth-only retune walks its coefficients over 10 ms --
    // 441 samples at 44.1 kHz, 480 at 48 kHz, 960 at 96 kHz. The ramp is
    // per-sample, so how it lands on callback boundaries does not matter:
    // at a 1024-sample buffer or larger it starts and finishes inside one
    // callback at every rate above; at a 64-sample buffer it straddles
    // about 7 callbacks at 44.1/48 kHz and 15 at 96 kHz. 10 ms is chosen so
    // a 6 dB rung moves at most 0.6 dB/ms (spec 4.7); nothing in NotchChain
    // enforces that bound -- the controller's ladder must not send a
    // multi-rung step in the deep direction.
```

### Commands and output

```
cmake --build build --config Release
...
HandsFree.vcxproj -> ...\AZ Soundtech Hands-free.exe
HandsFreeSnapshot.vcxproj -> ...\HandsFreeSnapshot.exe
HandsFreeTests.vcxproj -> ...\HandsFreeTests.exe
PeakinessSweep.vcxproj -> ...\PeakinessSweep.exe
(build succeeded, no errors)
```

```
build/tests/Release/HandsFreeTests.exe --gtest_filter=NotchChain*
[==========] Running 24 tests from 1 test suite.
...
[  PASSED  ] 24 tests.
```

```
cd build && ctest -C Release
...
100% tests passed, 0 tests failed out of 470
Total Test time (real) = 43.93 sec
```
