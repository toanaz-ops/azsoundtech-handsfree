# Task 3 report — `SoundcheckCandidates`

**Status: DONE_WITH_CONCERNS.** Commit `119b033`. `577/577` on `ctest -C Release`.

Worktree `.claude/worktrees/lane-m-soundcheck-0915`, branch
`feat/lane-m-active-soundcheck`, on top of `327762b`.

## What was implemented

| File | |
|---|---|
| `src/dsp/SoundcheckCandidates.h` | new — the interface exactly as the brief specifies it |
| `src/dsp/SoundcheckCandidates.cpp` | new — `depthFor`, `smooth`, `pick` |
| `tests/test_soundcheck_candidates.cpp` | new — the brief's 12 tests **plus one** (see Finding 2) |
| `CMakeLists.txt` | `HANDSFREE_CORE_SOURCES` += the .cpp/.h |
| `tests/CMakeLists.txt` | += `test_soundcheck_candidates.cpp` |

`depthFor` is a transcription of the corrected §4.4 rule, with the unit of both
sides of every comparison in a comment. All six worked examples hold as exact
numbers against `NotchController::kDepthLadderDb` — the shipped array, reached
through the `Ladder` parameter, so `src/dsp/` still includes nothing from
`src/app/`.

`pick` runs the seven steps in the brief's order. Two implementation decisions
that the brief left open:

- **Step 7 is an insertion into a fixed six-slot array**, not a sort of a
  collected list. It keeps the hottest six by construction and allocates
  nothing; ties keep the lower bin first.
- **Step 6 rejects `depthDb >= 0`, not just `needed < kMinUsefulCutDb`.** The
  rule's "no proposal" sentinel is `depthDb == 0`, and a preset whose
  `ceilingDb` is 0 reaches that same value through the ceiling clamp. Testing
  the sentinel rather than the threshold means a 0 dB ceiling produces silence
  instead of a 0 dB notch occupying one of sixteen chain slots. No brief test
  changes either way.

## Commands and output

### RED (step 2)

```
$ cmake --build build --config Release
tests\test_soundcheck_candidates.cpp(10,10): error C1083: Cannot open include
file: 'dsp/SoundcheckCandidates.h': No such file or directory
[...\build\tests\HandsFreeTests.vcxproj]
```

Exactly the failure the brief predicted.

### GREEN (step 4)

```
$ cmake -B build -G "Visual Studio 18 2026" -A x64
-- ASIO SDK found at .../external/asiosdk
-- Configuring done (5.2s)
-- Generating done (0.4s)

$ cmake --build build --config Release
  SoundcheckCandidates.cpp
  HandsFree.vcxproj -> ...\AZ Soundtech Hands-free.exe
  HandsFreeTests.vcxproj -> ...\build\tests\Release\HandsFreeTests.exe

$ cd build && ctest -C Release -R SoundcheckCandidates --output-on-failure
 1/13 Test #564: SoundcheckCandidates.DepthSignIsNegative ............ Passed
 2/13 Test #565: SoundcheckCandidates.DepthQuantisesOntoTheLadder .... Passed
 3/13 Test #566: SoundcheckCandidates.CeilingClampsTheProposal ....... Passed
 4/13 Test #567: SoundcheckCandidates.CeilingNotMultipleOfSixEndsOnTheCeiling  Passed
 5/13 Test #568: SoundcheckCandidates.SaturatesAtMinusTwentyFourAndReportsResidual  Passed
 6/13 Test #569: SoundcheckCandidates.SaturationByTheCeilingIsAlsoReported  Passed
 7/13 Test #570: SoundcheckCandidates.MarkedButNotProposedBelowMinUsefulCut  Passed
 8/13 Test #571: SoundcheckCandidates.SpeakerRolloffIsNotACandidate .. Passed
 9/13 Test #572: SoundcheckCandidates.BinWithALiveNotchIsSkipped ..... Passed
10/13 Test #573: SoundcheckCandidates.AtMostSixPerLaneAndTheHottestSurvive  Passed
11/13 Test #574: SoundcheckCandidates.UntrustedBinIsNeverACandidate .. Passed
12/13 Test #575: SoundcheckCandidates.MarginIsTheNegativeOfLoopGain .. Passed
13/13 Test #576: SoundcheckCandidates.AboveTheTrustedBandIsNeverACandidate  Passed

100% tests passed, 0 tests failed out of 13
```

### Full gate (step 5)

```
$ cd build && ctest -C Release
577/577 Test #577: logstats_fixture ................................. Passed

100% tests passed, 0 tests failed out of 577
Total Test time (real) =  44.39 sec
```

**577, not the brief's 574.** The brief's estimate was written before Task 2
added two tests: the pre-task baseline was 564, plus 13 here.

### Warnings

```
$ touch src/dsp/SoundcheckCandidates.cpp tests/test_soundcheck_candidates.cpp
$ cmake --build build --config Release 2>&1 | grep -iE "warning|error"
--- warning/error grep exit: 1 (1 = none found) ---
```

Pristine: both new translation units recompile with no diagnostic at all.

## Mutation evidence

Each mutation was applied to `SoundcheckCandidates.cpp` alone, built, run
against `-R SoundcheckCandidates`, then reverted and the tree rebuilt clean.
Script: scratchpad `mutate.py` / `mutate2.py` (not committed).

| # | Mutation | Result |
|---|---|---|
| M1 | quantiser inverted — `r >= depthRaw && r < rung` (spec rev 2's N2) | RED: DepthQuantisesOntoTheLadder, DepthSignIsNegative, CeilingClampsTheProposal, CeilingNotMultipleOfSix…, MarginIsTheNegative… |
| M2 | ceiling clamped with `std::min` instead of `std::max` | RED: 6 tests incl. CeilingClampsTheProposal, SaturationByTheCeiling… |
| M5 | `needed = -(hDb + kTargetMarginDb)` — rev 1's sign (F6) | RED: 9 tests |
| M10 | `residualDb = 0.0` — saturation swallowed | RED: SaturatesAtMinusTwentyFour…, SaturationByTheCeiling… |
| M3 | step 7 comparison reversed — coldest six survive | RED: AtMostSixPerLaneAndTheHottestSurvive |
| M4 | smoother is a no-op (`out = in`) | RED: AtMostSix…, MarginIsTheNegative…, MarkedButNotProposed…, SaturatesAtMinusTwentyFour…, UntrustedBin… |
| M6 | live-notch window narrowed to `k == lb` (inv 15) | RED: BinWithALiveNotchIsSkipped |
| M7 | `marginDb = +H_dB` | RED: MarginIsTheNegative…, AtMostSix… |
| M8 | propose threshold replaced by the mark threshold (F22) | RED: MarkedButNotProposedBelowMinUsefulCut |
| M9 | `trusted[]` never consulted | RED: UntrustedBinIsNeverACandidate |
| M11 | smoother FLAT (whole-array mean) | RED: SpeakerRolloffIsNotACandidate |
| M12 | step 1 band filter removed entirely | **STILL GREEN on the brief's 12** → Finding 2 |

No mutation of the depth rule or of the sort survived. The two findings below
came out of M4 and M12.

### Finding 1 — the brief's m-19 claim about the smoother is inverted

The brief states: *"A smoother that returned its input unchanged would make
every prominence zero and turn that test red"*, naming
`SpeakerRolloffIsNotACandidate` as the smoother's coverage.

M4 disproves it. A no-op smoother does make every prominence exactly zero — and
therefore marks **nothing**, so a test that asserts *zero candidates* still
passes. `SpeakerRolloffIsNotACandidate` stayed green under M4.

The smoother is nonetheless covered, from both sides:

- **too narrow / no-op** — caught by the five tests that require a candidate to
  EXIST (M4's red list above);
- **too wide / too flat** — caught by `SpeakerRolloffIsNotACandidate`, verified
  by M11 (a whole-array-mean smoother makes the roll-off look prominent).

No code change was needed; the header comment on `smooth()` was rewritten to
state the real coverage instead of repeating the brief's claim, because that
comment is what the next lane will read before changing the window width.

### Finding 2 — step 1 of the pick order was untested; a 13th test was added

M12 removed the `[kSweepLowHz, kTrustedHighHz]` band test from `pick`
altogether and **all twelve of the brief's tests stayed green**. In production
`LoopGainEstimator::finish()` already clears `trusted[]` above
`kTrustedHighHz`, so step 2 hides the gap — but `pick` is a pure function that
must not lean on its caller's invariant, and this module decides how much gain
leaves a PA.

`AboveTheTrustedBandIsNeverACandidate` was added: a hot, prominent poke at
8 kHz with `trusted == true` (a combination the estimator never emits, which is
precisely why nothing else exercises step 1), plus a control at 2 kHz proving
the zero is the band test and not a dead fixture. Re-running M12 with it
present turns it red:

```
13/13 Test #576: SoundcheckCandidates.AboveTheTrustedBandIsNeverACandidate ***Failed
92% tests passed, 1 tests failed out of 13
```

This is a deliberate departure from the brief's 12-test list, recorded in the
commit message and in the test's own comment.

The LOW half of step 1 is deliberately **not** asserted: at 48 kHz a bin below
`kSweepLowHz` sits at index ≤ 4, and a ±2^(1/6) window around it rounds to that
single bin, so its prominence is exactly 0 and step 5 rejects it before step 1
could matter. An assertion there would be untestable theatre; the reasoning is
in the test comment instead.

## Self-review

**Completeness.** All seven pick steps present and in the specified order; all
six §4.4 worked examples asserted as exact numbers against the shipped ladder;
both CMake lists updated; reconfigure done after the CMake change.

**Names.** The public interface is character-for-character the brief's, so
Task 4's `SoundcheckController` can be written against the brief unchanged.

**YAGNI.** No public surface beyond the brief. `smooth` is public only because
the brief says a later lane reuses it. No helper types, no allocation in `pick`
beyond the returned `Output`.

**Tests can fail.** Twelve mutations, twelve reds — plus the two findings above,
which are the cases where "can fail" turned out to be *false* and got fixed.

**Build output.** Pristine; forced recompile of both new TUs emits no
diagnostic.

**Layering.** `SoundcheckCandidates.h` includes `dsp/LoopGainEstimator.h` and
`<array>`; the .cpp adds `dsp/SoundcheckSignal.h`, `<algorithm>`, `<cmath>`.
Nothing from `src/app/`, no `juce_events`, no `juce_audio_devices`.

## Concerns

1. **`saturatedBins` counts only the surviving six.** The brief does not say
   whether it counts every saturated proposal or only the ones kept. I count
   the survivors, so the number and the per-candidate `residualDb` beside it
   always describe the same set — but a seventh, colder, saturated bin is then
   dropped by the six-cap without appearing in any count. Whoever writes the
   GUI string in a later task should confirm that is the intended meaning.
2. **`Input::ceilingDb` defaults to 0.0, which means "no cut allowed".** A
   caller who forgets to set it gets silence (step 6 rejects `depthDb >= 0`),
   not a wrong cut — the safe direction, but silent. `SoundcheckController`
   must set it from the running preset explicitly.
3. **`Candidate` carries floats while `depthFor` works in double.** Every value
   the app ships today (−6/−12/−18/−24, and Music's −10.0) is exactly
   representable, so nothing rounds now. A future preset ceiling that is not
   (e.g. −10.3) would show a `residualDb` off in the fourth decimal in the GUI.
4. **`Candidate::bin` is populated but unconsumed** until Task 4 maps a
   candidate back onto a detector bin. Kept because the brief's struct declares
   it.
5. **Full-suite count is 577, not the brief's 574** — the brief's estimate
   predates Task 2's two extra tests. Later briefs in this lane should start
   from 577.

---

# Fix report — review round 1/5

**Commit `b33d032`.** `581/581` on `ctest -C Release`; `17/17` on
`-R SoundcheckCandidates`. All seven review items applied.

## Changes

| Item | Change |
|---|---|
| **I-1** | `depthFor` returns the mark-only `Depth` when `ladder.rungsDb == nullptr \|\| ladder.count <= 0` — before the loop can deref. `pick` computes the same predicate into `ladderMissing` and refuses every proposal while still computing marks. |
| **I-2** | New test `ShallowOffLadderCeilingStillCuts` through `pick()`: `ceilingDb = -2.0`, `H_dB = -1` ⇒ one candidate, `depthDb == -2`, `residualDb == 3`, `saturatedBins == 1`. |
| **I-3** | `Input::ceilingDb` now defaults to `std::numeric_limits<double>::quiet_NaN()`; `Output::ceilingMissing` added; a non-finite ceiling computes marks, proposes nothing, raises the flag. The three states (NaN / 0.0 / < 0) are documented on the field, and `Output::ceilingMissing` tells Task 6 to surface it rather than render an empty list as a clean result. |
| **M-2** | `if (! std::isfinite (h)) continue;` before step 5. The prominence gate was also rewritten from `prom < k` to `! (prom >= k)` — see below. |
| **M-1** | The "low half of step 1 is unreachable" comment is scoped to `sampleRate >= 40.96 kHz`, with the 32 kHz counter-example (bin spacing 15.625 Hz, 93.75 Hz is bin 6, window spans bins 5..7) spelled out and the conclusion stated: at 32 kHz that half is LOAD-BEARING and must not be removed on the strength of this test's silence. |
| **M-3** | `smooth()` header comment: `hi` is clamped to `numBins - 1`, so above Nyquist / 2^(1/6) the window is one-sided and the result biased by the local slope — harmless as used (step 1 drops everything above 6 kHz) but meaningful only below `kTrustedHighHz` for a later reuser. |
| **M-5** | One-line comment in `pick`: the low edge here is `hz < kSweepLowHz` (excludes bin 4 at 48 k) while the estimator's band-SNR window starts at `hzToBin(kSweepLowHz) == bin 4` and includes it — aggregate vs per-bin, no functional consequence. |
| extra | `Input::trusted` comment: pass `Result::trusted` VERBATIM; `pick` has no `measured` input and cannot re-derive that gate, and a caller that rebuilds the array from per-bin SNR reopens the hole Task 2's review closed. |
| extra | The 13th test (`AboveTheTrustedBandIsNeverACandidate`, control half) added to the smoother comment's no-op list. |

**One deviation from the literal instruction, matching its stated intent.** I-1
gave the fix as `if (...) return out;` at the top of `pick`, but the same
sentence requires "marks still computed" and the test it specifies is
`markedCount > 0 && candidateCount == 0`. An early `return out` produces
`markedCount == 0`. I implemented the flag form, which satisfies the stated
test and the stated intent.

## Covering tests (4 new, 13 → 17)

`EmptyLadderMarksButProposesNothing`, `ShallowOffLadderCeilingStillCuts`,
`UnsetCeilingIsDetectedAndRefused`, `NonFiniteLoopGainIsNeverMarked`.

`UnsetCeilingIsDetectedAndRefused` asserts both states: NaN ⇒
`ceilingMissing == true`, 0 proposals, marks > 0; and `ceilingDb = 0.0` ⇒
`ceilingMissing == false`, 0 proposals, marks > 0. `NonFiniteLoopGainIsNeverMarked`
pokes NaN at 1000 Hz, a markable −5 dB bin at 1050 Hz *inside* the NaN's
1/3-octave window, and a real mode at 2000 Hz outside it, then asserts exactly
one mark and one proposal survive; a second field repeats it with `+infinity`.

## Commands and output

```
$ cmake --build build --config Release 2>&1 | grep -iE "warning|error"
--- warnings/errors above (none = clean) ---

$ cd build && ctest -C Release -R SoundcheckCandidates --output-on-failure
13/17 Test #576: SoundcheckCandidates.AboveTheTrustedBandIsNeverACandidate  Passed
14/17 Test #577: SoundcheckCandidates.EmptyLadderMarksButProposesNothing .. Passed
15/17 Test #578: SoundcheckCandidates.ShallowOffLadderCeilingStillCuts .... Passed
16/17 Test #579: SoundcheckCandidates.UnsetCeilingIsDetectedAndRefused .... Passed
17/17 Test #580: SoundcheckCandidates.NonFiniteLoopGainIsNeverMarked ...... Passed

100% tests passed, 0 tests failed out of 17
Total Test time (real) =   0.43 sec

$ cd build && ctest -C Release
100% tests passed, 0 tests failed out of 581
Total Test time (real) =  44.04 sec
```

581 = 564 baseline + 17. No warnings on either new/changed translation unit.

## Mutation evidence

| # | Mutation | Result |
|---|---|---|
| I-1a | `depthFor`'s ladder guard removed | RED: EmptyLadderMarksButProposesNothing |
| I-1b | `pick`'s `ladderMissing` forced to `false` | **STILL GREEN** — see below |
| I-1 | **BOTH** ladder guards removed | RED: EmptyLadderMarksButProposesNothing |
| I-2 | step-6 sentinel widened to `d.depthDb <= -6.0` (reject off-ladder depths) | RED: ShallowOffLadderCeilingStillCuts |
| I-3 | `ceilingMissing` forced to `false` | RED: UnsetCeilingIsDetectedAndRefused |
| M-2a | `! std::isfinite (h)` check removed | **STILL GREEN** — see below |
| M-2b | prominence gate reverted to `prom < kMinProminenceDb` | RED: NonFiniteLoopGainIsNeverMarked |
| M-2 | **BOTH** non-finite defences removed | RED: NonFiniteLoopGainIsNeverMarked |

### The two survivors are redundancy, not missing coverage

**I-1b.** `pick`'s guard and `depthFor`'s guard are two statements of one rule.
With `depthFor`'s guard standing, an empty ladder already returns `depthDb == 0`
and the step-6 sentinel drops the proposal, so neutering `pick`'s flag changes
no output. Removing **both** is red.

**M-2a.** The explicit `isfinite` check and the NaN-safe prominence form are
likewise two statements of one rule: a NaN in `hDb` also makes `prom` NaN, and
`! (prom >= k)` rejects it. Removing **both** is red.

Neither was removed. Both are defence-in-depth the review asked for, and
`depthFor` in particular is public and reachable without going through `pick`.
What I did instead was write the measurement into the code at both sites, so the
next reader does not delete an apparently-untested guard:

```cpp
    // DELIBERATELY REDUNDANT with depthFor's own guard: while both stand, no
    // test can tell them apart (measured 2026-09-16 -- neutering either one
    // alone leaves the suite green; removing BOTH turns
    // EmptyLadderMarksButProposesNothing red). Keep both anyway. [...]
```

## New concerns from this round

1. **`prom < k` ⇒ `! (prom >= k)` was not in the review's list.** The review
   asked only for the `isfinite` check on `h`. That alone does not close the
   hole: `hs[i]` is NaN for every bin whose 1/3-octave window *contains* a
   non-finite bin, and under `prom < k` such a neighbour passes a prominence
   gate it never satisfied. M-2b proves it. The rewritten comparison is
   identical for all finite inputs and rejects in the safe direction.
2. **Concern 2 of the first round is now resolved** by I-3 and struck from the
   list; concerns 1, 3, 4 and 5 stand unchanged (`saturatedBins` counts only
   the surviving six; float `Candidate` fields vs double arithmetic;
   `Candidate::bin` unconsumed until Task 4; the suite is 581, not the brief's
   estimate).
3. **`Output` is now 1 byte larger and `Input`'s default changed.** Any Task 4+
   code already written against `Input{}` with an implicit 0 dB ceiling will now
   get `ceilingMissing` and no proposals. That is the intended, loud failure —
   but it is a source-compatible change with a behaviour change, so it needs to
   reach whoever writes `SoundcheckController`.

---

# Fix report — review round 2/5

**Commit `23c99f1`.** `582/582` on `ctest -C Release`; `18/18` on
`-R SoundcheckCandidates`. Both open items applied.

## Changes

**I-3 residual — `depthFor` now refuses a non-finite ceiling.** The reviewer's
reading of `std::max` is correct: it is `(a < b) ? b : a`, so
`std::max (rung, NaN)` evaluates `rung < NaN`, which is false, and hands back
the raw rung. `depthFor (2.0, NaN, ladder)` returned a fully formed −12 dB
proposal with `saturated == false`. It now mirrors the ladder guard:

```cpp
    if (! std::isfinite (ceilingDb))
        return out;
```

`pick` raises `Output::ceilingMissing` for its own callers; `depthFor` is
public and has no such channel, so refusing is the only honest answer. Stated
on the declaration in the header too, so the contract is visible at the call
site.

**`pick`'s flags are computed before the null-input early return.**
`ladderMissing` and `ceilingMissing` are now evaluated, and
`out.ceilingMissing` written, above the `hDb == nullptr || trusted == nullptr ||
sampleRate <= 0` return. A controller that forgot the ceiling has most likely
forgotten the buffers too, and reporting "no proposals, ceiling fine" on the
way out of that would have pointed the investigation at the room.

## Covering tests (1 new + 1 extended, 17 → 18)

- **New `DepthForRefusesANonFiniteCeiling`** — NaN, `+inf` and `−inf` all ⇒
  `depthDb 0`, `residualDb 0`, `saturated false`, each with the offending value
  in the failure message; opens with a control asserting the same `H_dB` with a
  real ceiling IS −12 dB, so the zeroes cannot pass on a dead fixture.
- **`UnsetCeilingIsDetectedAndRefused` extended** with the null-input case: a
  default-constructed `Input` ⇒ `ceilingMissing true`, `candidateCount 0`,
  `markedCount 0`.

## Commands and output

```
$ cmake --build build --config Release 2>&1 | grep -iE "warning|error"
--- clean ---

$ cd build && ctest -C Release -R SoundcheckCandidates
100% tests passed, 0 tests failed out of 18
Total Test time (real) =   0.44 sec

$ cd build && ctest -C Release
100% tests passed, 0 tests failed out of 582
Total Test time (real) =  44.49 sec
```

## Mutation evidence

| # | Mutation | Result |
|---|---|---|
| R2-a | `depthFor`'s non-finite ceiling guard removed | RED: DepthForRefusesANonFiniteCeiling |
| R2-b | `ceilingMissing` written AFTER the null-input early return | RED: UnsetCeilingIsDetectedAndRefused |

Both isolate cleanly — unlike round 1's two redundant pairs, each of these
guards is the only thing standing between the input and the wrong answer.

## Concerns

No new ones. The standing list is unchanged from round 1: `saturatedBins`
counts only the surviving six; `Candidate`'s float fields vs double arithmetic;
`Candidate::bin` unconsumed until Task 4; the `Input{}` default-behaviour
change must reach whoever writes `SoundcheckController`; suite baseline is now
582.
