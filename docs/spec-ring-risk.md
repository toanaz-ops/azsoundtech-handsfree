# Spec — RING RISK readout

**Status:** UI built and shipped; **data source not implemented.**
**Owner of the remaining work:** DSP / detector side.
**Raised:** 2026-08-25, with the console rebuild.

---

## What exists today

The analyser toolbar carries a RING RISK readout at its left, beside the
section name. It is fully laid out, painted, and covered by a test.

```cpp
// src/gui/SpectrumView.h
enum class RingRisk { Unavailable, Low, Rising, Critical };

// Polled once per frame by the same timer that refreshes the plot.
// Null means Unavailable. Must not block: it runs on the message thread.
std::function<RingRisk()> ringRiskProvider;
```

`ringRiskProvider` is **null**. Nothing assigns it. The readout therefore
renders `Unavailable`: a hollow outlined chip reading `N/A`, in the tertiary
text colour, with no fill.

That is the deliberate resting state, and it is the part of this feature that
must not regress:

> An unwired risk indicator that reads "low" is worse than one that reads
> "n/a", because a soundman would act on it.

`MainComponent.RingRiskReadsUnavailableUntilSomethingProvidesIt` in
`tests/test_gui_wiring.cpp` pins that. Do not "fix" it by defaulting to `Low`.

## What is missing, and why the GUI cannot supply it

`NotchController::SnapshotBuffer` publishes magnitudes, placed notches, a
sample rate and a sequence number. It publishes **no score of how close the
room is to ringing**.

The GUI could compute something peakiness-shaped from the magnitudes it
already receives. It deliberately does not, because that would put a **second,
independently-computed** peakiness number on screen next to the one the
detector actually acts on. The two would disagree — different window, different
smoothing, different frame — and the operator would have no way to know which
one the filters were following. A readout that disagrees with the machine it
describes is worse than no readout.

So the number has to come from the detector that owns it.

## Proposed contract

### 1. Publish a score

The detector already computes everything needed. `PeakinessAnalyzer::Result`
carries `Candidate`s with a `peakiness` and a `score`, and
`PeakinessAnalyzer::getThreshold()` is the line a candidate must cross.
`CandidateScorer::scoreCandidate` then produces the confidence that decides
whether a notch is placed at all.

Add to `SnapshotBuffer`:

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

Publish both under the existing snapshot mutex, in the same
`publishSnapshot`-equivalent step that already writes `magnitudes` and
`notchCount`. No new lock, no new thread.

### 2. Map score to state — in the GUI, not the DSP

The DSP publishes a **number**; the GUI decides what counts as alarming, the
same way it already decides what a notch's age looks like. Proposed banding,
expressed against the live threshold so it tracks the RESPONSE preset instead
of being three more magic constants:

| State | Condition |
|---|---|
| `Unavailable` | `! ringRiskValid` |
| `Low` | `score < 0.55 × threshold` |
| `Rising` | `0.55 × threshold ≤ score < threshold` |
| `Critical` | `score ≥ threshold` |

`Critical` therefore means "the detector is about to place, or has just
placed, a notch" — which is exactly what the operator wants a second's warning
of. Publish the threshold in the snapshot too, or expose it through the
controller; do not hardcode `10.0` in the GUI.

### 3. Hysteresis is required, not optional

At 30 fps a raw comparison will flicker between two bands on any borderline
signal, and a flickering warning indicator is noise a soundman learns to
ignore. Require either:

- a hold time — a state may not step **down** for 750 ms after stepping up; or
- split thresholds — rise at the figures above, fall at 0.85 × them.

Either is fine. Pick one and note it in the code.

### 4. Wiring, once the score exists

One line in `MainComponent`'s constructor, alongside the other provider
lambdas, reading the controller of the **currently monitored slot** (see
`setDisplayedSlot`) — not slot 0:

```cpp
spectrumView_.ringRiskProvider = [this]
{
    NotchController::SnapshotBuffer snapshot {};
    notchControllers_[(std::size_t) displayedSlot_]->copySnapshot (snapshot);
    return gui::SpectrumView::riskForScore (snapshot);   // to be written
};
```

## Acceptance

1. With the detector stopped, or before it has history, the chip reads `N/A`
   and is hollow. The existing test still passes unchanged.
2. Feeding the rig a 1 kHz tone at a level that provokes a notch drives the
   chip to `Critical` **before** the notch appears in ACTIVE NOTCHES, not
   after. A warning that arrives with the fix is not a warning.
3. Broadband noise at a realistic stage level holds `Low`.
   `tests/test_peakiness.cpp` already establishes that noise-only bins peak at
   ~7.35 against a 10.0 threshold, so the ~0.74 ratio must land in `Low`,
   which the 0.55 band boundary above allows. If it does not, the band is
   wrong, not the test.
4. A borderline signal does not visibly flicker between two states.
5. A human listens on a real rig before this is called done, per `CLAUDE.md`.

## Out of scope

- Per-slot risk shown for **all** slots at once. The readout describes the
  monitored slot only; the masthead selector says which that is.
- Any change to placement behaviour. This feature is a readout. If wiring it
  up changes when a notch gets placed, something is wrong.
