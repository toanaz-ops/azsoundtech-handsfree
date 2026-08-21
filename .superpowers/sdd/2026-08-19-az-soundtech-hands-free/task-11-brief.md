# Task 11: Peakiness scoring

## Context you need before you start

The detector FFT stage (Task 10) landed in commit `fe126cb`. It produces a
`Detector::Spectrum` — read `src/dsp/Detector.h` in full before writing
anything. Two facts from it bind this task:

- `Spectrum::magnitudes` is `nullptr` when the tap had no new audio. You must
  handle that without dereferencing.
- `Spectrum::readCount` ranges **0..kHopSize (512)**, not 0..kFftSize. It is a
  timeline signal for Task 14, not a validity signal — a short hop still yields
  a perfectly good spectrum. Do NOT reject a spectrum because `readCount` is
  small.

Authority order if anything below conflicts with something else: **spec > plan >
this brief**. The last two briefs in this project each contained errors that a
subagent would have coded straight into a bug. If you find one here, stop and
say so rather than implementing something you believe is wrong.

- Spec §5.2 step 4: `docs/superpowers/specs/2026-08-19-az-soundtech-hands-free-design.md`
- Plan Task 11: `docs/superpowers/plans/2026-08-19-az-soundtech-hands-free.md`

## What the spec and plan actually require

Spec §5.2 step 4: *"Với mỗi bin ≥ 100 Hz: … **Peakiness** = `bin_mag / mean(neighbors ±2 bins)`. Feedback = peakiness > threshold (vd 8–15×)"*

Plan Task 11:
```cpp
double peakiness = binMag / mean(neighbors ± 2 bins);
if (peakiness > 10.0) candidateScore += 0.5;
```
Plan test: *"Synthetic signal: 1kHz tone in white noise → expect detection. Broadband noise only → no false positive."*

Note that "neighbors ±2 bins" means the **four** bins `i-2, i-1, i+1, i+2`. The
centre bin is NOT part of its own neighbourhood — including it would drag every
peakiness value toward 1 and make the 10x threshold meaningless.

## Bin geometry (verified, use these numbers in test comments)

At 48 kHz with kFftSize = 1024, bin width = 48000/1024 = **46.875 Hz**.

| Quantity | Value |
|---|---|
| 100 Hz | bin 2.133 → first scoreable bin is **3** |
| 300 Hz | bin 6.4 |
| 500 Hz | bin 10.667 |
| 1000 Hz | bin 21.333 → nearest bin **21** (984.375 Hz), bin 22 = 1031.25 Hz |

The minimum bin MUST be derived from `spectrum.sampleRate`, never hardcoded to
3 — the app supports 44.1/48/88.2/96 kHz and Task 9 exists precisely because
someone hardcoded a rate once already.

## Files

- Create: `src/dsp/PeakinessAnalyzer.h`
- Create: `src/dsp/PeakinessAnalyzer.cpp`
- Create: `tests/test_peakiness.cpp`
- Modify: `tests/CMakeLists.txt` — add `test_peakiness.cpp` and
  `../src/dsp/PeakinessAnalyzer.cpp` to the existing `HandsFreeTests` target.
  Do NOT create a second target and do NOT touch the GoogleTest or juce_dsp
  setup, both of which are already correct.
- Modify: `CMakeLists.txt` (root) — add `src/dsp/PeakinessAnalyzer.{h,cpp}` to
  the `HandsFree` target sources, alongside the existing `Detector` entries.

## Class interface (exact)

```cpp
class PeakinessAnalyzer
{
public:
    static constexpr int    kNeighbourRadius        = 2;      // spec: neighbours +-2
    static constexpr double kDefaultMinFrequencyHz  = 100.0;  // spec 5.2 step 4
    static constexpr float  kDefaultThreshold       = 10.0f;  // plan Task 11
    static constexpr float  kCandidateScore         = 0.5f;   // plan Task 11
    static constexpr int    kMaxCandidates          = 32;

    struct Candidate
    {
        int    bin         = 0;
        double frequencyHz = 0.0;   // bin * sampleRate / Detector::kFftSize
        float  magnitude   = 0.0f;
        float  peakiness   = 0.0f;
        float  score       = 0.0f;  // kCandidateScore; Tasks 12+ adjust it
    };

    struct Result
    {
        const Candidate* candidates = nullptr;  // valid until the next analyse()
        std::size_t      count      = 0;
    };

    PeakinessAnalyzer();

    void   setThreshold (float peakinessThreshold);
    float  getThreshold() const;
    void   setMinFrequencyHz (double hz);
    double getMinFrequencyHz() const;

    Result analyse (const Detector::Spectrum& spectrum);

    // Peakiness of one bin against its +-kNeighbourRadius neighbours.
    // Returns 0.0f when `bin` is too close to either end for a full
    // neighbourhood, or when the neighbourhood mean is zero.
    // Exposed because Task 14's auto-release must re-test a locked notch's bin
    // without re-running candidate selection.
    static float peakinessAt (const float* magnitudes, int numBins, int bin);

private:
    float  threshold_;
    double minFrequencyHz_;
    std::array<Candidate, kMaxCandidates> candidates_;  // pre-allocated
};
```

## Behaviour of `analyse` (exact)

1. If `spectrum.magnitudes == nullptr` or `spectrum.sampleRate <= 0.0`, return
   `{nullptr, 0}`. No dereference, no crash.
2. Compute the first scoreable bin as
   `max(kNeighbourRadius, ceil(minFrequencyHz_ / binWidth))` where
   `binWidth = spectrum.sampleRate / Detector::kFftSize`. The last scoreable bin
   is `Detector::kNumBins - 1 - kNeighbourRadius`. Bins outside that span have
   no full neighbourhood and are skipped.
3. For each bin in that span, a bin is a candidate only if BOTH hold:
   - it is a **local maximum**: `mag[i] > mag[i-1] && mag[i] >= mag[i+1]`
   - its peakiness is **strictly greater than** `threshold_`
4. Fill in `frequencyHz = bin * spectrum.sampleRate / Detector::kFftSize`,
   `magnitude`, `peakiness`, and `score = kCandidateScore`.
5. Return the candidates ordered by **descending peakiness**.
6. If more than `kMaxCandidates` bins qualify, keep the `kMaxCandidates`
   **highest-peakiness** ones. Do NOT simply stop at the first 32 bins scanned —
   that would silently drop the strongest feedback whenever it sits at a high
   bin index.

### Why the local-maximum rule is mandatory, not decoration

A 1 kHz tone at 48 kHz lands at bin 21.33, i.e. between bins. Both bin 21 and
bin 22 will show high peakiness. Without the local-max rule the analyzer emits
two candidates for one tone, and Task 13 would place two notches 47 Hz apart
for a single howl — burning two of the sixteen available notches and widening
the hole in the response. The rule uses `>` on the left and `>=` on the right so
that an exact plateau of two equal bins still collapses to one candidate.

### Divide-by-zero

`peakinessAt` must return `0.0f` when the neighbourhood mean is zero. Silence
gives an all-zero spectrum; `0/0` would be NaN, and NaN compares false against
every threshold, so it would not *look* broken while quietly poisoning any
later arithmetic that touches it. Return a real number.

## Allocation discipline

The detector thread is NOT the audio thread, so this code is not required to be
hard real-time. Pre-allocate anyway: `candidates_` is a fixed `std::array` sized
in the header, `analyse` writes into it and returns a pointer plus a count.
No `std::vector` growth, no sorting container built per call. `std::partial_sort`
or an insertion into the fixed array is fine — both are allocation-free.

## Tests (tests/test_peakiness.cpp)

`Detector::Spectrum` is a plain struct holding a `const float*`. That means most
of these tests can build a spectrum **by hand** from a local
`std::vector<float>` of `Detector::kNumBins` magnitudes, with no FFT and no ring
buffer involved. Use that for the arithmetic tests — it makes the expected
values exact instead of approximate. Use the real `Detector` only where the test
is genuinely about end-to-end behaviour (tests 1 and 2).

Before writing each assertion, compute the expected number by hand and put the
computation in the test comment. An assertion nobody worked out in advance
proves nothing.

1. `Peakiness.PeakinessAtMatchesSpecFormula` — hand-built array, all bins 1.0
   except bin 10 = 5.0. Neighbours of bin 10 are bins 8, 9, 11, 12, all 1.0, so
   mean = 1.0 and peakiness = 5.0/1.0 = **exactly 5.0**. Assert that.

2. `Peakiness.ToneInNoiseIsDetected` — plan's required test. Drive the real
   `Detector` from a `LockFreeRingBuffer<float>`: a 1 kHz sine at amplitude 1.0
   plus white noise at amplitude 0.05, 48 kHz, at least `4 * Detector::kFftSize`
   samples. Use `std::mt19937` with a **fixed seed** — a test that fails one run
   in fifty is worse than no test. Pump the detector until the history is primed
   (see `Detector`'s hop semantics), then analyse. Assert: exactly one candidate,
   `bin == 21`, `frequencyHz` within 1 Hz of 984.375, `peakiness > 10.0f`.

3. `Peakiness.BroadbandNoiseProducesNoCandidate` — plan's required test. Same
   rig, white noise only, fixed seed. Assert `count == 0`. If this test is
   flaky across seeds, that is a real finding about the threshold — report it,
   do not paper over it by picking a friendly seed and staying quiet.

4. `Peakiness.AdjacentBinsCollapseToOneCandidate` — hand-built spectrum with a
   two-bin ridge (e.g. flat 1.0, bin 21 = 40.0, bin 22 = 30.0). Both bins clear
   the threshold, but only bin 21 is a local maximum. Assert `count == 1` and
   `candidates[0].bin == 21`.

5. `Peakiness.BinsBelowMinFrequencyAreIgnored` — hand-built spectrum with a
   strong spike at bin 6 (= 281.25 Hz at 48 kHz). With
   `setMinFrequencyHz(500.0)` (bin 10.67, so first scored bin is 11) assert
   `count == 0`. Then `setMinFrequencyHz(100.0)` and assert the same spectrum
   now yields the bin-6 candidate. This tests the knob, not just the default.

6. `Peakiness.NullSpectrumProducesNoCandidate` — a default-constructed
   `Detector::Spectrum` (magnitudes nullptr). Assert `count == 0` and no crash.

7. `Peakiness.SilenceProducesNoCandidateAndNoNaN` — all-zero magnitudes. Assert
   `count == 0`, and assert `std::isfinite(PeakinessAnalyzer::peakinessAt(...))`
   for a mid-range bin.

8. `Peakiness.CandidatesAreOrderedByDescendingPeakiness` — hand-built spectrum
   with two isolated spikes of different heights. Assert `count == 2` and
   `candidates[0].peakiness > candidates[1].peakiness`, and that
   `candidates[0].bin` is the taller spike's bin.

9. `Peakiness.KeepsTheStrongestWhenOverCapacity` — hand-built spectrum with
   `kMaxCandidates + 4` isolated spikes, arranged so the **tallest ones sit at
   the highest bin indices**. Assert `count == kMaxCandidates` and that the
   tallest spike is present in the result. This is the test that catches a
   naive "stop scanning at 32" implementation.

10. `Peakiness.ThresholdIsExclusive` — hand-built spectrum where one bin's
    peakiness is exactly the threshold. Assert it is NOT a candidate (spec and
    plan both say `> threshold`). Construct it so the value is exactly
    representable: flat 1.0 neighbours and a spike of exactly 10.0 gives
    peakiness exactly 10.0f.

## Build and test

Use a build directory of your own so you do not race other agents working in
this repo:

```
cmake -B "C:/Users/id_az/AppData/Local/Temp/claude/D--DEV-CAVE-EP3-PROJECT005-AZ-handsfree/2dded5f4-ffa2-42a0-b637-e29e82dd0995/scratchpad/build-task11" -G "Visual Studio 18 2026" -A x64
cmake --build "<that dir>" --config Release
cd "<that dir>" && ctest -C Release --output-on-failure
```

The first configure+build compiles JUCE and may take 10+ minutes. That is
normal; let it finish. An ASIO-SDK CMake WARNING is expected (the SDK is
withheld for licensing) and is not your problem.

Target state: **37/37 tests pass** (27 existing + 10 new), and the `HandsFree`
app target still builds.

## Method

Follow TDD. Write the tests first against a stub whose `analyse` returns
`{nullptr, 0}` and whose `peakinessAt` returns `0.0f`, run them, and confirm
they fail for the right reason before writing the real body. If a test passes
against the stub, that test is not testing anything — fix the test.

## Commits

One commit: `feat: add peakiness scoring with tests`

## Success criteria

1. `PeakinessAnalyzer.{h,cpp}` exist with the exact interface above.
2. Peakiness is `binMag / mean(4 neighbours at +-1, +-2)`, centre bin excluded.
3. The minimum bin is derived from `spectrum.sampleRate`, not hardcoded.
4. Local-maximum filtering collapses an off-bin tone to a single candidate.
5. Over-capacity keeps the strongest, not the first.
6. No NaN or infinity escapes `peakinessAt` for any input.
7. 10 new tests pass; 37/37 total; both targets build.

## Report contract

- **Model used:** exact model string
- Status: DONE | DONE_WITH_CONCERNS | NEEDS_CONTEXT | BLOCKED
- Files with line deltas
- Full ctest summary line
- Commit hash
- Self-review, each answered with evidence not assertion:
  - Which tests did you watch FAIL before implementing, and what was the failure message?
  - Show the arithmetic behind the expected value in tests 1, 4 and 10.
  - Did test 3 (noise → no candidate) hold across more than one RNG seed? Say
    which seeds you tried.
  - Is `analyse` allocation-free after construction? Name every call it makes.
- Concerns — anything you had to guess at, anything in this brief you think is
  wrong, anything you implemented that you are not confident in. An empty
  concerns section on a task this size will be read as "did not look".
