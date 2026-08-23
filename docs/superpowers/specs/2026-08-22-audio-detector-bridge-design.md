# Design — the audio↔detector bridge

**Lane B of the parallel execution plan.** Written 2026-08-22 against `main`
@ `c93c771`.

**Status: DRAFT — awaiting owner approval.** No bridge code has been written.
This document exists to be argued with before anything is built on it.

**Scope.** The boundary between the audio thread and the detector thread: the
command queue, the notch model, the detector thread and its clock, and how the
spectrum and notch state reach the GUI. It unblocks plan Tasks 5, 12, 13, 14, 15
and 19.

**Inputs.** Owner decisions [D-05] (the detector adopts preset notches) and
[D-06] (auto-release timers freeze while the tap is dead), recorded in
`.superpowers/sdd/2026-08-19-az-soundtech-hands-free/owner-decisions.md`. The
GUI interface audit, `docs/superpowers/audits/2026-08-22-gui-interface-audit.md`.

---

## 1. The governing principle

The parallel execution plan describes four holes. They are not four problems.
They are one question — *who owns the notch state* — seen from four sides, and
[D-05] has already answered it: **the detector owns it.**

Everything below follows from that answer plus one rule:

> **Lock-free machinery is a cost paid for the audio thread's benefit. Paying it
> between two threads that are not real-time is complexity with no payer.**

The audio thread is real-time. The detector thread and the message thread are
not. So the design has exactly **two** lock-free channels, both touching the
audio thread, and uses an ordinary mutex everywhere else.

This is the single most consequential judgement in the document. The plan's own
wording for Task 19 — *"via thread-safe queue **or** atomic pointer swap"* —
assumes the GUI path needs lock-free machinery. It does not. The GUI reads from
the **detector**, never from the audio thread.

```
   message thread                 detector thread                audio thread
   ─────────────                  ───────────────                ────────────
                          mutex  ┌───────────────────┐
   GUI requests  ──────────────► │  NotchController  │
                                 │                   │
   GUI snapshot  ◄────────────── │  · NotchModel     │
                          mutex  │    (authoritative)│
                                 │  · ClockSource    │
                                 │  · Detector (FFT) │
                                 └─────────┬─────────┘
                                           │
                    SPSC ring <NotchCommand>│   (detector ──► audio)
                                           ▼
                                                          NotchChain × 2
                    SPSC ring <float> tap  ◄──────────────  (audio ──► detector)
```

Two lock-free rings. Two mutexes. The audio thread participates in nothing else.

---

## 2. Hole 1 — the command queue

### Shape

`LockFreeRingBuffer<NotchCommand>`, **owned by `AudioEngine`**, producer =
detector thread, consumer = audio thread. This closes plan Task 5, which the old
ledger wrongly marked complete: `NotchCommand` is defined in its own header and
referenced nowhere else in the repo.

Ownership sits with `AudioEngine` for symmetry with the tap: `AudioEngine` owns
`tapBuffer_` as *producer* and hands out a reference for the consumer, so it
should own `commandQueue_` as *consumer* and hand out a reference for the
producer. The same caller-contract comment style applies.

### Capacity

**128 entries.** `sizeof(NotchCommand)` is 16 bytes (three `uint8_t` plus one
padding byte, then three `float`), so the ring costs 2 KB.

The sizing case is not steady-state traffic — the detector emits a handful of
commands per second at most. It is a **preset load**, which under [D-05] can
install up to 16 notches on each of 2 channels in one batch: 32 commands
arriving at once, possibly while the detector is also acting. 128 gives 4×
headroom over the worst legitimate burst.

### Drain policy

Drain everything available, **capped at 64 commands per callback**.

The cap exists to bound worst-case callback time, and the bound is comfortable.
`Biquad::setNotchFilter` is validation branches plus one `sin`, one `cos` and
five divides — on the order of 100–200 ns. 64 of them is ~13 µs against a
0.67 ms budget at a 32-sample buffer and 48 kHz: under 2%.

64 is chosen over 32 so that a full 32-command preset applies **within a single
callback** even when detector traffic is interleaved. A preset that half-applies
across two callbacks is audible as two distinct changes to the sound.

### Overflow — why it cannot silently corrupt the model

This is where the tap and the command queue differ, and the difference matters.

The tap's producer is the audio thread, which cannot retry — so it drops, and
`tapDropCount_` records the fact. The command queue's producer is the **detector
thread**, which *can* retry. `LockFreeRingBuffer::write()` returns the number of
items actually written, so a short write is visible to the producer immediately.

**Rule: the detector keeps unsent commands in its outbox and retries on the next
loop iteration.** A full queue therefore delays a notch; it never loses one, and
the model never diverges from the chain.

A `notchCommandRetryCount_` is still worth exposing: sustained retries mean the
audio callback has stopped draining, which is a real fault the UI should be able
to surface.

### Drain point

First statement inside `audioDeviceIOCallbackWithContext`, after
`ScopedNoDenormals` and before any audio is processed — so a notch commanded
this callback takes effect on this callback's samples rather than being one
buffer late.

---

## 3. Hole 2 — notch-state read-back

**There is none, and none is needed.** This hole closes by deletion.

The plan calls Task 12 hard-blocked because the harmonic-aware algorithm must
iterate locked notches, and notch state lives in `AudioEngine::notchChains_`, a
private member whose methods are audio-thread-only. But under [D-05] the
detector is the sole author of every Set and Clear. A sole author does not need
to read back what it wrote — it needs to **remember what it commanded**.

Two independent facts confirm the model must live in the detector regardless:

1. **The data does not fit in `NotchChain`.** Auto-release needs `locked_at` and
   `last_detected_at` per notch. `NotchChain::NotchInfo` is
   `{frequency, Q, depthDB, state}` and cannot carry a clock — it is an
   audio-thread struct.
2. **The GUI needs the same missing fields.** The Lane C audit found Task 22's
   "Locked Time" column has no backing field anywhere, and spec §6.1 wants
   recently-detected notches drawn in a different colour. Both need the age the
   detector already has to track.

So `NotchModel` — the detector's authoritative record — is:

```
struct ModelNotch {
    double frequency;        // Hz
    double Q;
    double depthDB;
    double lockedAtMs;       // live-clock, see section 4
    double lastDetectedMs;   // live-clock; drives auto-release
    Origin origin;           // Detector | Preset | Manual
    bool   active;
};
ModelNotch notches[2][16];
```

(Amended 2026-08-23.) `ModelNotch` holds `double`; `NotchCommand` holds `float`.
The one-directional float→double widening at the model boundary is deliberate:
the command struct stays at 16 bytes for ring density, and no value flows back
from command to model, so there is no precision loss to reason about. Do not
"reconcile" the two to the same type in either direction without reopening this
decision.

`origin` is carried because it costs one byte and answers a question the owner
may revisit: [D-05] chose *"the detector adopts preset notches"*, but the
rejected alternative *"detector leaves them alone"* becomes a one-line policy
change if `origin` is recorded, and impossible to add later if it is not.

### Applying a command, and the one way the model can still diverge

`NotchChain::setNotch` **silently ignores** a command the biquad rejects — the
slot is left as it was (`NotchChain.h:48-50`). If the detector commanded it and
recorded success, the model would then claim a notch the chain does not have.

**Mitigation: the detector validates before sending, using the same predicates
`Biquad::setNotchFilter` applies** — `sampleRate > 0`, `Q > 0`, `0 < freq <
sampleRate/2`, and (amended 2026-08-23, owner-approved) **`depthDB <= 0`**: the
depth-bearing four-argument `setNotchFilter` rejects a positive depth because
the peaking form would BOOST the ringing frequency instead of cutting it. The
detector already knows the sample rate (it is carried in
`Spectrum::sampleRate` precisely to avoid a torn read). A command that would be
rejected is never sent, so the silent-rejection path is unreachable in practice.

This is defence in depth, not a proof. It is called out here so a future reader
knows the divergence risk was seen and bounded rather than missed.

---

## 4. Hole 3 — the detector thread and the clock

### The thread

A `juce::Thread` subclass inside `NotchController`. JUCE is already the
project's threading vocabulary, and `startThread` / `stopThread(timeout)` /
`threadShouldExit()` give a clean shutdown the audio device lifecycle can drive.

**Cadence: poll, with `wait(5)`.** Each wake, call `processLatestBlock()` in a
loop until it returns `magnitudes == nullptr`, so the detector never falls
behind when a large callback delivers several hops at once.

**Rejected: signalling from the audio thread.** A `WaitableEvent::signal()` is
not a lock, but it is a kernel transition on a thread whose contract in this
codebase is "no allocation, no locks, no logging". Polling costs one wake every
5 ms and buys the audio thread a guarantee. The tap holds 8192 samples (~170 ms
at 48 kHz), so a 5 ms poll has enormous margin against overrun, and adds at most
5 ms to a detection budget whose first gate is ~30 ms.

### The clock — two clocks, actually

[D-06] requires that auto-release measure **real elapsed time, but only while
the tap is delivering audio**. That is not one clock, it is a measure and a gate:

| | Source | Role |
|---|---|---|
| Wall clock | `ClockSource::nowMs()` | measures the **duration** of an interval |
| Tap liveness | recent non-null spectra | decides whether that interval **counts** |

`readCount` is **not** promoted to a cadence source — the carry-over rule still
bars that, and rightly: 512 samples is not a stable unit of time. It is demoted
to a liveness flag. Confusing those two roles is exactly the trap the handoff
warns about.

### The live clock

`NotchController` maintains `liveMs_`, advanced once per poll:

```
dt = clock.nowMs() - lastPollMs
if (clock.nowMs() - lastDataMs) < kTapSilenceTimeoutMs:
    liveMs_ += dt
lastPollMs = clock.nowMs()
```

Every timer in the spec — the ~30 ms candidate persistence (§5.2 step 6), the
15 s soundcheck (§5.3), the 30 s auto-release (§5.2 step 7) — reads `liveMs_`,
never the wall clock directly.

### Why `kTapSilenceTimeoutMs` is needed, and why it is 250 ms

The naive formulation — *"advance the live clock only on polls that delivered
data"* — is **wrong**, and it is worth recording why, because it is the obvious
implementation.

At a 5 ms poll and a 10.67 ms hop, roughly every other poll legitimately has no
new data. The live clock would then advance at about half real speed and a 30 s
auto-release would take a minute. The gate must be *"has the tap delivered
**recently**"*, not *"did it deliver **this poll**"*.

The threshold must exceed the longest legitimate gap between tap writes, which
is one audio callback. Worst realistic case is a 2048-sample buffer at 44.1 kHz:
**46.4 ms**. 250 ms gives >5× margin while costing at most 0.83% error against
the 30 s auto-release.

**Bound this assumes:** buffer sizes up to ~2730 samples at 44.1 kHz. Beyond
that the constant must be derived at runtime from the actual buffer size. It is
not today, because no supported configuration reaches it.

### Injectability is a requirement, not a nicety

Plan Task 14's test is *"Set notch, wait 30s **simulated** time, verify notch
auto-cleared."* No test may sit through 30 real seconds. `ClockSource` is a
two-method abstract interface (`nowMs()`), production-backed by
`juce::Time::getMillisecondCounterHiRes()` and test-backed by a fake that
advances on command.

Deliberately **not** `std::function<double()>`: it may heap-allocate, and this
is called on the detector thread's hot loop.

---

## 5. Hole 4 — publishing to the GUI

One `std::mutex` inside `NotchController` guards **one snapshot struct**
containing both the magnitude spectrum and the notch list.

```
void copySnapshot (SnapshotBuffer& destOwnedByCaller) const;
```

Three decisions in one line:

1. **A mutex, not lock-free.** Both endpoints are non-real-time. The GUI reads
   at 60 Hz (~16.7 ms apart); the detector writes at ~10.7 ms. Hold time is a
   ~3.6 KB memcpy, on the order of a microsecond. A seqlock or triple buffer
   would add a retry loop or a third buffer to solve a problem neither thread
   has.

2. **Caller-owned destination.** Plan Task 21 requires no allocation in the
   paint path. Returning a container would allocate every frame; copying into
   storage the view pre-allocated does not.

3. **Spectrum and notches under the same lock, in the same struct.** This is a
   correctness property, not tidiness. Task 20 draws notch markers *over* the
   spectrum. Two separate snapshots let the GUI pair a spectrum frame with a
   notch list from a different instant, and draw a marker for a notch that did
   not exist when that spectrum was captured.

This also resolves the borrowed-pointer hazard: `Spectrum::magnitudes` is
documented invalid after the next `processLatestBlock()`, so the detector thread
copies it into the snapshot while it is still valid, and the GUI never sees the
borrowed pointer at all.

---

## 6. Ownership and file layout

```
src/app/NotchController.h/.cpp    NEW  thread, model, clock, snapshot, policy
src/dsp/ClockSource.h             NEW  interface + juce-backed production impl
src/app/AudioEngine.h/.cpp        MOD  owns commandQueue_, drains it, accessor
```

**`MainComponent` owns both `AudioEngine` and `NotchController`** and wires them
together. `NotchController`'s constructor takes `LockFreeRingBuffer<float>& tap`,
`LockFreeRingBuffer<NotchCommand>& commands` and `ClockSource&`.

### This departs from the plan, deliberately

Plan Task 13 says *"Modify `src/app/AudioEngine.h/cpp` — add `NotchController`
member."* Two reasons not to:

- **It creates an ownership cycle.** `NotchController` needs the tap and the
  command queue, which `AudioEngine` owns; `AudioEngine` would own
  `NotchController`.
- **It destroys testability.** With the controller taking plain ring-buffer
  references, every behaviour in this document — candidate persistence,
  auto-release under a fake clock, the [D-06] freeze, preset adoption — is
  testable by hand-feeding a ring buffer, **with no audio device**. That is the
  same reasoning that kept `Detector` thread-free (`Detector.h:3-7`), and it is
  why `Detector` has real coverage today while `AudioEngine` had none until the
  fix pass added four tests.

### Lifecycle — the shutdown ordering (amended 2026-08-23, owner-approved)

`LockFreeRingBuffer::clear()` has a hard precondition: neither producer nor
consumer may be running. Both rings are cleared in `audioDeviceAboutToStart()`,
so the device lifecycle must guarantee the detector thread is stopped first:

```
device restart / stop:
  1. NotchController::stop(timeout)   -- detector thread joins; producer of
                                         commands AND consumer of tap are gone
  2. engine.stop()/restart            -- JUCE removes the audio callback
device start:
  3. audioDeviceAboutToStart()        -- clear() both rings (precondition holds)
  4. NotchController::start()         -- thread resumes on clean rings
```

**MainComponent owns this ordering** because it owns both objects. No code path
may clear either ring while `NotchController`'s thread is live.

---

## 7. What this design is tested by

Every item is reachable without an audio device.

| Behaviour | Test shape |
|---|---|
| Task 5 — commands cross the boundary | Write commands, run one callback, assert `NotchChain` state |
| Drain cap | Enqueue 100, assert exactly 64 applied in one callback, rest on the next |
| No silent loss | Fill the queue, assert the producer retries and nothing is dropped |
| [D-06] freeze | Fake clock +30 s with a dead tap → notch survives. Same with a live tap → notch clears |
| The half-speed bug | Live tap, polls that alternate empty → `liveMs_` tracks wall time within tolerance |
| Auto-release (Task 14) | Fake clock, simulated 30 s |
| [D-05] preset adoption | Load preset → notches enter the model with `origin = Preset` and auto-release applies |
| Snapshot consistency | Notch added between two reads never appears over a stale spectrum |
| Task 12 harmonics | Model has a locked 500 Hz; assert a 1 kHz candidate is penalised |

---

## 8. Alternatives rejected

| Rejected | Why |
|---|---|
| MPSC command queue (GUI and detector both produce) | [D-05] makes the detector sole producer, so SPSC suffices. Lock-free MPSC is materially harder to get right and this project has one thread that cannot afford a bug. |
| Audio thread publishes a notch snapshot | Adds a write to the real-time path for data the detector already owns, and still could not carry `lockedAtMs`. |
| Lock-free spectrum publication (seqlock / triple buffer) | Neither endpoint is real-time. Complexity with no beneficiary. |
| Audio thread signals the detector | Kernel transition on the real-time thread to save ~5 ms of latency the budget does not need. |
| `std::function` clock | May allocate; called in the detector's hot loop. |
| `NotchController` as an `AudioEngine` member (as planned) | Ownership cycle, and it would make the controller untestable without an audio device. |
| Advance the live clock only on data-bearing polls | **Wrong**, not merely worse — halves clock rate in healthy operation. Recorded in §4 so it is not re-invented. |

---

## 9. Out of scope, and one thing that must land first

Not covered here: the detection algorithm itself (Tasks 12–15 policy), the GUI
(16–24), presets on disk (25–26), licensing, installer.

**Prerequisite — `depthDB` has no path into the filter.** Verified this session:
`Biquad::setNotchFilter(freq, Q, sampleRate)` takes **no depth parameter at
all**. It implements the RBJ pure notch, which is an infinite-depth null. So
`depthDB` is not merely "applied by nothing" as the handoff says — there is
nothing to apply it *with*.

This blocks spec §5.1 (6–24 dB), Task 26's presets (−18 dB / −10 dB), Task 22's
Depth column and Task 20's depth-as-marker-height. Under [D-05] it also blocks
the preset round-trip this design assumes works.

It is independent of the bridge and should land before it. Tracked separately.

**(Amended 2026-08-23: LANDED.)** The depth-bearing four-argument
`Biquad::setNotchFilter(freq, Q, sampleRate, depthDB)` now exists
(`src/dsp/Biquad.h`), `NotchChain::setNotch` carries depth through to it and
replays depth across sample-rate changes (`src/dsp/NotchChain.cpp`). The
prerequisite is satisfied; see also §3's amended predicate list.

---

## 10. Open questions for the owner

None blocking. The five design questions carried in the decision log —
queue capacity and overflow, detector cadence, spectrum publication, the
tap-silence threshold, and depth-fix ordering — are all engineering choices, and
this document decides them with reasons and rejected alternatives, as the
parallel execution plan asked.

What the owner should push back on, if anywhere:

1. **§1's principle** — that the GUI path needs no lock-free machinery. Everything
   downstream leans on it.
2. **§6's departure from plan Task 13.** It contradicts the written plan.
3. **§4's 250 ms constant**, and the buffer-size bound it assumes.
