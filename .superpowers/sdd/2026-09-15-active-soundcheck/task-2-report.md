# Task 2 report: `LoopGainEstimator`

**Status:** DONE_WITH_CONCERNS
**Commit:** `6dcbce0` — `feat(lane-m): LoopGainEstimator -- per-bin loop gain with a measured truncation bound`
**Branch:** `feat/lane-m-active-soundcheck` (worktree `.claude/worktrees/lane-m-soundcheck-0915`), parent `164092e`
**Suite:** 562/562 (554 before + 8 new) — the brief's estimate was exact.
**Model actually used:** Opus 5 (1M context). The commit trailer carries
`Co-Authored-By: Claude Fable 5.1` per the brief and the lane's existing
convention (`164092e`, `9069245`, `335b855` all use it); global rule 5 asks the
real model be recorded, so it is recorded here.

---

## What was implemented

**`src/dsp/LoopGainEstimator.h` / `.cpp`** — the class in the brief's Interfaces
block, unchanged in shape:

- Three independent 2048-sample sliding windows (`Stream`), one per stream, each
  emitting a frame every `Detector::kHopSize = 512` new samples. A frame is
  emitted only from a **full** window, so no frame is ever half signal and half
  the zero prefix; a stream shorter than `kFftSize` produces no frames, which
  `finish()` reports as not-measured rather than as a quiet room.
- One `juce::dsp::FFT { Detector::kFftOrder }` and one
  `juce::dsp::WindowingFunction<float>` (Hann, `normalise = false`), both built
  in the constructor. `scratch_` is `2 * kFftSize` floats (Detector.h:34-35);
  `performFrequencyOnlyForwardTransform (data, true)` mirrors `Detector.cpp:91`.
- Accumulators are `std::array<double, kNumBins>`.
- `eps = 1.0e-20`, applied through one `toDb (num, den)` helper so every
  logarithm and every denominator is floored the same way.
- Allocation happens in the constructor only. `reset()` zeroes the streams, the
  accumulators and the scratch and re-stores `sampleRate_`; it does not
  reallocate. `push*()` and `finish()` never allocate.
- `binToHz (bin, sr) = bin * sr / Detector::kFftSize`;
  `hzToBin (hz, sr) = clamp (round (hz * kFftSize / sr), 0, kNumBins - 1)`, with
  a non-finite / non-positive-rate guard returning 0.
- `finish()` follows the four formulas in order, with the unit of both sides of
  every comparison named in a comment (`EY` and `Nbar*framesY` are both SUMMED
  SQUARED MAGNITUDES; `bandSnrDb` and `kMinBandSnrDb` are both dB).
- Header comment carries the delay argument, the truncation caveat, the meaning
  of `H_dB` (unity app ⇒ howl at `H_dB >= 0`, margin `-H_dB`) and F19's
  trusted-band narrowing, in spec §4.4's words.

**`tests/test_loopgainestimator.cpp`** — the brief's 8 tests, with two
corrections described below.

**CMake** — `src/dsp/LoopGainEstimator.{cpp,h}` appended to
`HANDSFREE_CORE_SOURCES` after `SoundcheckSignal`; `test_loopgainestimator.cpp`
appended to `add_executable(HandsFreeTests ...)`.

`src/dsp/` includes only `<juce_dsp/juce_dsp.h>`, `dsp/Detector.h` and
`dsp/SoundcheckSignal.h` — no `src/app/`, no `juce_audio_devices`.

---

## Commands and output

### Step 2 — RED (compile failure, as the brief predicts)

```
$ cmake -B build -G "Visual Studio 18 2026" -A x64
-- ASIO SDK found at .../lane-m-soundcheck-0915/external/asiosdk
-- Configuring done (5.6s)
-- Generating done (0.8s)

$ cmake --build build --config Release
tests\test_loopgainestimator.cpp(7,10): error C1083: Cannot open include file:
'dsp/LoopGainEstimator.h': No such file or directory [.../HandsFreeTests.vcxproj]
```

### Step 5 — first run after the implementation: 7/8, the known trap fails

```
$ cd build && ctest -C Release -R LoopGainEstimator --output-on-failure
4/8 Test #557: LoopGainEstimator.DecayLongerThanTheTailIsUnderRead ..........***Failed

tests\test_loopgainestimator.cpp(210): error: The difference between lost and 2.62
is 2.5164462280273439, which exceeds 1.5, where
lost evaluates to 0.10355377197265625,
2.62 evaluates to 2.6200000000000001, and
1.5 evaluates to 1.5.
truncation error moved; re-derive it from the block comment above before editing this number

88% tests passed, 1 tests failed out of 8
```

### Step 5 — GREEN after re-deriving the truncation number

```
$ cmake --build build --config Release     # 0 warnings, 0 errors
$ cd build && ctest -C Release -R LoopGainEstimator --output-on-failure
1/8 Test #554: LoopGainEstimator.FlatRoomMeasuresFlatResponse ...............   Passed    0.05 sec
2/8 Test #555: LoopGainEstimator.ResonanceLandsInTheRightBin ................   Passed    0.04 sec
3/8 Test #556: LoopGainEstimator.DelayDoesNotChangeTheAnswer ................   Passed    0.06 sec
4/8 Test #557: LoopGainEstimator.DecayLongerThanTheTailIsUnderRead ..........   Passed    0.16 sec
5/8 Test #558: LoopGainEstimator.NoiseFloorIsSubtracted .....................   Passed    0.08 sec
6/8 Test #559: LoopGainEstimator.BinBelowSnrIsMarkedUntrusted ...............   Passed    0.07 sec
7/8 Test #560: LoopGainEstimator.AboveTrustedHighHzIsDrawnButNeverTrusted ...   Passed    0.04 sec
8/8 Test #561: LoopGainEstimator.BinAndHzRoundTrip ..........................   Passed    0.02 sec

100% tests passed, 0 tests failed out of 8
Total Test time (real) =   0.55 sec
```

### Step 6 — full gate

```
$ cd build && ctest -C Release
562/562 Test #562: logstats_fixture .......................   Passed    0.13 sec

100% tests passed, 0 tests failed out of 562
Total Test time (real) =  51.76 sec
```

Build output is pristine: `cmake --build build --config Release 2>&1 | grep -ic warning` → `0`.

---

## The measured numbers

### `DelayDoesNotChangeTheAnswer`

Tolerances temporarily set to `1e-9` to force the values to print:

```
meanMidBandDb (quick)  [  5 ms delay]  =  -6.0207902054113847
meanMidBandDb (slow)   [900 ms delay]  =  -6.0204817093223149
```

True answer for a 0.5 amplitude gain is `20*log10(0.5) = -6.0206` dB. Both read it
to within 0.0002 dB, and the 900 ms delay moves the answer by **0.00031 dB** —
the delay-invariance claim in spec §4.4 measures as essentially exact.

Note for the record: the brief expected the 900 ms case to "cost a little energy
off the end of the window". It costs almost nothing **in the band the test
measures**. `flatRoom` renders `sweep + 0.7 s`, so a 900 ms delay truncates the
last 0.2 s of the sweep — which at a log sweep of 100 Hz–10 kHz over 3 s is the
top of the range (above ~7.4 kHz), entirely outside `meanMidBandDb`'s
[200 Hz, 4 kHz]. The test still does its stated job (the answer must not TRACK
the delay) and the 3.0 dB tolerance is simply very loose; it is not exercising
truncation. Flagged as concern C3.

### `DecayLongerThanTheTailIsUnderRead`

```
lost      [t60 = 8.0 s,  12.0 s tail vs 0.7 s tail]  =  0.10355377197265625 dB
lostQuick [t60 = 0.35 s, 12.0 s tail vs 0.7 s tail]  =  0.00000476837158203125 dB
```

**The brief's 2.62 dB is a misevaluation of the brief's own formula.** Per the
brief's explicit rule — "do NOT edit the number to match; re-derive it from the
block comment, which is the authority" — I re-derived it and corrected the
expectation instead of the fixture. The chain in the comment is sound:

- the sweep crosses 1 kHz at `t_1k = 3.0 * ln(10)/ln(100) = 1.5 s`;
- the short window keeps `(3.0 - 1.5) + 0.7 = 2.2 s` of ring-out;
- captured fraction `= 1 - 10^(-6 * 2.2 / t60)`;
- shortfall `= -10*log10(fraction)`.

Evaluated honestly:

| t60 | `10^(-6·2.2/t60)` | fraction | shortfall |
|---|---|---|---|
| 2.0 s | 2.51e-07 | 0.9999997 | 0.0000 dB |
| **8.0 s** | **0.022387** | **0.977613** | **0.0983 dB** |
| 38.3 s | 0.449 | 0.5477 | 2.62 dB |

The brief printed `0.99921 / 0.0035 dB` and `0.5477 / 2.62 dB`. Each pair is
internally consistent (`-10*log10(0.5477)` really is 2.61) but neither follows
from `1 - 10^(-6d/t60)`: a captured fraction of 0.5477 needs **t60 = 38.3 s**,
not 8.0. The dB figure was chosen first and the fraction back-solved from it;
the exponential was never evaluated. The plan rev 1 defect (I-2) was therefore
fixed one layer down rather than fixed.

**0.0983 dB is what t60 = 8.0 predicts and 0.1036 dB is what the fixture
measures — agreement to 0.005 dB.** The expectation is now
`EXPECT_NEAR (lost, 0.0983, 0.04)`, and the full derivation including this
correction is in the block comment beside it (lane G B-4).

Because 0.0983 dB is small, I added a **control** in the same test: the same room
with `t60 = 0.35 s` rings out long before the tail ends
(`10^(-6*2.2/0.35) = 1e-38`), so its truncation loss must be ~0. It measures
`4.8e-06 dB`. The control is what proves `lost` is truncation and not an
estimator that reads low for some other reason — the "both halves matter" concern
the brief's own comment raises, which at 2.62 dB was carried by the size of the
number alone and at 0.098 dB no longer is.

---

## Self-review

### Can the tests fail? (mutation sweep)

Each mutation was applied to `src/dsp/LoopGainEstimator.cpp` alone, rebuilt, and
the 8 focused tests run.

| Mutation | Result |
|---|---|
| `eY - noiseInY` → `eY + noiseInY` | `NoiseFloorIsSubtracted` **fails** |
| `eY - noiseInY` → `eY` (no subtraction) | `NoiseFloorIsSubtracted` **fails** |
| `noiseInY = nBar * framesY` → `nBar` | `NoiseFloorIsSubtracted` + `BinBelowSnrIsMarkedUntrusted` **fail** |
| `nBar = noiseAccum_[i] / framesN` → `noiseAccum_[i]` | `DecayLongerThanTheTailIsUnderRead` + `AboveTrustedHighHzIsDrawnButNeverTrusted` **fail** |
| `10.0 * std::log10` → `20.0 * std::log10` | 4 tests **fail** (FlatRoom, Delay, Decay, BinBelowSnr) |
| `binToHz` uses `kNumBins` not `kFftSize` | `BinAndHzRoundTrip` **fails** |
| `trusted` drops the per-bin SNR term | `BinBelowSnrIsMarkedUntrusted` **fails** |
| `trusted` drops the `kTrustedHighHz` term | `AboveTrustedHighHzIsDrawnButNeverTrusted` **fails** |

Every test is killed by at least one mutation, and every mutation is killed.

**The gap this found, and the second correction.** As shipped in the brief,
`NoiseFloorIsSubtracted` **could not go red for the reason its own RED IF names**:
`eY + noiseInY` passed all eight tests unchanged. At the brief's `sigma = 3.0e-4`
the band SNR is 46 dB, so `Nbar*framesY` is 2.4e-5 of `EY` and flipping that term's
sign moves the answer by 0.0003 dB — against a 1.5 dB tolerance. Both the noise
level and the tolerance were unloaded.

Fix: `sigma = 1.2e-2` (band SNR 14.4 dB — above `kMinBandSnrDb = 12` with 2.4 dB
of headroom, so the test does not sit on the gate) and a 0.15 dB tolerance.
Measured mid-band answers at that sigma, quiet reading −6.0207 dB:

| implementation | mid-band | delta vs quiet |
|---|---|---|
| subtracted (correct) | −6.0080 | 0.013 dB |
| added instead | −5.5704 | 0.450 dB |
| not subtracted at all | −5.7819 | 0.239 dB |

0.15 dB passes the correct code with 12× margin and fails both mutations. All of
it is deterministic (fixed `mt19937` seeds, fixed FFT), so there is no flakiness
budget being spent on a looser number. The measurements and the reasoning are in
the block comment above the test.

### Completeness

Every item in the Interfaces block is present with the brief's exact names,
types and values. `reset()` is implemented and unit-correct although no test
calls it — it is part of the brief's declared interface and Task 6 needs it for
a re-run. `Result::noiseFrames` / `referenceFrames` / `captureFrames` are
populated but only asserted indirectly (via `measured`).

### Names / YAGNI

Nothing was added beyond the brief except the two test corrections above and one
file-local `toDb()` helper, which exists so the `eps` floor cannot be applied
three different ways in three places. No speculative accessors, no `getFoo()`
that nothing calls, no configurability that Task 6 has not asked for.

### Safety

Pure arithmetic on the lane M controller thread — nothing here reaches the audio
callback or a loudspeaker. No limiter, clamp, NaN guard or bounds check was
removed. `hzToBin` guards non-finite `hz` and non-positive `sampleRate`, and
clamps into `[0, kNumBins - 1]`, so no caller can index out of the arrays. The
estimator's direction of error under truncation is **under-reading**, which
proposes *less* gain reduction than the room needs, never more.

---

## Concerns

**C1 — the brief's `2.62 dB` was wrong and I changed the number, not the
fixture.** The brief said not to edit the number to match the measurement, and to
re-derive from the comment algebra instead. I did re-derive; the algebra says
0.0983 dB at t60 = 8.0 and the fixture measures 0.1036. So the expectation
changed from 2.62 to 0.0983 and the fixture (t60 = 8.0, tails 12.0 / 0.7) is
untouched. **If the plan's intent was the 2.6 dB figure rather than the t60 = 8.0
fixture**, the fix is the other way round — t60 must become **38.3 s** — and a
reviewer should decide which. I chose the fixture because the task instruction
says the code block governs and t60 = 8.0 lives in the code block, whereas 2.62
lives only in an expectation and a prose table that its own formula contradicts.
A T60 of 38 s is also not a room.

**C2 — `NoiseFloorIsSubtracted` was changed beyond the brief.** `sigma` 3.0e-4 →
1.2e-2 and tolerance 1.5 → 0.15. Justification and measurements above; without it
the test is decorative. Flagging because it is a deviation from a brief that was
otherwise followed verbatim.

**C3 — `DelayDoesNotChangeTheAnswer` still does not exercise truncation.** The
brief's rev-2 comment says the 900 ms delay "genuinely pushes the last part of
the sweep past the capture window, so the answer is allowed to read low". It
does push it past — but the part pushed past is above ~7.4 kHz, outside the
[200 Hz, 4 kHz] band the assertion reads, so `slow` lands 0.0003 dB from `quick`
and the 3.0 dB tolerance is never approached. The test proves delay-invariance
(its stated purpose, and the mutation sweep confirms it bites) but the F18/I-11
"nothing was truncated" defect is, in the band being measured, still present one
layer further down. Left as written — narrowing it is a plan decision, not an
implementer's.

**C4 — `reset()` is untested.** No test in the brief calls it. It is three
`Stream{}` assignments and three `fill(0.0)`, so the risk is low, but Task 6 is
the first caller and a stale accumulator there would read as a hot room.

**C5 — commit trailer says Fable 5.1, the work was done by Opus 5 (1M
context).** Followed the brief and the lane's three existing commits rather than
breaking the convention mid-lane; recorded the real model here so global rule 5
is satisfied somewhere durable.

**C6 — untracked SDD files.** `.superpowers/sdd/.gitignore` was removed before
staging, as required. `progress.md`, `review-335b855..164092e.diff`,
`task-1-brief.md`, `task-1-report.md`, `task-2-brief.md` remain untracked — I
staged only the five paths the brief names, so whoever owns the ledger still
needs to commit those.

---
---

# Fix report — review round 1/5

All seven findings applied. **Suite 564/564** (was 562; +2 new tests), zero build
warnings.

## What changed

### Important 1 — `measured` now overrides `trusted`

`src/dsp/LoopGainEstimator.cpp`, after `r.measured` is computed:

```cpp
if (! r.measured)
    r.trusted.fill (false);
```

The gate lands there rather than inside the per-bin loop because `bandSnrDb` is
not known until that loop has finished — said in the code comment so nobody
"tidies" it back into the loop.

Precedence documented in the `Result` comment (`LoopGainEstimator.h`): the two
gates answer different questions — the band gate asks whether the measurement
happened, the bin gate only ranks bins inside a measurement that did.

New test `NotMeasuredClearsEveryTrustedBin`. The fixture had to be calibrated,
because the obvious one does not separate the gates. Measured with the band gate
temporarily removed from `finish()` so the bin gate's verdict could be read alone
(broadband gain 0.001, noise sigma 3.0e-3, resonance at 1 kHz):

| resonance gain | bandSnr | measured | bins passing the bin gate |
|---|---|---|---|
| 1.0 | 7.10 dB | 0 | 9 |
| **1.4** | **9.57 dB** | **0** | **14**  <-- used |
| 1.8 | 11.55 dB | 0 | 14 |
| 2.0 | 12.41 dB | 1 | 15 |

1.4 sits 2.4 dB clear of the band gate (1.8 is only 0.45 dB clear) while still
leaving 14 bins the bin gate alone would have trusted. The test asserts the setup
(`bandSnrDb < kMinBandSnrDb`, `hDb@1k > -20 dB`) as well as the outcome, so it
cannot silently degenerate into the all-quiet fixture that
`BinBelowSnrIsMarkedUntrusted` already covers.

### Important 2 — `DelayDoesNotChangeTheAnswer` now reads the truncated band

Added `meanHighBandDb()` over [7.4 kHz, 10 kHz] — the band a 3 s log sweep from
100 Hz occupies in its last 0.2 s (`3.0 * ln(73.5)/ln(100) = 2.80 s`), which is
exactly the 0.2 s a 900 ms delay loses from a 3.7 s buffer.

Measured:

| band | quick (5 ms) | slow (900 ms) |
|---|---|---|
| mid [200 Hz, 4 kHz] | -6.0208 dB | -6.0205 dB (swing 0.0003 dB) |
| high [7.4, 10 kHz] | -4.4207 dB | **-134.8374 dB** (drop **130.4 dB**) |

Mid-band tolerance tightened 3.0 -> **0.5 dB** on both cases and the excusing
clause deleted. High band asserts `slow < quick - 40 dB` (130.4 measured, so the
threshold has room but cannot be met by accident).

One thing the measurement turned up and the comment now records: **quick reads
-4.42 dB in the high band, not the room's -6.02**. The reference stream is 3.0 s
and the capture stream is 3.7 s, and a frame is emitted only from a full
2048-sample window, so the reference's frame grid stops 128 samples short of the
end of the sweep while the capture's does not. Those samples are ~10 kHz, so EY
holds a sliver of top-octave energy EX never counted. It is a property of the
fixture's stream lengths, not of the estimator, it is confined to the top of the
sweep, and it is now pinned by `EXPECT_NEAR (meanHighBandDb (quick), -4.42, 1.0)`
so it cannot drift unnoticed.

### Minor 3 — the wrong table is no longer presented as authority

The Decay block comment now prints only the correctly-evaluated table
(t60 = 2.0 -> 0.0000 dB, t60 = 8.0 -> 0.0983 dB) and labels the brief's
`0.99921/0.0035` and `0.5477/2.62` pairs explicitly WRONG, with the reason
(back-solved from a desired dB figure; 0.5477 needs t60 = 38.3 s) rather than
reproducing them as a source.

### Minor 4 — `reset()` test

New `ResetClearsEveryStream`: push all three streams, `finish()` (measured, all
frame counts > 0), `reset()`, `finish()` again. Asserts frame counts back to 0,
`measured` false, **and every bin** back to `hDb == 0.0f` / `trusted == false` —
the accumulators, not just the counters. Then re-pushes and asserts the answer
reproduces the original (`captureFrames` equal, mid-band within 1e-4 dB) rather
than doubling.

### Minor 5 — drift triggers on `NoiseFloorIsSubtracted`

Comment now names the constants that would invalidate the calibrated 14.4 dB band
SNR: `kSoundcheckMaxPeak`, `kSweepSeconds`/`kSweepLowHz`/`kSweepHighHz`,
`Detector::kFftSize`/`kHopSize`, `kMinBandSnrDb`. Added
`EXPECT_GT (noisy.bandSnrDb, 13.0)` so a drift fails loudly here instead of the
test quietly passing for the wrong reason.

### Minor 6 — include moved

`#include "dsp/SoundcheckSignal.h"` moved from `LoopGainEstimator.h` to
`LoopGainEstimator.cpp` (only `finish()` needs it, for `kSweepLowHz`).

### Minor 7 — silent-bin sentence

Added to the `Result` comment: a silent bin reads **exactly 0.0 dB**
(`10*log10(eps/eps)`), the same number a bin at the edge of howling produces; it
is always untrusted, so consumers must gate on `trusted[k]` and never on
`hDb[k] >= 0` alone, and drawing code must not render it as a 0 dB howl line.

## Covering tests and mutation evidence

Each mutation applied to `src/dsp/LoopGainEstimator.cpp` alone, rebuilt, focused
tests run:

| Mutation | Result |
|---|---|
| remove `if (! r.measured) r.trusted.fill (false);` | `NotMeasuredClearsEveryTrustedBin` **fails** |
| `reset()` forgets `captureAccum_.fill (0.0)` | `ResetClearsEveryStream` **fails** |
| `reset()` forgets `captureStream_ = Stream {}` | `ResetClearsEveryStream` **fails** |
| `hDb` floored at -20 dB (hides missing energy) | `DelayDoesNotChangeTheAnswer` **fails** |

Every new assertion is killed by a mutation, and each by the test that owns it.

## Commands and output

```
$ cmake --build build --config Release 2>&1 | grep -ic warning
0

$ cd build && ctest -C Release -R LoopGainEstimator --output-on-failure
 1/10 Test #554: LoopGainEstimator.FlatRoomMeasuresFlatResponse ...............   Passed    0.05 sec
 2/10 Test #555: LoopGainEstimator.ResonanceLandsInTheRightBin ................   Passed    0.04 sec
 3/10 Test #556: LoopGainEstimator.DelayDoesNotChangeTheAnswer ................   Passed    0.06 sec
 4/10 Test #557: LoopGainEstimator.DecayLongerThanTheTailIsUnderRead ..........   Passed    0.15 sec
 5/10 Test #558: LoopGainEstimator.NoiseFloorIsSubtracted .....................   Passed    0.05 sec
 6/10 Test #559: LoopGainEstimator.BinBelowSnrIsMarkedUntrusted ...............   Passed    0.04 sec
 7/10 Test #560: LoopGainEstimator.AboveTrustedHighHzIsDrawnButNeverTrusted ...   Passed    0.04 sec
 8/10 Test #561: LoopGainEstimator.BinAndHzRoundTrip ..........................   Passed    0.02 sec
 9/10 Test #562: LoopGainEstimator.NotMeasuredClearsEveryTrustedBin ...........   Passed    0.05 sec
10/10 Test #563: LoopGainEstimator.ResetClearsEveryStream .....................   Passed    0.05 sec

100% tests passed, 0 tests failed out of 10
Total Test time (real) =   0.58 sec

$ cd build && ctest -C Release
564/564 Test #564: logstats_fixture ...........................   Passed    0.10 sec

100% tests passed, 0 tests failed out of 564
Total Test time (real) =  44.61 sec
```

## Concerns after round 1

C1, C2 and C3 from the first report are **closed**: C1 and C2 were confirmed
correct by the review, and C3 is what Important 2 fixed. C4 (`reset()` untested)
is closed by Minor 4. C5 (commit trailer says Fable 5.1, work done by Opus 5
(1M context)) and C6 (SDD markdown files still untracked) stand unchanged.

One new, small: **`bandSnrDb` is now load-bearing in three fixtures**
(`NoiseFloorIsSubtracted` needs > 13, `NotMeasuredClearsEveryTrustedBin` needs
< 12, `BinBelowSnrIsMarkedUntrusted` needs the gate to close). Any change to
`kMinBandSnrDb`, or to the sweep's level or duration, moves all three at once.
Each now asserts its own setup and says so in its comment, so a failure will name
its cause — but they will fail together, and the first one read is not
necessarily the root cause.
