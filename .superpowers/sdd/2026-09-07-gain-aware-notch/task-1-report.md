# Task 1 report — `Biquad::rampNotchDepth`

**Status: DONE_WITH_CONCERNS**
**Commit: `f5bac4d` — feat(dsp): Biquad::rampNotchDepth -- retune depth without clearing filter state**

---

## 1. What I implemented

Exactly the brief, no deviations. The real files matched the brief's line references
closely enough that every anchor applied verbatim.

**`src/dsp/Biquad.h`**
- Appended the "Depth retune without a click" doc block (with the §4.7 convex-combination
  proof) immediately before `#pragma once`, verbatim from the brief.
- Public: `rampNotchDepth(freq, Q, sampleRate, depthDB, rampSamples)`, plus the three test
  accessors `stateForTest()` / `coeffsForTest()` / `rampRemainingForTest()` and the two
  POD structs `State` / `Coeffs`.
- Private: added the shared `static bool designPeaking(...)` declaration and the ramp state
  (`target_[5]`, `delta_[5]`, `rampRemaining_`).

**`src/dsp/Biquad.cpp`**
- Constructor now zeroes `target_` / `delta_` / `rampRemaining_`.
- New `Biquad::designPeaking` holds the four-argument peaking design and its five rejections
  in one place.
- The four-argument `setNotchFilter` now calls `designPeaking`, keeps its `reset()`, and adds
  `rampRemaining_ = 0` (a new design supersedes an in-flight ramp).
- New `rampNotchDepth`: rejects → untouched; `rampSamples <= 0` → install target immediately,
  **no `reset()`**; otherwise compute per-sample deltas from the coefficients *currently
  running* (so a restart mid-flight starts a fresh straight line, not a snap-back).
- `processSample` gained one `if (rampRemaining_ > 0)` branch: five additions per sample while
  a ramp is live, and it lands *on* `target_` on the last sample rather than on the
  accumulated sum of deltas. The existing Direct Form I transposed arithmetic below it is
  byte-for-byte unchanged.
- `reset()` now also clears `rampRemaining_`.

**`tests/test_biquad.cpp`**
- Two helpers (`magnitudeAt`, `noBoostProbeFrequencies`) added to the existing anonymous
  namespace next to `chargeThenFeedSilence`; `sineWave` and `rms` reused as instructed.
- Nine new tests, verbatim from the brief.
- One addition beyond the brief: `#include <algorithm>` — `std::max` is used by
  `RampNotchDepthDoesNotClick` and the file only included `<cmath>` and `<vector>`. Without it
  the build is not guaranteed (MSVC happened to pull it in transitively, but that is not a
  contract). This is the only line in the whole task that is not in the brief.

The three-argument pure-notch `setNotchFilter` was not touched; it inherits the ramp cancel
through its existing `reset()` call.

**Signal-path impact: 0 dB.** Nothing calls `rampNotchDepth`. The only change reachable from
the shipped path is one `int` comparison per sample in `processSample`, which is false for
every existing caller.

---

## 2. TDD evidence

### RED — tests written first, build fails on the missing API

```
cmake --build build --config Release
```

Distinct missing-member errors (deduplicated from the full log):

```
error C2039: 'Coeffs': is not a member of 'Biquad'
error C2039: 'State': is not a member of 'Biquad'
error C2039: 'coeffsForTest': is not a member of 'Biquad'
error C2039: 'rampNotchDepth': is not a member of 'Biquad'
error C2039: 'rampRemainingForTest': is not a member of 'Biquad'
error C2039: 'stateForTest': is not a member of 'Biquad'
```

Expected: the brief's Step 2 predicted exactly this — the nine tests reference an API that does
not exist yet, so the failure is a compile failure, not an assertion failure.

### GREEN — focused

```
cd build && ctest -C Release -R Biquad --output-on-failure
```

```
16/25 Test #45: Biquad.RampNotchDepthPreservesFilterStateExactly ....................   Passed    0.02 sec
17/25 Test #46: Biquad.RampNotchDepthDoesNotClick ...................................   Passed    0.02 sec
18/25 Test #47: Biquad.RampMidpointsNeverBoostAnyFrequency ..........................   Passed    0.02 sec
19/25 Test #48: Biquad.RampRestartedMidFlightStillNeverBoosts .......................   Passed    0.02 sec
20/25 Test #49: Biquad.RampReachesTheExactTargetCoefficients ........................   Passed    0.02 sec
21/25 Test #50: Biquad.RampDoesNotDivergeOverEveryLadderStepAndRate .................   Passed    0.02 sec
22/25 Test #51: Biquad.RampNotchDepthRejectionLeavesEverythingUntouched .............   Passed    0.02 sec
23/25 Test #52: Biquad.SetNotchFilterCancelsAnInFlightRampAndResetsState ............   Passed    0.02 sec
24/25 Test #53: Biquad.NonPositiveRampSamplesAppliesTheTargetAtOnceAndKeepsState ....   Passed    0.02 sec

100% tests passed, 0 tests failed out of 25
```

### GREEN — full suite

```
cd build && ctest -C Release
```

```
463/463 Test #463: logstats_fixture .....................................   Passed    0.14 sec

100% tests passed, 0 tests failed out of 463

Total Test time (real) =  34.17 sec
```

454 + 9 = 463, exactly as the brief predicted.

### Warnings

`touch src/dsp/Biquad.cpp tests/test_biquad.cpp && cmake --build build --config Release`
recompiles both files: **0 warnings in the whole build**, Biquad or otherwise.

---

## 3. Measured numbers

Measured with a temporary gtest harness appended to `tests/test_biquad.cpp`, run, then the file
restored byte-for-byte from a pre-harness copy (verified: `grep -c "TempMeasure\|cstdio" == 0`,
24 `TEST(Biquad` blocks, CRLF preserved). **The harness is not in the commit.**

### No-boost (the §4.7 invariant)

| Quantity | Value |
|---|---|
| Max mid-ramp gain over the whole `RampMidpointsNeverBoostAnyFrequency` grid | **0.0000 dB** |
| Probe count behind that figure | 94,500 (3 rates × 5 freqs × 3 Qs × 7 depth pairs × 5 ramp steps × ~60 probes) |

Not merely "under the 1 + 1e-9 tolerance" — the maximum `20·log10|H|` over all 94,500 probes is
exactly `0.0`. Tighter than the brief's own round-1 figure of 1.9e-15 dB, which used a different
probe set.

### Stability

| Quantity | Value |
|---|---|
| Max abs output over the divergence ladder (3 rates × 12 depth transitions × 8192 samples, unit-amplitude tone) | **0.981722** (bound: 1.0) |

### Attenuation

| Quantity | Value |
|---|---|
| Steady-state attenuation at 1 kHz before the ramp (design −6 dB) | **−5.997 dB** |
| Same, after ramping to −18 dB over 480 samples | **−17.997 dB** |

The ramp lands on the requested depth; the 3 mdB shortfall is the RMS window, not the filter.

### Click figures (0.5-amplitude 1 kHz sine @ 44.1 kHz; signal's own max per-sample step ≈ 0.0357)

| Case | max step in the 441-sample window |
|---|---|
| **The click this task removes** — `setNotchFilter(−12 dB)` mid-signal (calls `reset()`) | **0.248271** |
| `rampNotchDepth(−6 → −12 dB, 441)` | **0.035650** |
| `rampNotchDepth(−6 → −12 dB, 0)` — instant swap, state kept | 0.034703 |
| `rampNotchDepth(0 → −24 dB, 441)` (0.5-amp signal steps ≈ 0.0712) | 0.071093 |
| `rampNotchDepth(0 → −24 dB, 0)` — instant swap, state kept | 0.065099 |

**The headline: the reset click is 0.248 — 7× the signal's own maximum step, and 3.1× the test's
0.08 bound. The ramp reduces it to 0.0357, i.e. indistinguishable from the undisturbed
waveform (ratio 0.9993).** That is the entire justification for the task, quantified.

---

## 4. Self-review

### Completeness
All nine tests from the brief present and verbatim, plus both helpers in the right namespace.
All four brief sub-steps of the implementation applied.

### Quality
Names describe what, not how (`designPeaking`, `rampRemaining_`, `target_`, `delta_`). Header
comments follow the file's existing style — they explain *why* (the convex-combination proof,
the reason `reset()` is wrong here) rather than restating the code.

### Discipline
Only the three permitted files changed. Confirmed: `docs/superpowers/plans/…` and
`docs/superpowers/specs/…` show as modified in `git status` but were **already dirty before I
started** — I did not touch them and did not stage them. `git diff --cached --name-only` before
committing listed exactly the three files. `rm -f .superpowers/sdd/.gitignore` ran before the
commit; nothing under `.superpowers/` is staged or committed.

### Testing — mutation-tested, per the self-review instruction

**Mutation A — make the ramp secretly `reset()`** (added `reset()` to both success paths of
`rampNotchDepth`). Build clean, then:

```
88% tests passed, 3 tests failed out of 25
	 45 - Biquad.RampNotchDepthPreservesFilterStateExactly (Failed)
	 49 - Biquad.RampReachesTheExactTargetCoefficients (Failed)
	 53 - Biquad.NonPositiveRampSamplesAppliesTheTargetAtOnceAndKeepsState (Failed)
```

The two tests the brief names (#45, #53) **do** catch it, and #49 catches it as collateral
(`reset()` also clears `rampRemaining_`, so the ramp never runs). Confirmed, not assumed.

**Mutation B — replace the ramp with an instant coefficient swap** (`if (rampSamples <= 0)` →
`if (true)`):

```
96% tests passed, 1 tests failed out of 25
	 45 - Biquad.RampNotchDepthPreservesFilterStateExactly (Failed)
```

This is a finding — see Concern 1.

Source restored from a pristine copy after each mutation and verified with `diff` before the
commit.

---

## 5. Concerns

### Concern 1 — `RampNotchDepthDoesNotClick` does not test what its comment claims

The brief's comment says *"RED IF the ramp jumps coefficients in one step (a click) rather than
interpolating."* Empirically it does not. Mutation B replaced the ramp with an instantaneous
coefficient swap and the test still passed, on the mild depth pair it uses and on a harsher one:

- `−6 → −12 dB` instant: 0.0347 (bound 0.08, and `≤ 1.05 × before` = 0.0375) — passes both.
- `0 → −24 dB` instant: 0.0651 — still under 0.08.

**Why this is physically correct, not a bug in the implementation.** With the state preserved and
Direct Form I *transposed*, the output at the swap instant is `b0·x + z1`; `z1` carries the entire
history and only `b0` changes. For a 0 → −24 dB swap at Q = 30 / 1 kHz, `b0` moves 1.0 → 0.964, so
with |x| ≤ 0.5 the worst extra step is ~0.018. **A coefficient swap on a preserved state simply is
not a click.** The click comes from clearing `z1`/`z2`, and there the test bites hard: the
`setNotchFilter` reset path measures 0.248, comfortably red against the 0.08 bound.

**What this means for downstream tasks.** The test is a valid, sharp regression guard against the
one failure mode that actually clicks (an internal `reset()`). It is **not** evidence that the
ramp is necessary — nothing in this suite is. If Task 2 or the tester notes cite
`RampNotchDepthDoesNotClick` as "proof the ramp prevents the click", that claim is wrong; cite the
0.248-vs-0.0357 reset figure above instead. I left the test exactly as the brief wrote it (per the
"nothing beyond the brief" instruction) but its comment overstates its reach.

A secondary consequence: the ramp's real value at these depths is not the single-sample step but
avoiding modulation/zipper artefacts over the transition and keeping every intermediate design
inside the no-boost hull — which `RampMidpointsNeverBoostAnyFrequency` *does* prove, over 94,500
probes at exactly 0.0 dB.

### Concern 2 — the test accessors are unconditionally public

`stateForTest`/`coeffsForTest`/`rampRemainingForTest` are public in shipping builds, not behind a
friend declaration or a build flag. This is the brief's design (and correctly justified: no
black-box measurement can distinguish a preserved state from a cleared one), and they are all
`const` and side-effect-free, so the risk is only that production code may start reading them.
Flagging, not changing.

### Concern 3 — the `<algorithm>` include

The one line I added that the brief does not specify. `std::max` in the brief's own click test
requires it; MSVC compiled without it via transitive includes, which is luck, not a contract.
I judged an omitted-include fix to be within "adapt only where the real file differs". Called out
here so it is not mistaken for scope creep.

### Non-concerns, checked and clear

- Nothing under `.superpowers/` staged; `.superpowers/sdd/.gitignore` removed before the commit.
- No allocation in `processSample` — `target_`/`delta_` are fixed-size member arrays.
- No clamp, NaN guard, or bounds check removed. The five rejections moved into `designPeaking`
  unchanged and are now shared by both entry points, which is strictly harder to drift.
- CRLF line endings preserved in all three files (`file` reports "CRLF line terminators"; the
  repo is `core.autocrlf=true`, index LF).

---

## Fix round 1

Reviewer found two Important issues on commit `f5bac4d`. Both fixed; two optional minors folded
in.

### Important 1 — `tests/test_biquad.cpp:566-620` (`RampNotchDepthDoesNotClick`)

The reviewer's finding: the test's only ramp-shape assertions (the click-step bounds) cannot
distinguish "interpolating linearly" from "instant swap on a preserved state" or "wrong divisor
that walks short and snaps the remainder" — both pass the 0.08 step bound and the endpoint test
(`RampReachesTheExactTargetCoefficients`) only checks the final assign.

Fix: added a mid-ramp coefficient assertion inside the existing test (no new test, per the
brief). Two independently-designed `Biquad`s (`startDesign` = -6 dB, `targetDesign` = -12 dB, same
freq/Q/sampleRate) give ground-truth `b0` values outside the biquad under test. The loop captures
`f.coeffsForTest().b0` at `i == 10219`.

**Index correction from the brief's review comment.** The reviewer's draft said "sample i == 10220
(the 220th of 441 ramp samples...)" and explicitly flagged "check the test's actual indices." I
traced the code: `rampNotchDepth` is armed at `i == 10000` but does not touch coefficients itself;
the first delta is applied *inside* the `processSample` call made at that same `i == 10000`
(`rampRemaining_` goes 441 -> 440 and one delta is added). So after the call at sample
`i == 10000 + k - 1`, exactly `k` of 441 deltas have been applied. For `k == 220`,
`i == 10000 + 220 - 1 == 10219`, not 10220. Used `i == 10219` and documented the derivation in the
test's own comment so the next reader does not have to re-derive it.

Assertion:
```cpp
const double expectedMidB0 = startB0 + 220.0 * (targetB0 - startB0) / 441.0;
EXPECT_NEAR(midRampB0, expectedMidB0, 1e-12);
EXPECT_GT(midRampB0, std::min(startB0, targetB0));
EXPECT_LT(midRampB0, std::max(startB0, targetB0));
```

Also corrected the stale comment at the old `:562-565` ("RED IF the ramp jumps coefficients in one
step") to state what the test actually catches: a `reset()`-caused click (the step bound) and,
separately, a ramp that does not walk the exact straight line (the new mid-ramp assertion) — and
cross-referenced Concern 1 in this report, which is the source of the original comment's overreach.

**Mutation evidence** (build + focused run each time; `Biquad.cpp` restored from a pristine backup
and diffed byte-identical after each):

Mutation A — instant swap (`if (rampSamples <= 0)` -> `if (true)`):
```
D:\...\tests\test_biquad.cpp(606): error: The difference between midRampB0 and expectedMidB0 is
0.00093075936122699598, which exceeds 1e-12, where
midRampB0 evaluates to 0.99648078130831697,
expectedMidB0 evaluates to 0.99741154066954396, and
1e-12 evaluates to 9.9999999999999998e-13.
mid-ramp b0 is not on the straight line between the two designs

D:\...\tests\test_biquad.cpp(608): error: Expected: (midRampB0) > (std::min(startB0, targetB0)),
actual: 0.99648078130831697 vs 0.99648078130831697

[  FAILED  ] Biquad.RampNotchDepthDoesNotClick (0 ms)
```
Both the linearity assertion and the strictly-between bound go red (the instant swap lands exactly
on `targetB0`, which coincides with `min(startB0, targetB0)`).

Mutation B — wrong divisor (`/ static_cast<double>(rampSamples)` -> `/ static_cast<double>(rampSamples + 1)`):
```
D:\...\tests\test_biquad.cpp(606): error: The difference between midRampB0 and expectedMidB0 is
2.0962619488029688e-06, which exceeds 1e-12, where
midRampB0 evaluates to 0.99741363693149276,
expectedMidB0 evaluates to 0.99741154066954396, and
1e-12 evaluates to 9.9999999999999998e-13.
mid-ramp b0 is not on the straight line between the two designs

[  FAILED  ] Biquad.RampNotchDepthDoesNotClick (0 ms)
```
The linearity assertion alone catches it (the step-bound tests still pass, as the reviewer
predicted).

Restored to the fixed source and re-verified green (`diff` against the pristine post-fix copy
reported identical before rebuilding).

### Important 2 — `src/dsp/Biquad.h` doc block

The reviewer's finding: the doc block described `setNotchFilter`/`reset()` as the only ways a ramp
gets cancelled, and treated an accidental cancellation as a device-stop-only case. But
`AudioEngine.cpp:615-619` calls `chain.reset()` from inside the per-sample audio loop as a NaN
self-heal, and `NotchChain::reset()` (`NotchChain.cpp:65-71`) loops every filter, so one non-finite
sample cancels every in-flight ramp in the chain — not just the one that produced it. I read
`AudioEngine.cpp:595-624` and `NotchChain.cpp:65-116` directly to confirm this before writing the
doc addition, and confirmed the NaN-heal path is a bare `chain.reset()` with no accompanying
`setSampleRate()` call, whereas `NotchChain::setSampleRate()` (`NotchChain.cpp:103-116`) *does*
replay `notchInfo_[i].depthDB` through `setNotchFilter` for every Active slot — that replay is what
a device-restart reset gets and a NaN-heal reset does not.

Fix (doc-only, no behaviour change): appended a paragraph to the existing doc block citing
`AudioEngine.cpp:617` and `NotchChain.cpp:110`, stating the filter is left frozen at whatever
intermediate depth the ramp had reached (not the target `NotchInfo.depthDB` reports) until the
next explicit Set, and that whether a NaN heal should re-apply the target is a decision for the
caller (`NotchChain::setNotch`), not for `Biquad`. No code touched.

### Optional minors folded in (both trivial and safe)

1. `src/dsp/Biquad.cpp` — `processSample`'s comment claimed "no second branch" while the ramp
   branch has an if/else (assign-on-target vs. add-delta). Reworded to describe both paths
   accurately instead of removing the (correct) claim that there's no allocation or logging.
2. `src/dsp/Biquad.h` — added one clause to the §4.7 proof text noting that ramping from a
   never-configured identity biquad (`b0=1`, all else 0, i.e. depth == 0) is also non-boosting: the
   stability triangle is convex and contains that origin point, so it is still a valid convex
   combination. Cited the reviewer's measured worst case (-3.5e-11 dB).

Not touched: test accessor visibility (Concern 2) and the `<algorithm>` include (Concern 3) — both
flagged by the prior implementer as non-issues / deliberate, and the reviewer did not ask for
either to change.

### Verification

Build:
```
cmake --build build --config Release
```
Clean build, no warnings.

Focused:
```
build/tests/Release/HandsFreeTests.exe --gtest_filter=Biquad*
...
[==========] 24 tests from 1 test suite ran. (16 ms total)
[  PASSED  ] 24 tests.
```

Full suite:
```
cd build && ctest -C Release
...
100% tests passed, 0 tests failed out of 463
Total Test time (real) =  33.36 sec
```

### Concerns

None new. Concern 1 from round 1 (the *original* test comment overstating what the click-step
bound alone proves) is now resolved by the added assertion and the corrected comment. Concerns 2
and 3 from round 1 stand as documented, unchanged, not in scope for this fix round.
