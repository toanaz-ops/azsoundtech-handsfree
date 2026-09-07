# Task 3 report — `ScoreBreakdown::riseRatio`

**Status:** DONE. Commit `7a279f6` on `claude_desk/lane-g-brainstorm-sdd-f3c568`.
**Expected level change:** 0 dB. Nothing reads the field; `score` verified byte-identical by
the product-identity test plus 470 untouched pre-existing tests.

## Implemented

Exactly the brief, one path and one extra output.

- `src/dsp/CandidateScorer.h` — `float riseRatio = 1.0f;` inserted into `ScoreBreakdown`
  between `mNorm` and `penalty`, with the brief's comment block verbatim (why rNorm's
  saturation at rise 1.5 is not enough, and why 1.0 is the conservative neutral).
- `src/dsp/CandidateScorer.cpp:80` — `out.riseRatio = 1.0f;` in the `historyCount_ == 0` branch.
- `src/dsp/CandidateScorer.cpp:106` — `out.riseRatio = rise;` immediately after `rise` is
  computed, **before** the `(rise - 1)/0.5` normalisation and its clamp.
- The "history too young" branch and the sub-threshold early return (`CandidateScorer.cpp:52`)
  are untouched: `out.riseRatio` keeps its 1.0 default there, which is the documented neutral.

Not touched: `score`, `pNorm`, `rNorm`, `mNorm`, `penalty`, any threshold, any clamp. The
`std::max (reference[bin], 1e-12f)` divisor floor is still in place, so `riseRatio` inherits
the same inf/NaN protection `rNorm` has.

## Deviation from the brief (real file won)

The brief's Step 1 code for tests 2 and 3 does not work against the real `Detector`.
`Detector::processLatestBlock` (`src/dsp/Detector.cpp:67`) reads **at most one hop** per call,
so a single `tap.write` + one `processLatestBlock` leaves the 2048-point analysis window
three-quarters stale and `analyse()` returns `count == 0`. Both tests failed on
`ASSERT_GT (detected.count, 0u)`, in the setup, before reaching any assertion about `riseRatio`.

Fix: both tests now loop four write/`processLatestBlock` calls to fill the window, and score
against the last spectrum. The assertions are unchanged, and the property each test exists for
is preserved:

- `RiseRatioIsNeutralWithoutUsableHistory` still never calls `commitBlock`, so `historyCount_`
  is genuinely 0 at the moment of scoring — the no-history branch is the one under test.
- `ScoreStaysTheProductOfTheSameFourFactors` still warms 66 quiet hops through `feedHops`
  first, so real history exists and the reference-frame path is the one exercised.

Test 1 was correct as written in the brief and is unmodified.

## Tests

Three new tests appended to `tests/test_candidatescorer.cpp`, using the file's existing
`Rig`, `makeToneInNoise()`, `kHop`, `kFrameMs`, `kSampleRate` — no new helpers.

1. `CandidateScorer.RiseRatioIsTheRawRatioWhileRNormStaysSaturated` — 66 quiet hops, then a
   hard tone start; finds a candidate with `refFrame != nullptr && rNorm >= 1.0f` and asserts
   `rNorm == 1.0f` **and** `riseRatio > 2.0f`. This is the discriminating test: it is the only
   one that can tell the raw ratio from the saturated axis.
2. `CandidateScorer.RiseRatioIsNeutralWithoutUsableHistory` — no `commitBlock` ever;
   `riseRatio == 1.0f`.
3. `CandidateScorer.ScoreStaysTheProductOfTheSameFourFactors` — `score == pNorm * rNorm *
   mNorm * penalty`, and `scoreCandidate(...) == breakdown.score` (one arithmetic path, not two).

### TDD evidence

**RED** — tests written first, build against the unmodified header:

```
tests\test_candidatescorer.cpp(605,5): error C2039: 'riseRatio': is not a member of
    'CandidateScorer::ScoreBreakdown'
tests\test_candidatescorer.cpp(627,5): error C2039: 'riseRatio': is not a member of
    'CandidateScorer::ScoreBreakdown'
```

**RED (second, real)** — after adding the field, tests 2 and 3 still failed, on the setup
defect described above, which is what exposed the brief/`Detector` mismatch:

```
[ RUN      ] CandidateScorer.RiseRatioIsNeutralWithoutUsableHistory
tests\test_candidatescorer.cpp(624): error: Expected: (detected.count) > (0u), actual: 0 vs 0
[ RUN      ] CandidateScorer.ScoreStaysTheProductOfTheSameFourFactors
tests\test_candidatescorer.cpp(647): error: Expected: (detected.count) > (0u), actual: 0 vs 0
 2 FAILED TESTS
```

**GREEN** — focused:

```
build/tests/Release/HandsFreeTests.exe --gtest_filter=CandidateScorer*
[==========] 13 tests from 2 test suites ran. (157 ms total)
[  PASSED  ] 13 tests.
```

**GREEN** — full gate:

```
cd build && ctest -C Release
100% tests passed, 0 tests failed out of 473
Total Test time (real) =  40.86 sec
```

473 = the 470 baseline + these 3. No pre-existing scorer or controller test changed result,
which is the real proof the field is additive.

**No new warnings.** Release build emits 0 errors; the surviving warnings are all pre-existing
and in other files (`LockFreeRingBuffer.h` C4324 padding, `NotchListPanel.cpp` C4244 float→int).
Nothing new from `CandidateScorer.h/.cpp` or the test file.

## Self-review

**A test that passes with `riseRatio` hard-coded to 1.0 is not a test — checked by mutation.**
Replaced `out.riseRatio = rise;` with `out.riseRatio = 1.0f;`, rebuilt, ran:

```
[  FAILED  ] CandidateScorer.RiseRatioIsTheRawRatioWhileRNormStaysSaturated
 1 FAILED TEST   (12 passed)
```

The mutant is killed by exactly the test that is supposed to kill it. The implementation was
restored from a backup and re-verified (`grep` shows both assignments back at lines 80 and 106)
before the full suite and the commit.

Honest note on the other two: tests 2 and 3 would both survive that mutation. That is by
design — test 2 locks the neutral-value contract (it cannot distinguish "assigned 1.0" from
"defaulted to 1.0", and does not need to), and test 3 is a no-regression guard on the
arithmetic. Test 1 carries the discriminating weight alone.

**Struct-layout safety.** Inserting a field mid-struct would silently break any positional
aggregate initialisation. Grepped every user: the only `ScoreBreakdown` outside
`CandidateScorer.*` and its test is `src/app/NotchController.h:368`, a plain data member with
no braced init. Nothing to break.

**Commit hygiene.** `rm -f .superpowers/sdd/.gitignore` ran before staging; `git add` used the
three explicit paths; `git diff --cached --name-only` confirmed exactly those three; `git status`
after the commit shows `.superpowers/sdd/2026-09-07-gain-aware-notch/` still untracked — nothing
under `.superpowers/` was committed.

## Concerns for the next task

1. **No test covers the "history exists but every frame is too young" branch.** `riseRatio`
   reports the neutral 1.0 there by falling through to the default rather than by an explicit
   assignment (the brief's deliberate choice, and the comment documents it). It is correct as
   read, but it is correct by omission — a future refactor that initialises the field to
   anything else would change that branch with no test to catch it. Cheap to add if Task 5
   wants belt and braces.
2. **The sub-threshold early return also yields 1.0** (`CandidateScorer.cpp:52` returns before
   any axis is computed). Same shape of risk, same lack of coverage. Both are conservative
   values for a `riseRatio >= 2.0` gate, so nothing unsafe reaches the placement policy — but
   Task 5 should read `riseRatio` only alongside a `score > 0` candidate, not standalone.
3. **`riseRatio` is unbounded above.** Against a near-silent reference the divisor floors at
   `1e-12`, so the ratio can reach ~1e12 legitimately. It is finite (no inf, no NaN — that is
   what the floor buys), but Task 5 must compare it against `kSteepRiseRatio` rather than scale
   it into anything.

## Fix round 1

Two review findings, both comment-only — no code token changed in either file.

**1. (Important) `ScoreStaysTheProductOfTheSameFourFactors` kept as-is, comment rewritten.**
The assertion (`b.score == b.pNorm*b.rNorm*b.mNorm*b.penalty`) mirrors `CandidateScorer.cpp:148`
by construction and cannot, by itself, detect a numeric drift introduced by this diff — it holds
before and after any such drift as long as `score` is still literally that product. The old "RED
IF the new field changes the arithmetic" comment overstated what the test can catch. Replaced it
in `tests/test_candidatescorer.cpp` (above the test, lines 638-649) with a comment that says what
the test DOES prove (one shared arithmetic path between `scoreCandidate()` and
`scoreCandidateDetailed()`, `riseRatio` as a read-only extra output riding alongside, never a
fifth multiplied factor) and what it does NOT prove (score unchanged vs. before this diff — that
evidence is the pre-existing 470-test regression suite with hand-computed expectations, untouched
by this task). The "RED IF" line now names the two things that would actually fail the test: the
two entry points diverging, or `riseRatio` becoming a real factor. Test name, body, and assertions
are unchanged.

**2. (Minor) `riseRatio` header doc — third default-1.0 source added.** The doc comment on
`ScoreBreakdown::riseRatio` (`src/dsp/CandidateScorer.h`) named the no-history and
history-too-young branches as the sources of the neutral 1.0 but omitted the third: the
sub-threshold early return at `CandidateScorer.cpp:52-53` (`candidate.peakiness <=
kDefaultThreshold -> return out`), which also leaves `riseRatio` at its initializer before any
axis is computed. Added that clause. Also added a one-line note that the value is unbounded above
(the `1e-12` divisor floor means a near-silent reference can push the ratio to ~1e12) and that
callers must compare it against a threshold, never scale by it — reinforcing concern 3 from the
original report directly at the field's declaration site.

### Commands and output

Build (header touched, full incremental rebuild via cmake):
```
cmake --build build --config Release
```
Result: build succeeded. Only pre-existing warnings in unrelated files (`NotchListPanel.cpp`
C4244, `MainComponent.cpp` C4996) — nothing new from `CandidateScorer.h/.cpp` or the test file.

Focused filter:
```
build/tests/Release/HandsFreeTests.exe --gtest_filter=CandidateScorer*
[==========] 13 tests from 2 test suites ran. (160 ms total)
[  PASSED  ] 13 tests.
```

Full gate:
```
cd build && ctest -C Release
100% tests passed, 0 tests failed out of 473
Total Test time (real) =  39.68 sec
```

### Diff summary

```
 src/dsp/CandidateScorer.h      | 11 ++++++++---
 tests/test_candidatescorer.cpp | 15 +++++++++++----
 2 files changed, 21 insertions(+), 5 deletions(-)
```
Both hunks are comment text only — confirmed by inspecting `git diff` before staging: no
non-comment line in either file changed.

### Commit

`rm -f .superpowers/sdd/.gitignore` ran before staging (repo policy: that file must not exist
tracked or untracked at commit time). `git add tests/test_candidatescorer.cpp
src/dsp/CandidateScorer.h`, `git diff --cached --name-only` confirmed exactly those two paths,
then:

```
git commit -m "docs(dsp): riseRatio -- name every default-1.0 path; say what the one-path test proves"
```

Commit `d892b3c` on `claude_desk/lane-g-brainstorm-sdd-f3c568`.
