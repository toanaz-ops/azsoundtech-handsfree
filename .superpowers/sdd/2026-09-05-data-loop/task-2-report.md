# Task 2 report: CandidateScorer::ScoreBreakdown

## What was implemented

Added `CandidateScorer::ScoreBreakdown` and `scoreCandidateDetailed()` exactly
as specified in `task-2-brief.md`, verbatim:

- `src/dsp/CandidateScorer.h:77-91` — new `ScoreBreakdown` struct (rawPeakiness,
  pNorm/rNorm/mNorm, penalty, score, refFrame, refAgeMs) and the
  `scoreCandidateDetailed()` declaration, with the brief's comment preserved.
- `src/dsp/CandidateScorer.cpp:34-150` — `scoreCandidate()` now delegates to
  `scoreCandidateDetailed(...).score`; the old body was renamed into
  `scoreCandidateDetailed()`, filling `out.*` fields alongside each existing
  local (`out.pNorm`, `out.rNorm`, `out.mNorm`, `out.penalty`), recording
  `out.refFrame` / `out.refAgeMs` at the point axis 2 picks its reference
  frame, and computing `out.score = out.pNorm * out.rNorm * out.mNorm *
  out.penalty` — same four operands, same order, same types
  (`out.mNorm` holds `static_cast<float>(mNorm)`) as the original
  `pNorm * rNorm * static_cast<float>(mNorm) * penalty`. No arithmetic,
  clamp, or the 0.45 x riseReference history scan was touched — only field
  writes were added alongside the existing locals.
- `tests/test_candidatescorer.cpp` — appended the brief's two tests verbatim
  under a new `CandidateScorerBreakdown` test suite.

## TDD evidence

### RED — `scoreCandidateDetailed` not yet declared

Command:
```
cmake --build build --config Release --target HandsFreeTests
```
Relevant output (after appending the tests, before touching the header/cpp):
```
tests\test_candidatescorer.cpp(433,27): error C2039: 'scoreCandidateDetailed': is not a member of 'CandidateScorer'
      src\dsp\CandidateScorer.h(31,7): see declaration of 'CandidateScorer'
```
This is the expected RED for a new API: the member doesn't exist yet, so the
call site fails to compile (with cascading `const object must be initialized`
/ EXPECT-macro overload-resolution errors on the same undeclared `b`, all
downstream of the same root cause).

### GREEN — implementation in place

Command:
```
cmake --build build --config Release --target HandsFreeTests
```
Result: clean build, `HandsFreeTests.vcxproj -> ...\HandsFreeTests.exe`, no
errors.

Focused run:
```
cd build && ctest -C Release --output-on-failure -R CandidateScorer
```
Output:
```
100% tests passed, 0 tests failed out of 9
```
including the two new tests:
```
Test #342: CandidateScorerBreakdown.DetailedScoreIsBitExactWithTheFloatOverloadAndItsAxesMultiplyToIt ... Passed
Test #343: CandidateScorerBreakdown.RefFrameIsTheNewestFrameAtLeastFortyFivePercentOfRiseReferenceOld ... Passed
```

### Full suite

Command:
```
cd build && ctest -C Release
```
Output:
```
100% tests passed, 0 tests failed out of 414
```
(408 pre-existing per the brief's estimate + Task 1's SessionLogger suite that
had already brought the count to 412, plus these 2 new tests = 414, matching
the brief's expectation.)

## Files changed

- `src/dsp/CandidateScorer.h`
- `src/dsp/CandidateScorer.cpp`
- `tests/test_candidatescorer.cpp`

Commit: `ca390d4` — "feat(scorer): scoreCandidateDetailed exposes the axes and
the reference frame"

## Self-review

- Completeness vs brief: struct fields, method signature, delegation pattern,
  and both tests match the brief verbatim. Nothing extra was added (no new
  helpers, no refactors beyond the rename+field-capture the brief specifies).
- Names: `ScoreBreakdown`, `scoreCandidateDetailed`, field names all taken
  directly from the brief — clear and consistent with existing `pNorm`/
  `rNorm`/`mNorm`/`penalty` naming in the file.
- No over-building: did not touch `NotchController` or any caller (out of
  scope per the task description — Task 3/4 will wire it in).
- Tests verify real behaviour: test 5 drives the real Detector +
  PeakinessAnalyzer + CandidateScorer pipeline for 60 real cycles before
  comparing `scoreCandidate()` against `scoreCandidateDetailed().score` on
  live candidates (bit-exact `EXPECT_EQ`, not `EXPECT_FLOAT_EQ`), and checks
  the four-axis product independently. Test 6 hand-constructs a history ring
  with known ages (0/180/20 ms elapsed -> stamps at 0/180/200, clock at 300)
  to pin down exactly which frame `refFrame`/`refAgeMs` must point at, with
  `EXPECT_DOUBLE_EQ` on `refAgeMs` justified per the ambiguity resolution
  (both sides are sums of the same double literals, so it's exact — verified
  by the test passing).
- Test output pristine: rebuilt `CandidateScorer.cpp` and
  `test_candidatescorer.cpp` from a touched state and grepped the build log
  for `warning|CandidateScorer` — only the two "building this file" lines
  appeared, no compiler warnings from either file.

## Concerns

None. The change is header/struct-plus-refactor only: `scoreCandidate()`'s
observable behaviour (return value) is provably identical to before, verified
both by inspection (identical operand list, same order, same static_cast
placement) and by the bit-exact `EXPECT_EQ` test comparing the two entry
points against live pipeline data across every candidate in a real frame.
Expected level change: 0 dB, as stated in the task — confirmed, since no
arithmetic changed.
