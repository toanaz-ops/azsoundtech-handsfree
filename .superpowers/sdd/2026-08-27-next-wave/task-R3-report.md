# Task R3 report — wiring + acceptance (lane R, RING RISK)

**Branch:** `claude_desk/merge-branches-subagent-c76c7f`
**Commits:** `4342ec4` (feat), `767d9d7` (chore/snapshot), `bdc6265` (docs)
**Suite:** 451 → **453/453**, `ctest -C Release`.
**Level change: 0 dB.** No file under `src/app/NotchController*` or `src/dsp/`
was touched — verified below.

---

## 1. What I implemented

### The wiring (`src/app/MainComponent.cpp`, ctor)

Spec §4 / A-R6 verbatim, placed next to the other provider lambdas:

```cpp
spectrumView_.ringRiskProvider = [this]
{
    NotchController::SnapshotBuffer snapshot {};
    notchControllers_[(std::size_t) displayedSlot_]->copySnapshot (snapshot);
    return gui::SpectrumView::riskForScore (snapshot);
};
```

The comment above it records three things a reader would otherwise have to
re-derive: the number comes from the detector and never from a GUI-side
peakiness; the band returned is RAW because `timerCallback` already applies the
750 ms hold (holding here would hold twice); and the fresh `copySnapshot` per
tick is the deliberate A-R6 trade, not an oversight.

### One test accessor (`src/gui/SpectrumView.h`)

`void tickForTest() { timerCallback(); }`. `juce::Timer` is a **private** base of
`SpectrumView`, so `static_cast<juce::Timer&>(view).timerCallback()` does not
compile (`error C2243: conversion ... exists, but is inaccessible`) — that was
the first RED. The headless suite pumps no message loop, so without this a test
could only assert on a hand-set `ringRisk_`, which would prove nothing about the
wiring. The accessor runs the real `timerCallback()`.

### Three tests (`tests/test_gui_wiring.cpp`)

| Test | Covers |
|---|---|
| `MainComponent.RingRiskReadsUnavailableUntilSomethingProvidesIt` | (a) the pinned test, **assertion unchanged** |
| `MainComponent.RingRiskGoesCriticalWhenTheMonitoredSlotsDetectorConfirms` | (b) a real howl on the monitored slot lights the chip |
| `MainComponent.RingRiskFollowsTheMonitoredSlotAcrossASwitch` | (c) switching to a quiet slot drops the old slot's Critical |

**About the pinned test.** Its assertion is untouched and it still passes with
the provider wired, for the reason R1 built in: an idle controller has scored
nothing, so it publishes `ringRiskValid == false`, and `riskForScore` reports
that as `Unavailable` rather than as a reassuring `Low`. I added two sentences
to its comment saying so, and left the NAME alone because it is pinned by the
spec and by the brief. **Suggested rename for a later pass, not taken here:**
`RingRiskReadsUnavailableUntilTheDetectorHasHistory` — the current name now
describes a condition ("until something provides it") that is permanently
satisfied, which will mislead the next reader.

**How the drive works, and why it costs a real sleep.** `MainComponent`'s
controllers run on `JuceMonotonicClock`; there is no injection point, unlike
`FakeClock` in `tests/test_notchcontroller.cpp`. `CandidateScorer`'s rise axis
compares against the newest history frame at least `0.45 × riseReferenceMs`
(~202 ms) old, and its clock advances only by the REAL gap between drained
blocks. A loop that pumps hops back to back therefore scores `rNorm = 0`
forever, whatever it feeds. The smallest honest shape is: one quiet frame (the
reference), one real `juce::Thread::sleep (260)`, then tone frames. The two
driving tests cost ~0.5 s each. This is documented in a header comment in the
test file so the next person does not "optimise" the sleep away.

## 2. TDD evidence

### RED (before the wiring, after the tests + `tickForTest`)

```
$ ./build/tests/Release/HandsFreeTests.exe --gtest_filter=MainComponent.RingRisk*
[ RUN      ] MainComponent.RingRiskReadsUnavailableUntilSomethingProvidesIt
[       OK ] MainComponent.RingRiskReadsUnavailableUntilSomethingProvidesIt (1257 ms)
[ RUN      ] MainComponent.RingRiskGoesCriticalWhenTheMonitoredSlotsDetectorConfirms
tests\test_gui_wiring.cpp(900): error: Expected equality of these values:
  app.getSpectrumViewForTest().getRingRisk()
    Which is: 4-byte object <00-00 00-00>
  gui::SpectrumView::RingRisk::Critical
    Which is: 4-byte object <03-00 00-00>
[  FAILED  ] MainComponent.RingRiskGoesCriticalWhenTheMonitoredSlotsDetectorConfirms (868 ms)
[ RUN      ] MainComponent.RingRiskFollowsTheMonitoredSlotAcrossASwitch
tests\test_gui_wiring.cpp(920): error: Expected equality of these values:
  ... same, Which is: 4-byte object <00-00 00-00> vs <03-00 00-00>
[  FAILED  ] MainComponent.RingRiskFollowsTheMonitoredSlotAcrossASwitch (1095 ms)
[  PASSED  ] 1 test.
[  FAILED  ] 2 tests
```

**RED for the right reason.** The `ASSERT_TRUE (snap.ringRiskValid)` and
`ASSERT_GE (snap.ringRiskScore, snap.ringRiskThreshold)` inside those tests
PASSED — the detector really did reach Critical off the real path. Only the chip
stayed at `Unavailable` (`<00-00 00-00>`), because `ringRiskProvider` was null.
A test that had failed on the snapshot assertion instead would have meant the
drive was broken, not the wiring.

### GREEN (after the wiring, nothing else changed)

```
$ ./build/tests/Release/HandsFreeTests.exe --gtest_filter=MainComponent.RingRisk*
[ RUN      ] MainComponent.RingRiskReadsUnavailableUntilSomethingProvidesIt
[       OK ] MainComponent.RingRiskReadsUnavailableUntilSomethingProvidesIt (445 ms)
[ RUN      ] MainComponent.RingRiskGoesCriticalWhenTheMonitoredSlotsDetectorConfirms
[       OK ] MainComponent.RingRiskGoesCriticalWhenTheMonitoredSlotsDetectorConfirms (511 ms)
[ RUN      ] MainComponent.RingRiskFollowsTheMonitoredSlotAcrossASwitch
[       OK ] MainComponent.RingRiskFollowsTheMonitoredSlotAcrossASwitch (491 ms)
[  PASSED  ] 3 tests.
```

### Full suite

```
$ cmake --build build --config Release
$ cd build && ctest -C Release
...
453/453 Test #453: logstats_fixture .......................   Passed    0.29 sec

100% tests passed, 0 tests failed out of 453

Total Test time (real) =  35.65 sec
```

Baseline was 451; +2 is exactly the two new tests. Run again after the docs and
tool commits: same 453/453.

## 3. Snapshots

```
$ cmake --build build --config Release --target HandsFreeSnapshot
$ build/tools/Release/HandsFreeSnapshot.exe shots 1440 920 --fast
wrote ...\shots\console-idle.png (1440 x 920)
console-live: RING RISK chip STAGED to Critical (not measured -- see the comment in tools/snapshot.cpp)
wrote ...\shots\console-live.png (1440 x 920)
```

I read both PNGs back.

**`shots/console-idle.png`** — right-hand end of the analyser toolbar, past the
RANGE fields: the label `RING RISK` in small caps, then a hollow outlined chip
reading **`N/A`** in dim/tertiary grey with no fill. Legible at 1440 px, the
text is centred and does not touch the box. This is the genuine reading: no
device, detection disarmed, nothing published. Everything else in the shot is
unchanged from the lane D baseline (WAITING FOR SIGNAL, NOTHING RINGING, the
routing table with slot 01 stereo/INDEP).

**`shots/console-live.png`** — same position, chip now filled and reading
**`CRITICAL`** in the theme's `danger` red on a dark red wash, with a red
border. The word fits its box with margin on both sides; it is the most visible
thing on the toolbar without competing with the trace. The rest of the shot is
the usual live composition: three notch stems (01 amber at 247 Hz, 03R dashed at
1.2 kHz on lane R, 02 at 1.9 kHz), the ACTIVE NOTCHES table with GOOD/FALSE/
unjudged rows.

**How console-live was seeded — read this before trusting the picture.** The
chip in the live shot is **STAGED, not measured**. The tool feeds a musical
composite with detection **disarmed**, so the wired chip's honest reading there
is `N/A` — identical to the idle shot, which would have shown the owner nothing
new. Lighting it for real would mean arming detection and feeding a howl with
real inter-block gaps (see §2), and the auto-placed notch that followed would
wreck the three staged notch ages the live shot exists to demonstrate. So the
displayed field is set through `ringRiskForTest()` — the accessor R2 added for
exactly this "put the component in a known on-screen state" job, i.e. an
EXISTING hook, no new infrastructure — immediately before the final shoot, and
the tool prints a line saying so. What the picture proves is what the chip LOOKS
like when lit: colour, contrast, and that `CRITICAL` fits its box. That the chip
reaches Critical from a real detector is proven by
`MainComponent.RingRiskGoesCriticalWhenTheMonitoredSlotsDetectorConfirms`, not
by this image.

## 4. Acceptance ledger (spec §Acceptance 1–5)

| # | Claim | Status | Evidence / who finishes it |
|---|---|---|---|
| 1 | Detector stopped or without history → `N/A`, hollow; the existing test passes unchanged | **PROVEN HERE** | `MainComponent.RingRiskReadsUnavailableUntilSomethingProvidesIt` (assertion untouched), `NotchControllerRingRisk.InvalidBeforeAnyFrame`, `...DetectionDisabledPublishesInvalidNotZero`; `console-idle.png` |
| 2 | A 1 kHz tone drives the chip Critical **before** the notch appears in ACTIVE NOTCHES | **NEEDS THE RIG** | The ordering is built (A-R2: the notch list is gathered before the frame is scored, so this frame's notch lands in the NEXT snapshot) and the Critical transition is tested off a real detector — but "one frame earlier, in a real room" is a human observation. In the 1.1.3 tester note, item 1. |
| 3 | Broadband noise holds `Low` | **PROVEN in unit form, NEEDS THE RIG for level** | Restated per A-R3: the scorer gates on peakiness before scoring, so a noise-only frame scores exactly 0 → `Low`, and `NotchControllerRingRisk.NoiseOnlyFramesAreValidAndReadLow` pins it. A real stage noise floor is not `NoiseSource`. Tester note, item 2. |
| 4 | A borderline signal does not visibly flicker | **PROVEN HERE** | `SpectrumView.RingRiskDoesNotFlickerAcrossManyHoldWindows`, `...FallsSevenFiftyAfterTheLastCriticalObservation`, `...StepUpRestartsTheHold`, `...UnavailableOverridesTheHold` (R2) |
| 5 | A human listens on a real rig | **NEEDS THE RIG** | 0 dB, so per the 2026-08-27 owner decision the listen happens in alpha. Tester note, "Cái gì KHÔNG đổi". |

2, 3 and 5 are written into `docs/release-notes/1.1.3-alpha.md` as things to
look at, not claimed done anywhere.

## 5. Files changed

| File | Commit | What |
|---|---|---|
| `src/app/MainComponent.cpp` | `4342ec4` | the provider lambda (+ comment) |
| `src/gui/SpectrumView.h` | `4342ec4` | `tickForTest()` |
| `tests/test_gui_wiring.cpp` | `4342ec4` | 2 new tests + drive helpers; pinned test's comment only |
| `tools/snapshot.cpp` | `767d9d7` | stage the chip lit for `console-live.png`, print that it is staged |
| `docs/spec-ring-risk.md` | `bdc6265` | status header, §1 "What exists", §2 A-R3 units/banding, §3 HOLD chosen, §4 done, acceptance restated, **Known gaps (owner)** |
| `docs/GIOI-THIEU.md` | `bdc6265` | the four states for a soundman |
| `docs/KY-THUAT-CHONG-HU.md` | `bdc6265` | new §3.6 data path; pending list drops the RING RISK item |
| `docs/release-notes/1.1.3-alpha.md` | `bdc6265` | new tester note |
| `memory/ring-risk-lane-r-2026-09-06.md`, `memory/MEMORY.md` | `bdc6265` | 8 lessons + index line |

Not touched: `src/app/NotchController.*`, anything under `src/dsp/`,
`installer/`, `Z:\...\TESTER-NOTES.md`. `release-alpha.ps1` was NOT run (A-R10).
`shots/` is gitignored and was not force-added. No `.superpowers/sdd/.gitignore`
existed; the five `review-*.diff` files and `progress.md` were left unstaged.

Confirm:

```
$ git diff 4342ec4~1..bdc6265 --stat -- src/app/NotchController.cpp src/app/NotchController.h src/dsp
(no output)
```

## 6. Self-review

- **Did the wiring change any placement decision?** No. The lambda only reads a
  snapshot copy. The detector's own code is byte-identical (diff above).
- **Does the pinned test still test what it was written to test?** Yes, and it
  now tests something stronger: with the provider wired, `Unavailable` is a
  measured "no data", not an unassigned `std::function`. Its assertion is
  unchanged. Its name is now slightly misleading — flagged, not changed.
- **Is `tickForTest()` a hole in the class?** It exposes a call that a timer
  would make anyway; it cannot start or stop the timer, and the private
  `juce::Timer` base is still private. The alternative was making `timerCallback`
  public or asserting on hand-set state.
- **Does the 260 ms sleep make the suite flaky?** It is a floor, not a window:
  the scorer needs history *at least* 202 ms old, and a slower machine makes the
  gap larger, not smaller. The drive then retries up to 40 tone frames. Nothing
  asserts an upper bound on time.
- **Did I re-read the images?** Yes, both, before writing §3.

## 7. Concerns / open

1. **The live snapshot's chip is staged.** Stated in the tool's stdout, in a
   comment in `tools/snapshot.cpp`, in its commit message, and in §3 above. If
   the controller wants an unstaged live shot, that is new tool infrastructure
   (arm detection, timed pump, accept the auto-placed notch) and was explicitly
   out of scope for R3.
2. **The pinned test's name outlived its meaning.** Rename proposed in §2; not
   done unilaterally because the name appears in the spec and in the brief.
3. **`NotchController::setSampleRate` has no production caller** (R1 finding).
   Now recorded under "Known gaps (owner)" in the spec. Untouched by lane R —
   the ring-risk reset rides on `setWidth` like everything else — but somebody
   should decide whether the device path ought to call it.
4. **A-R7** (`CandidateScorer` gating on the compile-time peakiness threshold
   rather than the live one) is recorded in the same section and still waits on
   an owner decision. It moves placement, so it stayed out of lane R.
5. **Release** is the controller's, after final review: patch 1.1.2 → 1.1.3
   (A-R10). `docs/release-notes/1.1.3-alpha.md` is written and ready to be
   folded into `TESTER-NOTES.md` at that point — I did not edit the drop folder.

## 8. Fix round 1 (reviewer: Important 1, Minor 3, Minor 4)

**What was wrong.** §4's row 1 and §6's second bullet both credited
`MainComponent.RingRiskReadsUnavailableUntilSomethingProvidesIt` with proving
the *wired* idle path. It does not: that test constructs `MainComponent` and
reads `getRingRisk()` without ever calling `tickForTest()`. The headless suite
pumps no message loop, so `timerCallback()` never runs on its own, and the
assertion is on `ringRisk_`'s default-constructed value — identical whether or
not `ringRiskProvider` is assigned at all. §6's claim "with the provider
wired, `Unavailable` is a measured 'no data', not an unassigned
`std::function`" was false for that test. `console-idle.png` was cited
alongside as if it corroborated the wiring; `tools/snapshot.cpp` also runs no
dispatch loop, so the `N/A` in that PNG is the same unpolled default field,
not a measured reading.

**What now proves it.** Added
`MainComponent.RingRiskReadsUnavailableWhenTheMonitoredDetectorHasNoHistory`
in `tests/test_gui_wiring.cpp`, right after the pinned test: constructs
`MainComponent` exactly like the pinned test, confirms the monitored
controller has no history yet (`ASSERT_FALSE (snap.ringRiskValid)`), calls
`tickForTest()` once to run the real `timerCallback()` through the wired
provider, then asserts `getRingRisk() == RingRisk::Unavailable`. Mutation
check: temporarily changed `SpectrumView::riskForScore`'s
`if (! snapshot.ringRiskValid) return RingRisk::Unavailable;` to
`return RingRisk::Low;` — the new test failed (and so did
`RingRiskFollowsTheMonitoredSlotAcrossASwitch`, corroborating), confirming it
actually exercises the guard:

```
[ RUN      ] MainComponent.RingRiskReadsUnavailableWhenTheMonitoredDetectorHasNoHistory
tests\test_gui_wiring.cpp(901): error: Expected equality of these values:
  app.getSpectrumViewForTest().getRingRisk()
    Which is: 4-byte object <01-00 00-00>
  gui::SpectrumView::RingRisk::Unavailable
    Which is: 4-byte object <00-00 00-00>
[  FAILED  ] MainComponent.RingRiskReadsUnavailableWhenTheMonitoredDetectorHasNoHistory
```

Reverted the mutation immediately after (`git diff -- src/` empty, confirmed).
The pinned test's name and assertion are untouched, per the controller
ruling — it stays cited for the thing it actually proves: the readout never
defaults to `Low` when nothing has assigned `ringRiskProvider`.
`docs/spec-ring-risk.md` Acceptance 1 now cites the new companion test for the
wired idle path and notes `console-idle.png` shows the default field, not a
polled one.

Full suite after the fix: 453 → **454/454**, `ctest -C Release`
(`build && ctest -C Release`).

**Minor 3** (CRITICAL wording overstates placement — a full notch table or a
guarded bin can hold the chip at Critical with nothing placed): softened one
sentence each in `docs/GIOI-THIEU.md` and `docs/release-notes/1.1.3-alpha.md`.

**Minor 4**: re-wrapped the `docs/KY-THUAT-CHONG-HU.md` pending-list line left
ragged after the RING RISK item was removed.
