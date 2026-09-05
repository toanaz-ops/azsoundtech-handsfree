# Spec — RING RISK readout

**Status:** **implemented (lane R, 2026-09-06), chờ kiểm chứng rig.**
The score is published by the detector (R1), banded and held by the GUI (R2),
and wired to the monitored slot (R3). Acceptance 1 and 4 are proven by tests;
2, 3 and 5 need a real rig and a human — they are in the 1.1.3 tester note, not
claimed done here.
**Raised:** 2026-08-25, with the console rebuild.
**Amended:** by the "Amendment lane R — 2026-09-06" block in
`.superpowers/sdd/2026-08-27-next-wave/task-R3-brief.md`, which governs where it
and this file disagree. Sections 1-3 below are rewritten to match it.

---

## What exists today

The analyser toolbar carries a RING RISK readout at its RIGHT-hand end, past
the RANGE controls. It is fully laid out, painted, and covered by a test.

```cpp
// src/gui/SpectrumView.h
enum class RingRisk { Unavailable, Low, Rising, Critical };

// Polled once per frame by the same timer that refreshes the plot.
// Null means Unavailable. Must not block: it runs on the message thread.
std::function<RingRisk()> ringRiskProvider;
```

`ringRiskProvider` is assigned in `MainComponent`'s constructor (section 4).
With nothing to report — detection disarmed, no scorer history yet, or a slot
just switched to — the readout renders `Unavailable`: a hollow outlined chip
reading `N/A`, in the tertiary text colour, with no fill.

That is the deliberate resting state, and it is the part of this feature that
must not regress:

> An unwired risk indicator that reads "low" is worse than one that reads
> "n/a", because a soundman would act on it.

`MainComponent.RingRiskReadsUnavailableUntilSomethingProvidesIt` in
`tests/test_gui_wiring.cpp` pins that. Do not "fix" it by defaulting to `Low`.

## What exists, and why the GUI does not supply the number itself

`NotchController::SnapshotBuffer` publishes magnitudes, placed notches, a
sample rate, a sequence number — and, since R1, `ringRiskScore`,
`ringRiskValid` and `ringRiskThreshold`.

The GUI could compute something peakiness-shaped from the magnitudes it
already receives. It deliberately does not, because that would put a **second,
independently-computed** peakiness number on screen next to the one the
detector actually acts on. The two would disagree — different window, different
smoothing, different frame — and the operator would have no way to know which
one the filters were following. A readout that disagrees with the machine it
describes is worse than no readout.

So the number has to come from the detector that owns it.

## The contract, as built

### 1. Publish a score

Placement uses `CandidateScorer::scoreCandidateDetailed` and then
`score = breakdown.score * asym` (`NotchController.cpp`), compared against
`CandidateScorer::kConfirmScore`. `ringRiskScore` is the **max of that same
`score`** over every candidate of every analysed lane of the slot (A-R1, A-R8)
— nothing is recomputed, and there is one readout per slot, not one per lane.

`SnapshotBuffer` carries:

```cpp
// The highest candidate confidence seen in the frame this snapshot describes,
// 0.0 when no bin was scoreable. Same value the placement decision uses --
// NOT a second measure of the same thing.
float ringRiskScore = 0.0f;

// False until the detector has enough history to score at all (the first few
// blocks after a start, a device change, or a sample-rate change). The GUI
// renders Unavailable while this is false, rather than rendering 0.0 as "low".
bool ringRiskValid = false;
```

Published under the existing snapshot mutex, in the same step that writes
`magnitudes` and `notchCount` — with that step **moved below the detection
pass** (A-R2), because before the pass there is no score for the frame yet. No
new lock, no new thread. The notch list is still gathered **before** the frame
is scored, which is what makes acceptance 2 possible: a notch this frame places
appears in the NEXT snapshot, one hop after the chip went Critical.

`ringRiskValid` is true only when detection is armed **and** at least one active
lane's scorer has committed >= 1 block of history since the last reset — start,
device change, sample-rate change, or `setWidth` (A-R4). Detection off means
`N/A`, never a reassuring `Low`.

### 2. Map score to state — in the GUI, not the DSP

The DSP publishes a **number**; the GUI decides what counts as alarming, the
same way it already decides what a notch's age looks like.

**Units (A-R3).** The score is not a peakiness. It is `CandidateScorer`'s
placement confidence: a product of three 0..1 axes times a penalty and the
asymmetry multiplier, so it lives in **0..1**. The band line is therefore
`CandidateScorer::kConfirmScore` (0.7) — the same line placement compares
against — published as `ringRiskThreshold` so the GUI hardcodes nothing. This
spec's original figures (`PeakinessAnalyzer` 5..20, default 10.0) were the wrong
scale: `score >= 10.0` can never happen.

| State | Condition |
|---|---|
| `Unavailable` | `! ringRiskValid`, or a NaN score, or a non-finite / <= 0 threshold |
| `Low` | `score < 0.55 × ringRiskThreshold` |
| `Rising` | `0.55 × ringRiskThreshold ≤ score < ringRiskThreshold` |
| `Critical` | `score ≥ ringRiskThreshold` |

`Critical` therefore means "the detector is about to place, or has just
placed, a notch" — which is exactly what the operator wants a second's warning
of. `gui::SpectrumView::riskForScore()` is that function, and it is pure: it
reads the three ring-risk fields of one snapshot and nothing else.

### 3. Hysteresis is required, not optional

At 30 fps a raw comparison will flicker between two bands on any borderline
signal, and a flickering warning indicator is noise a soundman learns to
ignore. Of the two shapes originally offered, **the HOLD is the one chosen**
(A-R5), in `SpectrumView::RingRiskHysteresis`:

- a step **up** is immediate, and re-arms the hold;
- a step **down** may not happen within `kHoldMs` = **750 ms** of the last frame
  that observed the level at or above the held state — a frame *equal* to the
  held band re-arms it too, otherwise the chip blinks down for one frame roughly
  every 750 ms while a score sits on a band edge;
- `Unavailable` overrides an in-flight hold outright: once the detector stops
  scoring, showing a stale `Critical` would be inventing data.

Why the hold rather than split thresholds: the bands are already fractions of a
live threshold, so a second set of fall fractions would be a second thing to
keep in step with the DSP. Time is a parameter, not a clock the object owns, so
the 750 ms rule is testable with no wall-clock sleep.

### 4. Wiring (task R3, done)

In `MainComponent`'s constructor, alongside the other provider lambdas, reading
the controller of the **currently monitored slot** (see `setDisplayedSlot`) —
not slot 0. The band returned here is RAW; `timerCallback` runs it through the
750 ms hold before anything is painted, so the lambda must not hold as well:

```cpp
spectrumView_.ringRiskProvider = [this]
{
    NotchController::SnapshotBuffer snapshot {};
    notchControllers_[(std::size_t) displayedSlot_]->copySnapshot (snapshot);
    return gui::SpectrumView::riskForScore (snapshot);
};
```

A fresh `copySnapshot` per tick is deliberate (A-R6): ~8 KB at 30 fps off one
uncontended mutex, against coupling the readout to the plot's refresh order.
Not optimised before anything has measured a problem.

## Acceptance

1. With the detector stopped, or before it has history, the chip reads `N/A`
   and is hollow. The existing test still passes unchanged. **PROVEN**
   (`MainComponent.RingRiskReadsUnavailableUntilSomethingProvidesIt`, plus
   `NotchControllerRingRisk.InvalidBeforeAnyFrame` /
   `DetectionDisabledPublishesInvalidNotZero`).
2. Feeding the rig a 1 kHz tone at a level that provokes a notch drives the
   chip to `Critical` **before** the notch appears in ACTIVE NOTCHES, not
   after. A warning that arrives with the fix is not a warning. **NEEDS THE
   RIG** — the ordering is built (the notch list is gathered before the frame
   is scored, section 1) and a headless test proves the chip reaches `Critical`
   off a real detector
   (`MainComponent.RingRiskGoesCriticalWhenTheMonitoredSlotsDetectorConfirms`),
   but "one frame earlier, on a real room" is a human observation.
3. Broadband noise at a realistic stage level holds `Low`. Restated per A-R3,
   because the old wording used peakiness units: `CandidateScorer` gates on
   peakiness before it scores at all, so a noise-only frame scores **exactly
   0**, which is `Low` — not `Unavailable`, because the detector really did
   measure it. `NotchControllerRingRisk.NoiseOnlyFramesAreValidAndReadLow`
   pins that with `NoiseSource`. **NEEDS THE RIG** for "at a realistic stage
   level": a real room's noise floor is not the test's.
4. A borderline signal does not visibly flicker between two states. **PROVEN**
   (`SpectrumView.RingRiskDoesNotFlickerAcrossManyHoldWindows` and the rest of
   the `SpectrumView.RingRisk*` hold tests).
5. A human listens on a real rig before this is called done, per `CLAUDE.md`.
   **NEEDS THE RIG.** Nothing in lane R touches the audio path — expected level
   change **0 dB** — so per the 2026-08-27 owner decision the listen happens in
   alpha; see `docs/release-notes/1.1.3-alpha.md`.

## Known gaps (owner)

- `NotchController::setSampleRate` has **no production caller** (found in R1).
  A device or sample-rate change reaches the controller only through
  `setWidth`, which happens to reset the same state. It is exercised by tests
  and by `tools/snapshot.cpp` only. Nothing in lane R changed it — the ring-risk
  reset path rides on `setWidth` like everything else — but a reader of
  `setSampleRate` would reasonably assume the device path calls it, and it does
  not.
- A-R7: `CandidateScorer.cpp` gates on `PeakinessAnalyzer::kDefaultThreshold`
  (compile-time) rather than the live `analyzer.getThreshold()`, so the RESPONSE
  preset does not move the scorer's gate. Out of scope for lane R — changing it
  changes placement behaviour — and waiting on an owner decision.

## Out of scope

- Per-slot risk shown for **all** slots at once. The readout describes the
  monitored slot only; the masthead selector says which that is.
- Any change to placement behaviour. This feature is a readout. If wiring it
  up changes when a notch gets placed, something is wrong.
