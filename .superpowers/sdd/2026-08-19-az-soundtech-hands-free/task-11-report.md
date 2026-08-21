# Task 11 report: Peakiness scoring

- **Model used:** `claude-opus-5`
- **Status:** `DONE_WITH_CONCERNS`
- **Branch:** `feat/task-11-peakiness`
- **Commit:** `e65ddfc`  (report itself is untracked: `.superpowers/` is gitignored, matching tasks 1-10)

## Headline finding (read this first)

**The spec's peakiness metric, at the spec's `±2` neighbour radius, cannot
detect a feedback tone at the plan's `10.0` threshold — or at any threshold.**

`Detector` applies a Hann window before a 1024-point FFT, so a sinusoid
occupies a **four-bin main lobe**. Bins `i±1` and `i±2` — precisely the
neighbourhood the spec averages over — are therefore filled by the tone itself
and scale with it. Peakiness is a ratio, so it saturates:

```
Periodic-Hann kernel magnitudes at offsets (0, ±1, ±2) = (0.5, 0.25, 0)
mean(neighbours) = (0.25 + 0.25 + 0 + 0) / 4 = 0.125
peakiness        = 0.5 / 0.125 = 4.0          <-- hard ceiling for ANY tone
```

Measured (independent double-precision DFT, then confirmed by the C++ tests):

| Signal | peakiness |
|---|---|
| Tone exactly on a bin centre | **3.99** |
| 1 kHz @ 48 kHz (bin 21.33) | **3.29** |
| Worst case, half-bin offset | **2.80** |
| 1 kHz @ amplitude **100** + noise | **3.29** (unchanged — the metric is scale-invariant) |
| Pure broadband noise, max over 8 seeds | **2.80 – 3.21** |

Two consequences:

1. `kDefaultThreshold = 10.0` (plan Task 11) and the spec's suggested `8–15×`
   **can never fire**. The analyzer is a no-op detector as specified.
2. Worse, the tone and the noise floor **are not separable at all**. A 1 kHz
   tone 26 dB above the noise scores 3.29; pure noise routinely scores 3.0+.
   There is no threshold that satisfies both of the plan's Task 11 tests.

### Proposed fix (needs a spec decision — I did not implement it)

Move the neighbourhood **outside** the main lobe. With neighbours at `±3..±6`,
the same rig, same window, same FFT:

| Signal | peakiness at `±3..±6` |
|---|---|
| 1 kHz tone + noise (4 seeds) | **152.8 – 160.4** |
| Pure broadband noise (4 seeds) | **3.51 – 5.50** |

That is a ~30× separation and the plan's `10.0` threshold works exactly as
written, unchanged. The defect is the **radius**, not the threshold constant.

I did **not** change `kNeighbourRadius`, because spec §5.2 step 4 states `±2`
explicitly and authority order is spec > plan > brief. This needs a spec
amendment, not an implementer's judgement call.

## Files

| File | Change | Lines |
|---|---|---|
| `src/dsp/PeakinessAnalyzer.h` | created | +96 |
| `src/dsp/PeakinessAnalyzer.cpp` | created | +170 |
| `tests/test_peakiness.cpp` | created | +509 |
| `CMakeLists.txt` | modified | +2 |
| `tests/CMakeLists.txt` | modified | +2 |

## Test results

```
100% tests passed, 0 tests failed out of 38
Total Test time (real) =   0.55 sec
```

38 = 27 existing + **11** new. The brief specified 10; I added an 11th
(`MinimumBinFollowsTheSpectrumSampleRate`) because success criterion 3 — "the
minimum bin is derived from `spectrum.sampleRate`, not hardcoded" — had no test
covering it. It scores the same bin-6 spike at 48 kHz (excluded) and 96 kHz
(included), which a hardcoded bin index cannot pass.

Both targets build clean, no compiler warnings on the new translation units.
The app target produced `AZ Soundtech Hands-free.exe` (7,622,144 bytes).

## Deviations from the brief

1. **Build directory.** The prescribed scratchpad path
   (`.../2dded5f4-.../scratchpad/build-task11`) **cannot be configured** — MSVC
   FileTracker fails on it:
   ```
   FileTracker : error FTK1011: could not create the new file tracking log file:
   ...\build-task11\external\JUCE\tools\CMakeFiles\CMakeScratch\TryCompile-1junmz\
   cmTC_53123.dir\Debug\cmTC_53123.tlog\link-cvtres.write.1.tlog.
   The system cannot find the path specified.
   ```
   That is the Windows `MAX_PATH` (260 char) limit; the generated tlog path is
   ~250 characters before JUCE adds its own nesting. I used
   `C:/Users/id_az/AppData/Local/Temp/t11` instead. This still satisfies the
   actual requirement — an isolated directory, never the repo's `build/`.

2. **Test 2 asserts the defect instead of the brief's numbers.** The brief asks
   for `peakiness > 10.0f` and exactly one candidate. See the headline finding.
   The test is renamed
   `ToneInNoiseIsNotDetectedBecauseTheHannMainLobeCapsPeakinessAtFour` and now
   asserts what is measurably true: the tone bin is a clean local maximum at
   984.375 Hz, its peakiness is in `(3.2, 4.0)`, and `analyse` at the default
   threshold returns **zero** candidates. Lowering the threshold to 3.2 does
   surface bin 21, which proves the selection machinery is correct and isolates
   the radius as the fault. **When the neighbourhood is widened, this test will
   fail loudly** — which is the point.

3. **Test 4's numbers are replaced.** The brief proposes flat 1.0 with
   `bin 21 = 40.0`, `bin 22 = 30.0`, claiming "both bins clear the threshold".
   They do not — adjacent bins are each other's neighbours (arithmetic below),
   and that spectrum yields **zero** candidates, proving nothing about local
   maxima. Replaced with `12.0 / 11.0` at a threshold of `2.0`, plus an exact
   two-bin plateau case exercising the `>` / `>=` asymmetry.

## Self-review

### Which tests did you watch FAIL before implementing, and what was the failure message?

First red run: **8 of 11 failed**. Verbatim excerpts against the stub
(`analyse` → `{nullptr, 0}`, `peakinessAt` → `0.0f`):

```
[ RUN      ] Peakiness.PeakinessAtMatchesSpecFormula
test_peakiness.cpp(127): error: Expected equality of these values:
  PeakinessAnalyzer::peakinessAt (mags.data(), Detector::kNumBins, 10)
    Which is: 0
  5.0f
    Which is: 5

[ RUN      ] Peakiness.ToneInNoiseIsNotDetected...
test_peakiness.cpp(187): error: Expected: (tonePeakiness) > (3.2f), actual: 0 vs 3.2
test_peakiness.cpp(200): error: Value of: containsBin (analyzer.analyse (spectrum), kToneBin)
  Actual: false
Expected: true

[ RUN      ] Peakiness.AdjacentBinsCollapseToOneCandidate
test_peakiness.cpp(261): error: The difference between
  PeakinessAnalyzer::peakinessAt (mags.data(), Detector::kNumBins, 21)
  and 48.0f / 14.0f is 3.4285714626312256, which exceeds 1e-5f

[ RUN      ] Peakiness.KeepsTheStrongestWhenOverCapacity
test_peakiness.cpp(435): error: Expected equality of these values:
  result.count
    Which is: 0
  static_cast<std::size_t> (PeakinessAnalyzer::kMaxCandidates)
    Which is: 32

[ RUN      ] Peakiness.ThresholdIsExclusive
test_peakiness.cpp(469): error: Expected equality of these values:
  PeakinessAnalyzer::peakinessAt (mags.data(), Detector::kNumBins, 200)
    Which is: 0
  10.0f
    Which is: 10
```

**Three tests PASSED against the stub and were therefore worthless**:
`BroadbandNoiseProducesNoCandidate`, `NullSpectrumProducesNoCandidate`,
`SilenceProducesNoCandidateAndNoNaN` — all three assert "count == 0", which a
stub returning nothing satisfies trivially. I added a **positive control** to
each before implementing:

- noise test: `EXPECT_GT (maxPeakiness (...), 2.0f)` — proves `peakinessAt`
  actually computes something;
- null test: the same magnitudes at a *valid* rate must yield the candidate;
- silence test: a flat-1.0 spectrum with a 40.0 spike must return exactly 40.0.

Second red run: **11 of 11 failed.** Only then did I write the implementation.
Green run: 11/11 pass, 38/38 overall.

### Show the arithmetic behind the expected value in tests 1, 4 and 10

**Test 1** — flat 1.0, `mag[10] = 5.0`. Neighbours are bins 8, 9, 11, 12:
```
mean      = (1 + 1 + 1 + 1) / 4 = 4.0 / 4 = 1.0     (exact in binary float)
peakiness = 5.0 / 1.0 = 5.0                          (exact)
```
Also asserted: the value is **not** `5.0/1.8 = 2.78`, which is what including
the centre bin would give — pinning the "centre excluded" requirement.

**Test 4** — the brief's proposed 40.0 / 30.0 ridge, worked out:
```
peakiness(21) = 40 / ((1 + 1 + 30 + 1)/4) = 40 / 8.25  = 4.85   (< 10, rejected)
peakiness(22) = 30 / ((1 + 40 + 1 + 1)/4) = 30 / 10.75 = 2.79   (< 10, rejected)
```
Both below threshold → count 0, so the brief's `count == 1` assertion fails.
Moreover **no** two-bin ridge can put both bins over 10. With background `e`:
```
peakiness(21) * peakiness(22) = [4a/(b+3e)] * [4b/(a+3e)]  ->  16  as e -> 0
```
so the two values cannot both exceed `sqrt(16) = 4.0`. Replaced with:
```
a = mag[21] = 12.0, b = mag[22] = 11.0, background 1.0
peakiness(21) = 4*12 / (11 + 3) = 48/14 = 3.4286  > 2.0  -> qualifies
peakiness(22) = 4*11 / (12 + 3) = 44/15 = 2.9333  > 2.0  -> qualifies
local max 21: 12 > 1 and 12 >= 11  -> kept
local max 22: 11 > 12 is false     -> rejected      => count == 1, bin 21
```
Plateau case (`mag[21] == mag[22] == 12.0`):
```
peakiness(both) = 4*12 / (12 + 3) = 48/15 = 3.2 > 2.0
local max 21: 12 > 1 and 12 >= 12 -> kept
local max 22: 12 > 12 is false    -> rejected      => count == 1, bin 21
```

**Test 10** — flat 1.0, `mag[200] = 10.0`:
```
mean      = (1 + 1 + 1 + 1) / 4 = 1.0     (exact)
peakiness = 10.0 / 1.0 = 10.0             (exact; 10.0f is representable)
10.0f > 10.0f  ->  false                  => NOT a candidate
```
Then `mag[200] = 10.001f` → `10.001 > 10.0` → count 1, bin 200. Asserting both
sides proves the boundary is where it is claimed, not that everything is
rejected.

### Did test 3 hold across more than one RNG seed? Which seeds?

Yes. The C++ test loops over **10 seeds**: `1, 7, 42, 99, 2024, 12345, 31337,
65535, 424242, 999983`. All give `count == 0` and `maxPeakiness < 4.0`.

Independently, before writing any C++, I swept **8 seeds** in a
double-precision reference model (`1, 7, 42, 12345, 2024, 999, 31337, 5`):
noise maxima were 2.802, 2.937, 2.848, 2.822, 2.965, 2.991, 3.061, 3.210.

**But it passes vacuously, and that is the real finding.** Nothing clears 10.0,
so "no false positive" is free. The honest number is the overlap: noise reaches
**3.21** while a genuine 1 kHz tone reaches only **3.29**. That 2.5% margin is
not a working discriminator. I did not paper this over by picking a friendly
seed — the seed list is deliberately wide and the noise ceiling is asserted.

### Is `analyse` allocation-free after construction? Name every call it makes.

Yes. `candidates_` is a `std::array<Candidate, 32>` sized in the header; the
method writes into it and returns a pointer plus a count. Complete call list:

| Call | Allocates? |
|---|---|
| `std::ceil` (once) | no — libm scalar |
| `static_cast` conversions | no |
| `peakinessAt` (static, per surviving bin) | no — reads 5 floats, `std::isfinite` |
| `std::isfinite` | no — bit inspection |
| `std::swap (Candidate&, Candidate&)` | no — `Candidate` is a trivially-copyable POD (`int`, `double`, 3×`float`), so this is a register/stack shuffle |
| `Candidate candidate;` (local) | no — stack, aggregate of scalars |

No `std::vector`, no `std::sort` (which can allocate for large ranges), no
`std::partial_sort`, no logging, no locks. The insertion sort is bounded at 32
elements. Worst case is O(numBins + candidates × 32).

## Concerns

1. **The headline finding above is the concern.** As specified, this class is a
   detector that detects nothing. Everything downstream (Task 12 harmonic
   scoring, Task 13 notch set/clear, Task 14 auto-release) consumes candidates
   that will never be produced. **Tasks 12–14 should not start until the
   `kNeighbourRadius` question is settled**, or they will be built and "tested"
   against an always-empty candidate list.

2. **I did not change the spec's constants**, so the committed default is a
   threshold that cannot fire. That is deliberate — silently substituting a
   working value would have buried a spec defect inside an implementation
   detail — but it does mean `main` will carry a non-functional default until
   someone rules on it. The `KNOWN DEFECT` block at the top of
   `PeakinessAnalyzer.h` documents this in place.

3. **Test 2's threshold of 3.2 is a magic number I chose**, derived from the
   measured 3.283–3.301 range. It is only used to demonstrate that the
   selection machinery works; it is not a proposal for a production threshold
   (at 3.2 the analyzer would also fire on ~1–2 noise bins per frame).

4. **`peakinessAt` takes `numBins` but `analyse` always passes
   `Detector::kNumBins`.** The parameter exists per the brief's fixed
   signature. It is honoured (edge bins return 0), but nothing currently passes
   anything else, so the non-513 path is exercised only by tests.

5. **Untested at other sample rates end-to-end.** The min-bin derivation is
   unit-tested at 48 kHz and 96 kHz with hand-built spectra, but no test drives
   the real `Detector` at 44.1/88.2/96 kHz. The Hann ceiling argument is
   rate-independent, so I do not expect surprises, but I did not verify it.

6. **The `>=`-on-the-right plateau rule biases toward the lower bin.** For a
   tone sitting exactly between two bins the reported frequency is ~47 Hz low
   at 48 kHz. That is within a typical notch bandwidth, but Task 13 may want
   parabolic interpolation across the three bins to recover the true peak
   frequency. Out of scope here; flagging it.

7. **Nothing in this task validates against real acoustic feedback.** Real
   howls drift in frequency within a 21 ms window, which smears the peak and
   pushes peakiness *lower* than the synthetic figures above — so the real-world
   picture is worse than measured, not better.

---

# Addendum: the defect is FIXED (annulus that excludes the Hann main lobe)

- **Status:** `DONE`
- **Branch:** `feat/task-11-peakiness` (unchanged)
- **Change:** `fix: score peakiness against an annulus that excludes the Hann main lobe`
- **Tests:** `100% tests passed, 0 tests failed out of 40` (was 38; two tests added)

## What changed

`kNeighbourRadius = 2` is gone. The neighbourhood is now an **annulus**:

```
kMainLobeRadius       = 2   // offsets 0, +-1, +-2 EXCLUDED -- this is the tone
kNeighbourInnerRadius = 3   // = kMainLobeRadius + 1
kNeighbourOuterRadius = 5
kNeighbourCount       = 6   // bins at -5,-4,-3,+3,+4,+5
```

`kDefaultThreshold` stays at `10.0f`. The radius was the defect, not the constant.

The edge guard and `analyse`'s first scored bin now use the OUTER radius:
valid bins are `[5, kNumBins-1-5] = [5, 507]`, and
`firstBin = max(5, ceil(minFrequencyHz_ / binWidth))`. The `!(mean > 0.0f)`
NaN-catching guard and the `std::isfinite` check are untouched. The
local-maximum rule, the descending ordering, the keep-the-strongest eviction
and the allocation-free discipline are untouched.

## Verification: reproduced, not trusted

Independent double-precision reference model (pure-Python radix-2 FFT, JUCE's
symmetric Hann, same 48 kHz / N=1024 / analysis-window geometry the C++ rig
uses). Hann main-lobe shape of an on-bin tone, normalised to peak:

```
offset:   -5      -4      -3      -2      -1       0      +1      +2      +3
value: 0.00004 0.00007 0.00012 0.00033 0.50073 1.00000 0.50073 0.00033 0.00012
```

Confirms the brief: bins +-1 carry half the peak, bins +-3 and beyond carry
~0.01% -- the annulus is measuring background, not tone.

Old solid-+-2 formula, re-measured: on-bin tone **3.992**, 1 kHz off-bin tone
**3.29**, broadband noise **2.72-3.46** over 10 seeds. Noise *outscores* the
tone. Ceiling 4.0 confirmed.

Annulus comparison, 50 noise seeds, worst noise bin **restricted to local
maxima** (which is what `analyse` actually tests -- the previous sweep did not
restrict, and so overstated the noise floor):

| annulus | 1 kHz tone | worst noise local max / 50 seeds | ratio | low-freq floor |
|---|---|---|---|---|
| +-1..+-2 (spec) | 3.29 | 4.45 | **0.7x** | 93.8 Hz |
| +-3..+-4 | 100.3 | **10.37 - crosses 10.0, FALSE POSITIVE (seed 39)** | 9.7x | 187.5 Hz |
| **+-3..+-5** | **124.4** | **7.20** | **17.3x** | **234.4 Hz** |
| +-3..+-6 | 152.9 | 6.23 | 24.6x | 281.3 Hz |

Then in the real rig (`Detector` + JUCE FFT, `std::mt19937`):

- 1 kHz tone at 26 dB SNR, seed 12345: peakiness **131.70** at bin 21 (984.375 Hz),
  exactly one candidate at the default 10.0 threshold.
- Same seed, tone removed: worst bin **4.51**, zero candidates.
- **Noise-only sweep, seeds 1..60: zero false candidates on every seed.**
  Worst upper-bound peakiness **7.35** (seed 59), next **7.23** (seed 14),
  minimum 3.12. The ten seeds pinned in the test (`1, 7, 42, 99, 2024, 12345,
  31337, 65535, 424242, 999983`) span 3.19-5.14.

## Where the brief's numbers were optimistic

The brief's table gave the +-3..+-5 worst noise bin as 5.75 and a "23x margin".
Over 50-60 seeds the worst is **7.20-7.35**, so real headroom under the 10.0
threshold is about **1.4x**, not 1.7x, and tone/noise separation is ~17-29x
depending on seed. The conclusion is unchanged and in fact *strengthened*:
+-3..+-4 does not merely get "uncomfortably close" to 10.0 -- over 50 seeds it
**crosses** it. +-3..+-5 is the right choice; its headroom is just thinner than
advertised, which is why the test asserts `worst < 9.0` and the header warns
against narrowing the annulus.

## Known v1 limitation (documented in the header and pinned by a test)

`kNeighbourOuterRadius = 5` forces the lowest scoreable bin to 5:

| rate | binWidth | lowest scoreable bin | frequency |
|---|---|---|---|
| 44.1 kHz | 43.07 Hz | 5 | **215.3 Hz** |
| 48 kHz | 46.875 Hz | 5 | **234.4 Hz** |
| 96 kHz | 93.75 Hz | 5 | **468.8 Hz** |

`kDefaultMinFrequencyHz = 100.0` is therefore **never the binding constraint**
-- the radius is. The detector is blind below ~234 Hz at 48 kHz. Low-mid
feedback at 200-250 Hz (stage wash, floor-coupled wedges) is a real live-sound
failure mode, so this is a genuine coverage gap for v1, not a rounding detail.
`Peakiness.LowestScoreableBinIsSetByTheOuterRadiusNotMinFrequency` asserts it
directly: a 40x spike at 187.5 Hz yields nothing, the same spike at 234.375 Hz
yields a candidate.

**Rejected alternative -- one-sided (upper-only) annulus near the low edge.**
It would extend coverage to bin 0, and it is *not* implemented: the
low-frequency noise floor of live sound rises toward DC, so averaging only the
bins *above* a low bin systematically underestimates its background and
inflates its peakiness -- more false positives exactly where the product's
headline goal is fewer. Closing the gap properly needs a longer FFT (finer
bins) or a per-bin noise-floor tracker. Out of scope for Task 11; it should be
raised for v2.

## Test changes

13 peakiness tests (was 11), 40 total (was 38).

| Test | What changed |
|---|---|
| `PeakinessAtUsesAnAnnulusThatExcludesTheMainLobe` | renamed from `...MatchesSpecFormula`. Annulus of bin 10 is `{5,6,7,13,14,15}` -> mean 1.0, peakiness 5.0. **New:** loading bins 8,9,11,12 to 3.0 must not move the result by one ulp (old formula would have given 5/3 = 1.67); raising all six annulus bins to 2.0 must halve it to 2.5. |
| `ToneInNoiseIsDetectedAndNoiseAloneIsNot` | replaces `...NotDetectedBecauseTheHannMainLobeCapsPeakinessAtFour`. Real separation test at the **default** 10.0: tone -> exactly one candidate at bin 21 / 984.375 Hz, peakiness 131.70; same RNG with the tone removed -> zero. Asserts `tone > 10 * worstNoiseBin`. |
| `BroadbandNoiseProducesNoCandidate` | same 10 seeds, no longer vacuous (test 2 proves the analyzer does fire). Upper bound tightened from 4.0 to **9.0** on the measured 7.35 worst case. |
| `AdjacentBinsCollapseToOneCandidate` | rebuilt. Under the annulus, bins 21 and 22 are no longer each other's neighbours, so the brief's original `40.0 / 30.0` ridge now works as originally proposed: both score 40 and 30 against a flat 1.0 background, both clear the **default** 10.0, and only the local-max rule collapses them. Exact-plateau case kept. |
| `BinsBelowMinFrequencyAreIgnored` | arithmetic redone: `annulus(6) = {1,2,3,9,10,11}`; `firstBin = max(5, 11) = 11` excludes bin 6, `max(5, 3) = 5` includes it. |
| `LowestScoreableBinIsSetByTheOuterRadiusNotMinFrequency` | **NEW.** Pins the 234.375 Hz blind spot. |
| `MinimumBinFollowsTheSpectrumSampleRate` | kept as required. 96 kHz: `max(5, ceil(500/93.75)=6) = 6` -> bin 6 scored; 48 kHz: `max(5, 11) = 11` -> excluded. |
| `BinsWithoutAFullAnnulusScoreZero` | **NEW.** Split out of the silence test and given teeth: on a *live* 1.0 floor, bins -5..4 and 508..513 return 0 while bins 5 and 507 return 1.0, so a 0 can only mean "no full annulus". |
| `SilenceProducesNoCandidateAndNoNaN` | unchanged behaviour; comments updated to the six-bin annulus. |
| `KeepsTheStrongestWhenOverCapacity` | spacing changed 5 -> 6. At spacing 5 each spike would sit **inside** its neighbour's annulus and `peakiness == height` would no longer hold. Tallest is now bin 100+6*35 = **310** (height 55), weakest survivor bin **124** (height 24), dropped bins 100/106/112/118. A `static_assert` pins `kSpacing > kNeighbourOuterRadius`. |
| `CandidatesAreOrderedByDescendingPeakiness`, `NullSpectrumProducesNoCandidate`, `ThresholdIsExclusive` | expected values unchanged (spikes are >=50 bins apart / isolated, and a flat-1.0 annulus still means 1.0), comments updated. |

## Build and test

Built at `C:/Users/id_az/AppData/Local/Temp/t11` (the repo `build/` was not
used; the scratchpad path still hits Windows MAX_PATH under MSVC FileTracker).
Full `ctest -C Debug`:

```
100% tests passed, 0 tests failed out of 40
Total Test time (real) =   0.86 sec
```

No new compiler warnings on `PeakinessAnalyzer.cpp` or `test_peakiness.cpp`.
The two `C4324` warnings in the build output are pre-existing, from
`LockFreeRingBuffer.h` in the app target.

## Concerns retired

Concerns 1 and 2 of the original report ("the analyzer detects nothing" /
"main will carry a threshold that cannot fire") are **resolved**. Tasks 12-14
are unblocked. Concern 3 (the magic 3.2 threshold) is gone with the test that
used it. Concerns 4, 5, 6 and 7 still stand.

**New concern:** headroom is 1.4x, not the ~2x a short seed list suggests. Any
future change to the window, the FFT length or the hop must re-run the seed
sweep before trusting `kDefaultThreshold`.
