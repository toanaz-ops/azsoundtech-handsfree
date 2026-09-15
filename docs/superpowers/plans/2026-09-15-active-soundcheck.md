# Active Soundcheck (Lane M) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One `ĐO` action plays a 3 s log sine sweep out of one output channel at a time with every lane that feeds that channel muted, captures the raw mic, computes loop gain per bin, and PROPOSES preventive notches quantised onto lane G's depth ladder — nothing reaches a filter until the operator presses `ÁP DỤNG`.

**Architecture:** Three PURE classes in `src/dsp/` do all the arithmetic and are tested with no device and no thread: `SoundcheckSignal` (sample index → amplitude, plus the ramp-out envelope), `LoopGainEstimator` (Welch-style energy accumulation of noise floor / reference / capture into `H_dB[k]`), `SoundcheckCandidates` (peak-pick + prominence + the depth rule). `AudioEngine` gains **seven atomics, one drop counter and one capture ring**, read ONCE per callback beside `bypass`, driving four insertion points already identified in the callback. `SoundcheckController` (`src/app/`, its own poll thread on an injectable `ClockSource`) is the state machine: it never holds a pointer to a `NotchController`, it only sets engine flags, drains `micCapture_`, and publishes `OutputResult`s. Everything that writes a filter — `ÁP DỤNG` / `BỎ` — runs on the message thread through the existing `setNotch` / `clearNotch`.

**Tech Stack:** C++17, JUCE 8/9 (`juce_audio_devices`, `juce_dsp`, `juce_events`, `juce_gui_basics`), GoogleTest via ctest, CMake + `Visual Studio 18 2026` generator, MSVC, Python 3 (logstats fixture test), NSIS (`installer/release-alpha.ps1`).

**Spec:** `docs/superpowers/specs/2026-09-15-active-soundcheck-design.md` — **rev 4**, 1202 lines, three independent read-only review rounds; round 3 concluded READY FOR PLAN with no blockers. **Decisions:** `docs/superpowers/decisions/2026-09-15-lane-m-active-soundcheck.md` — Q1–Q17 plus Q3-relitigated ×2 and Q15-relitigated are **FIXED**; do not re-open any of them.

**Release:** 1.3.0 (`pwsh -File installer\release-alpha.ps1 -Part minor`), after the docs task — **and BLOCKED until the coordinator has recorded the owner's answers to the nine confirmation items in the decision file.** See Task 12.

---

## The nine owner confirmations — the release gate

The spec's header and the decision record's first section list **nine** items the owner must confirm before a signal reaches a real PA. They are reproduced here because Task 12 refuses to run until every one of them has an owner answer written into `docs/superpowers/decisions/2026-09-15-lane-m-active-soundcheck.md` as a new `Q<n> (lật lại <ngày>)` entry — never by editing the old entry (`recording-design-decisions` rule, and the decision file's own closing note says so).

| # | Item | What the coordinator must have in writing |
|---|---|---|
| 1 | **Q2** — emission level | −20 dBFS peak, phrased as spec §3 phrases it ("20 dB below the system's full scale, at your current master"), with **no** dB SPL figure |
| 2 | **Q15 (relitigated)** — mute the whole OUTPUT CHANNEL | 4.5 s of silence per output channel, **~72 s worst case** for 8 stereo slots |
| 3 | **Q6** — propose vs place | v1 only PROPOSES. The owner's original prompt said "place"; this is a deliberate divergence |
| 4 | **Q3 (relitigated)** — the remaining self-abort set | and the worst-case stop latency: ≤ 31.3 ms @ buffer 64, ≤ 51.3 ms @ buffer 1024, plus driver/amp latency the app does not control |
| 5 | **Q16** — a separate `ĐO` button rather than re-using `SOUNDCHECK`. Also a divergence from the original prompt |
| 6 | **Q7 + Q9** — the preset asymmetry: a preventive notch placed now never auto-releases, but the same notch re-loaded from a preset comes back as `Origin::Preset` and DOES release after 30 s of quiet |
| 7 | **`kResultsTimeoutMs` = 20 000 ms** (down from 60 000) |
| 8 | **`ClearReason::SoundcheckReplace`** — a new value in lane D's enum |
| 9 | **`Origin origin` added to `SnapshotNotch`** — the second and last additive change to `NotchController` |

Tasks 1–11 may all be executed, reviewed and committed without these answers: none of them ships anything to a tester. **Task 12 is the gate.** Task 12 Step 0 names, per item, which task has to be reworked if an answer comes back NO.

---

## Global Constraints

Every task's requirements implicitly include this whole section.

### Live-sound rules (CLAUDE.md, repo rule 10)

- **This lane makes the app emit sound into a PA for the first time.** From `AudioEngine` outward the sweep passes exactly one hard clamp, `±kMaxOutputLevel = 1.0f` (`src/app/AudioEngine.cpp:15`, applied at `:631-647`). **There is no limiter anywhere in this app.** The original lane M prompt said there was; there is not.
- **Never remove a clamp, a limiter, a NaN/denormal guard, or a bounds check.** `ScopedNoDenormals` (`src/app/AudioEngine.cpp:487`), the `std::isfinite` guards (`:605-606`, `:615-619`) and the output clamp are untouched by this lane. The injection point goes **before** the clamp, never after.
- **Every task states its expected level change up front** in a `Mức level dự kiến` line, copied from spec §3 — not invented here.
- **GUI work is not reported without a rendered picture that has been READ BACK**: `build/tools/Release/HandsFreeSnapshot.exe shots 1440 920 --fast`.

### The seven atomics + one counter + one ring (spec §4.1)

Declared in `src/app/AudioEngine.h` beside the existing cross-thread block (anchor text: `// Cross-thread state. std::atomic keeps the audio callback lock-free`, `src/app/AudioEngine.h:329-341` — numbers are pre-Task-5), with these exact names, types and initialisers:

```cpp
// The OUTPUT channel being measured. -1 = not running. This IS the mute key
// (Q15 relitigated): every lane whose outputChannels[lane] == scOutChannel_ is
// dropped. It returns to -1 at EVERY Gap, so it can NOT be used as the
// tap-suspend key -- see scSuspendTaps_.
std::atomic<int>          scOutChannel_       { -1 };
// Suspends tap writes for the WHOLE run: set at Arm, cleared at the end of the
// LAST channel's tail (or on Abort). Held across every Gap.
std::atomic<bool>         scSuspendTaps_      { false };
std::atomic<int>          scCaptureInChannel_ { -1 };
// Capture gate, true across NoiseFloor + Sweep + Tail (F5). Separate from
// scOutChannel_ so a SAFETY property is not inferred from the sign of an index.
std::atomic<bool>         scCaptureActive_    { false };
// Sweep sample index. NEGATIVE during the NoiseFloor phase. The audio thread
// OWNS it; the controller only reads it.
std::atomic<std::int64_t> scSampleIndex_      { 0 };
// Peak amplitude, ALREADY clamped to <= kSoundcheckMaxPeak before it is stored.
std::atomic<float>        scPeak_             { 0.0f };
// Ramp-out anchor. -1 = none. Set by the message thread, CONSUMED by the
// callback, which also clears it and scOutChannel_ when the ramp finishes (F8).
std::atomic<std::int64_t> scRampOutAtSample_  { -1 };

static constexpr std::size_t kCaptureCapacity = 65536;
LockFreeRingBuffer<float>   micCapture_      { kCaptureCapacity };
std::atomic<std::uint64_t>  micCaptureDrops_ { 0 };
```

`kCaptureCapacity` is **not** "how much audio we need". `micCapture_` is a stream the lane M thread drains every 5 ms, exactly as the detector drains its tap. It is the **maximum tolerable drain latency**: 65536 samples = 1.37 s @ 48 kHz, 0.68 s @ 96 kHz, **0.34 s @ 192 kHz** — still 68× the 5 ms drain period at the highest rate (F25).

### Constants (spec §4.10) — every value, verbatim

| Constant | Value | Defined on |
|---|---|---|
| `kSweepLowHz` | `100.0` | `SoundcheckSignal` |
| `kSweepHighHz` | `10000.0` | `SoundcheckSignal` |
| `kSweepSeconds` | `3.0` | `SoundcheckSignal` |
| `kRampMs` | `30.0` | `SoundcheckSignal` |
| `kRampOutMs` | `30.0` | `SoundcheckSignal` |
| `kSoundcheckMaxPeak` | `0.1f` (−20 dBFS) | `SoundcheckSignal` |
| `kSoundcheckMinPeak` | `0.01f` (−40 dBFS) | `SoundcheckSignal` |
| `kTrustedHighHz` | `6000.0` | `LoopGainEstimator` |
| `kMinBandSnrDb` | `12.0` | `LoopGainEstimator` |
| `kMinBinSnrDb` | `6.0` | `LoopGainEstimator` |
| `kCandidateMarginDb` | `-6.0` (the **MARK** threshold) | `SoundcheckCandidates` |
| `kMinUsefulCutDb` | `3.0` (the **PROPOSE** threshold) | `SoundcheckCandidates` |
| `kMinProminenceDb` | `6.0` | `SoundcheckCandidates` |
| `kTargetMarginDb` | `6.0` | `SoundcheckCandidates` |
| `kMaxPreventivePerLane` | `6` | `SoundcheckCandidates` |
| `kTailSeconds` | `0.7` | `SoundcheckController` |
| `kGapMs` | `300.0` | `SoundcheckController` |
| `kNoiseFloorMs` | `500.0` | `SoundcheckController` |
| `kMicAbortDbfs` | `-6.0` | `SoundcheckController` |
| `kMicAbortHoldMs` | `20.0` | `SoundcheckController` |
| `kResultsTimeoutMs` | `20000.0` | `SoundcheckController` |
| `kPollMs` | `5.0` | `SoundcheckController` |

**Fixed for 1.3.0 and NOT on the GUI** (spec §4.10). A slider on any of them turns every future bug report into "which value was it on?".

**Deliberate deviation from §4.10's "one place, `SoundcheckController.h`", declared here so a reviewer does not read it as drift.** Each constant is *defined* on the class that computes with it, because `src/dsp/` must not include `src/app/` headers (library rule below) and the audio callback in `AudioEngine.cpp` cannot include `app/SoundcheckController.h` — that header includes `AudioEngine.h`. `SoundcheckController.h` then **aliases every one of them**:

```cpp
static constexpr double kSweepLowHz = SoundcheckSignal::kSweepLowHz;   // ...and so on, all 15
```

This is exactly lane G's m-D shape: one **definition**, aliases elsewhere, never a second literal (`gui::SpectrumView::kRingRiskRisingFraction = NotchController::kRiskFreezeFraction`, `src/gui/SpectrumView.h:246`). `SoundcheckController.h` therefore still answers "where do I look up a lane M constant?" with one file. Task 4 Step 6 greps for duplicated literals.

**One value is NOT a constant and must not be filed with them** (spec §4.10, R3-3):

| Value | What it is |
|---|---|
| `RunParams::noiseFloorGate` | **Live, not constant.** `= controller.getPeakinessThreshold()` (`src/app/NotchController.h:365`), read **once at `Arm` on the message thread**, then immutable for the run. Default `PeakinessAnalyzer::kDefaultThreshold = 10.0f` (`src/dsp/PeakinessAnalyzer.h:124`), operator-adjustable in [5, 20]. It is **NEVER** `CandidateScorer::kConfirmScore` — see the unit trap below |

### The unit trap this lane already fell into once (N1) — read before writing any comparison

`PeakinessAnalyzer::peakinessAt` (`src/dsp/PeakinessAnalyzer.h:166`) returns an **unbounded ratio**: on the real rig the worst noise bin over 60 seeds reads **7.35** and a 1 kHz tone reads **131.70** (`src/dsp/PeakinessAnalyzer.h:60-65`). `CandidateScorer::kConfirmScore = 0.7f` (`src/dsp/CandidateScorer.h:49`) is the threshold of a **0..1 product**. Spec rev 2 compared the first against the second and would have aborted **every run in every room at the first channel**. This is `memory/ring-risk-lane-r-2026-09-06.md` repeating in the opposite direction inside the same project. **Every comparison this plan introduces names the unit of both sides in a comment.**

### The RISING gate, as an identity (spec §4.3, R3-2)

Refuse to run when, and only when:

```cpp
snapshot.ringRiskValid
  && snapshot.ringRiskScore >= NotchController::kRiskFreezeFraction * snapshot.ringRiskThreshold
```

`kRiskFreezeFraction = 0.55f` (`src/app/NotchController.h:140`); `ringRiskThreshold` is published as `CandidateScorer::kConfirmScore` (`src/app/NotchController.h:436`, assigned at `src/app/NotchController.cpp:648`), so the effective number is **0.55 × 0.7 = 0.385**. Take the PRODUCT from the snapshot; never hard-code 0.385 — it is the same constant the GUI's RISING band uses, so the chip on screen and lane M's gate cannot drift apart. **`ringRiskValid == false` does NOT refuse**: it means "no frame has been scored yet", which is always true in Bypass and after every reset, and refusing there would lock out the commonest case of all (open the app, measure the room). It is RECORDED instead — `soundcheck_start` carries `ring_risk: null`.

### The thread map

| Thread | What it may do |
|---|---|
| **Message thread** | the `ĐO` / `ÁP DỤNG` / `BỎ` / `DỪNG` button lambdas; `SoundcheckController::preflight` and `arm`; **every** `setNotch` / `clearNotch`; reading `getPeakinessThreshold()` once at `Arm`; turning detection off at `Arm`; all GUI |
| **Soundcheck thread** (`SoundcheckController`) | poll every 5 ms, drain `micCapture_`, run `LoopGainEstimator` / `SoundcheckCandidates`, set the seven engine atomics, publish `OutputResult`s, emit log events. **Holds no `NotchController*` and calls no `NotchController` method** (inv 17) |
| **Audio callback** | the seven atomics (read ONCE, beside `bypass`), `micCaptureDrops_`, `micCapture_.write`. **No lock, no allocation, no logging (inv 16 — review-enforced; §5.2 checklist)** |

### The twenty safety invariants (spec §4.11)

Each is a testable statement and §5.1 has at least one test for it — **except invariant 16**, which is a property of the source text, not observable behaviour, and is enforced by the reviewer checklist in §5.2. The spec numbers them 1..20 with an extra `10b`; this plan keeps the spec's labels, so "inv 10b" below means the spec's `10b`.

1. Every sample leaving `SoundcheckSignal` has `|x| <= kSoundcheckMaxPeak`, for **any** `peak` passed in — negative, NaN, or > 1 included.
2. `x(0) == 0.0f`, the window's last sample `== 0.0f`, and `x(n) == 0` for `n < 0`.
3. **Every callback**, `scOutChannel_` and `scCaptureInChannel_` are re-checked against THAT callback's channel counts; an out-of-range index means no injection, no capture, no out-of-bounds write.
4. The injection point is **before** the `±kMaxOutputLevel` clamp (`src/app/AudioEngine.cpp:631-647`).
5. The sweep is written to **exactly one** channel: `scOutChannel_`.
6. `scCaptureActive_ == false` ⇒ **no** sample enters `micCapture_`. `scOutChannel_ == -1` ⇒ **no** sweep sample and **no** lane muted. The two gates are **independent**: `NoiseFloor` has `scCaptureActive_ == true` and `scOutChannel_ != -1` but `scSampleIndex_ < 0`, so the amplitude is 0.
7. The **seven** soundcheck atomics are read **once** per callback, beside `bypass` (`src/app/AudioEngine.cpp:509-514`); the rest of the callback uses only the stack copies.
8. While running, **every** lane with `outIdx == scOutChannel_` is muted: that channel carries the sweep and nothing from any chain.
9. `Abort` from anywhere ⇒ the **callback** generates a raised-cosine ramp-out of at most `kRampOutMs` and then sets `scOutChannel_ = -1` **itself**. No other thread need still be alive. There is no hard-cut path.
10. Tap suspension is keyed on **`scSuspendTaps_`**, and that flag is held **across every `Gap`**: from `Arm` to the end of the LAST channel's tail no tap is written and `tapDropCounts_` does **not** move. Keying it on `scOutChannel_` (which returns to −1 at every Gap) is wrong and costs ≈ 11.5 s of release clock over 16 channels (spec §4.1).
10b. Total `liveMs_` drift from one run is **≤ ~420 ms, once for the whole run** — not once per channel.
11. While running, detection is off on every slot and **no `NotchCommand`** is pushed into any command ring.
12. Detection is restored **before** `Results` is entered, and `kResultsTimeoutMs <= 20 s`.
13. No notch is placed unless the operator presses `ÁP DỤNG`.
14. Every proposed depth is `<= 0`, `>= kMaxDepthDb`, and is either a rung of `kDepthLadderDb` **or the preset ceiling itself** — including a ceiling that is not a multiple of 6 (`presets/Music.json` carries **−10.0**, read 2026-09-15).
15. No preventive notch lands on a bin that already has a live notch within ±1 bin.
16. The callback touches **only** the seven atomics + `micCaptureDrops_` + `micCapture_`; no lock, no allocation, no logging. **(Review-enforced, §5.2 — no test can catch a violation here; only a reader can.)**
17. `SoundcheckController` **never** calls `NotchController`; every `setNotch` / `clearNotch` runs on the message thread. The noise-floor gate too: the message thread reads `getPeakinessThreshold()` at `Arm` and hands it in via `RunParams::noiseFloorGate`; the lane M thread holds no pointer to any `NotchController`.
18. Lane M neither writes nor consumes lane G's "room memory" (`src/app/NotchController.cpp:1116` already gates Soundcheck out of it).
19. A failed `Preflight` ⇒ **no** sample emitted, state back to `Idle`.
20. A sample-rate or channel-count change mid-run ⇒ abort within one poll (5 ms) plus the ramp-out.

### Repo / process rules that have already cost this project time

- **Never `git stash`.** The stash stack is shared with the main checkout and every other worktree, and another session may pop yours (lane G Task 4 process trap, recorded in `.superpowers/sdd/2026-09-07-gain-aware-notch/progress.md`). Set work aside with a temporary WIP commit instead.
- **`git add` explicit paths only.** Never `git add .`, never `git add -A`. And note `git commit -- <path>` commits the WHOLE working-tree file, so another lane's hunk rides along (`memory/gui-console-lessons-2026-08-24.md`).
- **Delete `.superpowers/sdd/.gitignore` before EVERY commit.** The `sdd-workspace` script rewrites it with `*` on every run, silently un-tracking this repo's "`.superpowers/` is TRACKED" policy (`memory/sdd-workspace-gitignore-trap-2026-09-04.md`).
- **`ev` — not `kind` — is the log's dispatch key** (lane G B-3). A new event type without its own `ev` name in `MainComponent::notchEventToVar` (`src/app/MainComponent.cpp:578-588`) makes `tools/logstats.py` pop the wrong record.
- **Liveness is `activeForTest`, never `depthDbForTest(...) < 0`** (lane G B-3). `pushClearLocked` (`src/app/NotchController.cpp:270`) lowers only `active` and deliberately leaves `depthDB` alone, so a depth-based probe matches every slot that has ever held a notch.
- **UTF-8 explicitly in every script that reads and rewrites a repo file** (repo rule 6). `tools/logstats.py` already opens with `encoding="utf-8"` (`tools/logstats.py:20`); keep it. Vietnamese corrupts silently otherwise and the corruption survives into git.
- **`src/dsp/` must not include `juce_audio_devices` or any `src/app/` header.** `AudioEngine.h` is the only file that pulls `juce_audio_devices`, deliberately (`src/app/AudioEngine.h:32-36`). The three new DSP classes include at most `<juce_dsp/juce_dsp.h>` (which `src/dsp/Detector.h:55` already does) and the standard library. Where a DSP class needs a lane G constant — the depth ladder — it takes it as a **parameter**, so there is still exactly one definition and no copy.
- **Every new test carries a comment saying which production change turns it red** (repo convention, lanes S / R / D / G).
- **A harness whose `width_` does not match what the test asserts poisons the NEXT test in the same file** (`memory/gain-aware-notch-lane-g-2026-09-07.md` item 18): a mono harness with `width_ == 2` turns `effectiveLinked()` on and writes a dead lane-1 entry. Every LINKED test in Task 7 builds `width_` and the lane-1 tap explicitly.

### Build and verify (from the worktree root)

- configure — only when a header or a `CMakeLists.txt` changed:
  `cmake -B build -G "Visual Studio 18 2026" -A x64`
- build: `cmake --build build --config Release`
- one suite: `cd build && ctest -C Release -R <regex> --output-on-failure`
- the full gate before every commit that touches `src/`: `cd build && ctest -C Release` must print `100% tests passed`.

The generator is **18 2026, not 17 2022** — this machine only has Build Tools 2026. Do not drop `-G`: the `cmake` on PATH is the MinGW/WinLibs build and defaults to Ninja, which rejects `-A x64`. Use `build/`; never create `build-*` variants.

**Test-count baseline: 547, and here is how it was verified** rather than taken on trust from the commit message. `tests/CMakeLists.txt:107` registers the GoogleTest binary with `gtest_discover_tests(HandsFreeTests)`, so every `TEST`/`TEST_F` macro in the 23 files listed at `tests/CMakeLists.txt:20-42` becomes one ctest case; `tests/CMakeLists.txt:113` adds exactly one more, `logstats_fixture`. Counting the macros, in this worktree at `2c84075`:

```
grep -h "^TEST" tests/*.cpp | wc -l            ->  546
grep -rn "INSTANTIATE_TEST\|TEST_P(" tests/    ->  no matches
```

No parameterised suites exist to multiply a macro into several cases, so 546 + 1 = **547**, matching `547/547` in `installer/TESTER-NOTES.md:3` and the roadmap's lane G row. Every per-task `100% tests passed (N)` below is an **ESTIMATE** carried forward from 547. Use the number `ctest` actually prints; if it differs from the estimate by more than the tests you just wrote, find out why rather than editing the estimate.

---

## File Structure

| File | Responsibility after this plan |
|---|---|
| `src/dsp/SoundcheckSignal.h` / `.cpp` | **New, pure.** The signal constants; `clampPeak`; the closed-form log sweep `sampleAt(n)` with its 30 ms raised-cosine window; the separate `rampOut(n, n0, R)` envelope; `instantaneousHz(n)`. No state beyond ctor-computed doubles, no allocation, no JUCE |
| `src/dsp/LoopGainEstimator.h` / `.cpp` | **New, pure.** Welch accumulation of noise floor / reference / capture over the detector's 2048-point Hann FFT at hop 512; `hDb[k]`, per-bin `trusted[k]`, band SNR. Uses `juce::dsp::FFT` only |
| `src/dsp/SoundcheckCandidates.h` / `.cpp` | **New, pure.** 1/3-octave smoothing, marking, prominence, the ±1-bin live-notch exclusion, the depth rule and its saturation report. Takes the depth ladder as a parameter so `src/dsp/` stays free of `src/app/` |
| `src/app/SoundcheckController.h` / `.cpp` | **New.** `RunParams`, `Target`, `OutputResult`, the state machine on an injectable `ClockSource`, the poll thread, the abort set, the five log events, and the **message-thread** free function `applySoundcheckResults` |
| `src/app/AudioEngine.h` / `.cpp` | + seven atomics, `micCapture_`, `micCaptureDrops_`, accessors, the test seam `setSoundcheckPeakUnclampedForTest`; four insertion points in the callback; `micCapture_.clear()` in the existing `audioDeviceAboutToStart` drain block |
| `src/app/NotchController.h` / `.cpp` | **Two additive changes only**: `Origin origin` on `SnapshotNotch` (`src/app/NotchController.h:394-406`, filled at `src/app/NotchController.cpp:578-580`) and `ClearReason::SoundcheckReplace` (`src/app/NotchController.h:67-70`). Nothing else |
| `src/app/MainComponent.h` / `.cpp` | `reasonName` gains `soundcheck_replace`; the five `soundcheck_*` log events; the `ĐO` / `DỪNG` / `ÁP DỤNG` / `BỎ` wiring; the control lock; the `onBeforeRestart` abort+join |
| `src/gui/ModeRail.h` / `.cpp` | + `measureButton { "ĐO" }` and `onMeasure`, laid out beside `soundcheckButton` (Q16 option 1) |
| `src/gui/SpectrumView.h` / `.cpp` | + `setSoundcheckOverlay(...)` / `clearSoundcheckOverlay()` and the paint of the margin curve, the markers, and the dimmed 6–10 kHz "low confidence" band. **Data comes from lane M, never from `copySnapshot()`** — the snapshot is frozen while a run is in flight |
| `src/gui/SoundcheckPanel.h` / `.cpp` | **New.** The progress overlay (`ĐANG ĐO · kênh 2/4`, lane M's own clock, the big `DỪNG`) and the one-line results strip (`tìm thấy N điểm dễ hú` + `ÁP DỤNG` / `BỎ`). Grabs keyboard focus; `Esc` is the secondary path |
| `tools/logstats.py` | a `soundcheck_replace` tally + `--expect-soundcheck-replaced`; the five `soundcheck_*` `ev` names need **no** branch and must fall through (spec §4.7) |
| `tests/test_soundchecksignal.cpp`, `test_loopgainestimator.cpp`, `test_soundcheck_candidates.cpp`, `test_soundcheckcontroller.cpp` | **New** files, added to the list at `tests/CMakeLists.txt:20-42` |
| `tests/test_audioengine.cpp`, `test_notchcontroller.cpp`, `test_sessionlogger.cpp`, `test_spectrumview.cpp`, `test_moderail.cpp`, `test_gui_wiring.cpp` | extended per spec §5.1 |
| `tests/fixtures/session-sample.jsonl`, `tests/CMakeLists.txt` | the fixture gains the five `soundcheck_*` lines and one `soundcheck_replace` clear; the `logstats_fixture` entry gains `--expect-soundcheck-replaced 1` |
| `CMakeLists.txt` | the four new `src/` pairs added to `HANDSFREE_CORE_SOURCES` (`CMakeLists.txt:84`), which the app, the test binary and `HandsFreeSnapshot` all consume |
| `tools/snapshot.cpp` | a `soundcheck` scene producing `shots/console-soundcheck-results.png` |
| `docs/GIOI-THIEU.md`, `docs/KY-THUAT-CHONG-HU.md`, `docs/release-notes/1.3.0-alpha.md`, `installer/TESTER-NOTES.md`, `memory/`, the roadmap | the docs task |

---

### Task 1: `SoundcheckSignal` — the sweep, the window, and the ramp-out, as pure functions of a sample index

**Mức level dự kiến (spec §3):** **0 dB.** This task adds an unreachable code path: nothing calls `SoundcheckSignal` until Task 5. The properties it must carry forward: `|x| <= kSoundcheckMaxPeak = 0.1f` (−20 dBFS) for **any** requested peak including NaN, and `x(0) == 0`, last-sample `== 0`, `x(n<0) == 0` so the noise-floor phase needs no separate "emitting" flag.

**Files:**
- Create: `src/dsp/SoundcheckSignal.h`, `src/dsp/SoundcheckSignal.cpp`
- Create: `tests/test_soundchecksignal.cpp`
- Modify: `CMakeLists.txt` — add both to `HANDSFREE_CORE_SOURCES` (anchor text: `set(HANDSFREE_CORE_SOURCES`, `CMakeLists.txt:84`; the paths there are ABSOLUTE, `${CMAKE_SOURCE_DIR}/src/...`, because `tools/` and `tests/` resolve relative paths against their own directory)
- Modify: `tests/CMakeLists.txt` — add `test_soundchecksignal.cpp` to the `add_executable(HandsFreeTests ...)` list (anchor text: `test_slotconfig.cpp`, `tests/CMakeLists.txt:20`)

**Interfaces:**
- Consumes: nothing. `<cmath>`, `<cstdint>`, `<algorithm>` only — no JUCE, no `src/app/`.
- Produces:
  ```cpp
  class SoundcheckSignal
  {
  public:
      static constexpr double kSweepLowHz       = 100.0;
      static constexpr double kSweepHighHz      = 10000.0;
      static constexpr double kSweepSeconds     = 3.0;
      static constexpr double kRampMs           = 30.0;
      static constexpr double kRampOutMs        = 30.0;
      static constexpr float  kSoundcheckMaxPeak = 0.1f;    // -20 dBFS
      static constexpr float  kSoundcheckMinPeak = 0.01f;   // -40 dBFS

      static float clampPeak (float requested) noexcept;

      struct Params
      {
          double sampleRate    = 48000.0;
          double lowHz         = kSweepLowHz;
          double highHz        = kSweepHighHz;
          double sweepSeconds  = kSweepSeconds;
          double rampMs        = kRampMs;
          float  peak          = kSoundcheckMaxPeak;
      };

      explicit SoundcheckSignal (const Params& p) noexcept;

      [[nodiscard]] float        sampleAt       (std::int64_t n) const noexcept;
      [[nodiscard]] double       instantaneousHz (std::int64_t n) const noexcept;
      [[nodiscard]] std::int64_t totalSamples()   const noexcept;
      [[nodiscard]] std::int64_t rampSamples()    const noexcept;
      [[nodiscard]] float        clampedPeak()    const noexcept;

      static float rampOut (std::int64_t n, std::int64_t anchor,
                            std::int64_t rampLengthSamples) noexcept;
      static std::int64_t rampOutSamples (double sampleRate) noexcept;
  };
  ```
  `sampleAt` returns `0.0f` for `n < 0` and for `n >= totalSamples()`. `rampOut` returns `1.0f` for `n < anchor`, the raised-cosine half-window for `anchor <= n < anchor + rampLengthSamples`, and **exactly** `0.0f` for `n >= anchor + rampLengthSamples`.

**Why the callback may evaluate this per sample.** `sampleAt` costs one `std::exp` and one `std::sin`. At the worst rate/buffer combination the app supports (192 kHz, 64-sample buffer) the callback budget is 333 µs and this adds 64 × (exp + sin) ≈ 64 × 40 ns ≈ **2.6 µs**, under 1 %. Constructing the object is one `std::log` plus arithmetic, once per callback on the stack — no allocation, no lock (inv 16).

- [ ] **Step 1: Write the failing tests**

Create `tests/test_soundchecksignal.cpp`:

```cpp
// tests/test_soundchecksignal.cpp
//
// SoundcheckSignal is the ONLY thing between "the app decided to measure" and a
// loudspeaker. Every test here is a safety test; none of them needs a device,
// a thread or a clock.
#include <gtest/gtest.h>

#include "dsp/SoundcheckSignal.h"

#include <cmath>
#include <limits>
#include <vector>

namespace
{
constexpr double kSr = 48000.0;

SoundcheckSignal::Params defaultParams (float peak = SoundcheckSignal::kSoundcheckMaxPeak)
{
    SoundcheckSignal::Params p;
    p.sampleRate = kSr;
    p.peak       = peak;
    return p;
}

// Zero crossings of the sweep over a short span, converted back to a frequency.
// Counting crossings is deliberately independent of instantaneousHz(): a test
// that asked the class for the answer it is checking proves nothing.
double measuredHzAround (const SoundcheckSignal& s, std::int64_t centre, std::int64_t span)
{
    int crossings = 0;
    float prev = s.sampleAt (centre - span / 2);
    for (std::int64_t n = centre - span / 2 + 1; n < centre + span / 2; ++n)
    {
        const float cur = s.sampleAt (n);
        if ((prev < 0.0f && cur >= 0.0f) || (prev >= 0.0f && cur < 0.0f))
            ++crossings;
        prev = cur;
    }
    return (crossings / 2.0) * kSr / (double) span;
}
} // namespace

// RED IF: clampPeak() stops clamping, or the clamp is applied only at the
// setter and not at the point of use (spec §3 safety table: "a value with two
// routes to becoming wrong needs BOTH clamped -- one clamp is half a clamp",
// lane G lesson M-B). inv 1.
TEST (SoundcheckSignal, PeakIsClampedWhateverIsAsked)
{
    for (float asked : { 1.0f, 10.0f, -5.0f, std::numeric_limits<float>::quiet_NaN(),
                         std::numeric_limits<float>::infinity() })
    {
        const SoundcheckSignal s { defaultParams (asked) };

        EXPECT_LE (s.clampedPeak(), SoundcheckSignal::kSoundcheckMaxPeak) << "asked " << asked;
        EXPECT_GE (s.clampedPeak(), 0.0f) << "asked " << asked;

        for (std::int64_t n = 0; n < s.totalSamples(); n += 97)
            ASSERT_LE (std::abs (s.sampleAt (n)), SoundcheckSignal::kSoundcheckMaxPeak)
                << "asked " << asked << " at n=" << n;
    }
}

// RED IF: the raised-cosine window is dropped, or it is applied as w(n)+something
// that does not vanish at the ends. A non-zero first sample IS a click into a PA.
// inv 2.
TEST (SoundcheckSignal, FirstAndLastSampleAreExactlyZero)
{
    const SoundcheckSignal s { defaultParams() };

    EXPECT_FLOAT_EQ (s.sampleAt (0), 0.0f);
    EXPECT_FLOAT_EQ (s.sampleAt (s.totalSamples() - 1), 0.0f);
}

// RED IF: the noise-floor phase is reintroduced as a separate "emitting" flag
// instead of a negative sample index. inv 2 / inv 6.
TEST (SoundcheckSignal, NegativeIndexReturnsZero)
{
    const SoundcheckSignal s { defaultParams() };

    for (std::int64_t n : { -1LL, -100LL, -24000LL, -(std::int64_t) (0.5 * kSr) })
        EXPECT_FLOAT_EQ (s.sampleAt (n), 0.0f) << "n=" << n;

    EXPECT_FLOAT_EQ (s.sampleAt (s.totalSamples()), 0.0f);
    EXPECT_FLOAT_EQ (s.sampleAt (s.totalSamples() + 4096), 0.0f);
}

// RED IF: the exponential phase term is written with the wrong sign or the wrong
// K, which produces a sweep that runs backwards or stops short of 10 kHz. +-5%
// at each end, measured from zero crossings rather than asked of the class.
TEST (SoundcheckSignal, InstantaneousFrequencyIsMonotoneAndHitsBothEnds)
{
    const SoundcheckSignal s { defaultParams() };
    const std::int64_t total = s.totalSamples();

    // A 20 ms span at the start is long enough to hold ~2 cycles at 100 Hz.
    const double atStart = measuredHzAround (s, (std::int64_t) (0.05 * kSr), (std::int64_t) (0.020 * kSr));
    const double atEnd   = measuredHzAround (s, total - (std::int64_t) (0.05 * kSr), (std::int64_t) (0.004 * kSr));

    EXPECT_NEAR (atStart, 100.0,   100.0   * 0.05);
    EXPECT_NEAR (atEnd,   10000.0, 10000.0 * 0.05);

    double previous = 0.0;
    for (std::int64_t n = 0; n < total; n += total / 64)
    {
        const double hz = s.instantaneousHz (n);
        ASSERT_GT (hz, previous) << "not monotone at n=" << n;
        previous = hz;
    }
}

// RED IF: the fade-in is made linear or shortened. The ramp must rise from 0 to
// the full peak envelope over exactly kRampMs and never overshoot.
TEST (SoundcheckSignal, RampIsMonotoneOverThirtyMilliseconds)
{
    const SoundcheckSignal s { defaultParams() };
    const std::int64_t ramp = s.rampSamples();

    EXPECT_EQ (ramp, (std::int64_t) (SoundcheckSignal::kRampMs * kSr / 1000.0));

    // The envelope is not directly observable, so probe it by the running peak
    // over one period of the (100 Hz, 480-sample) tone at each point.
    auto envelopeNear = [&s] (std::int64_t centre)
    {
        float m = 0.0f;
        for (std::int64_t n = centre; n < centre + 480; ++n)
            m = std::max (m, std::abs (s.sampleAt (n)));
        return m;
    };

    float previous = -1.0f;
    for (std::int64_t n = 0; n + 480 < ramp; n += 480)
    {
        const float e = envelopeNear (n);
        ASSERT_GE (e, previous) << "envelope fell during the fade-in at n=" << n;
        previous = e;
    }
}

// RED IF: the ramp-out reuses the fade-in window instead of being its own
// function (F8), or its last sample is "very small" rather than EXACTLY zero.
// The callback relies on reaching exactly 0 to know the ramp is over and to
// clear scOutChannel_ itself. inv 9.
TEST (SoundcheckSignal, RampOutIsMonotoneAndReachesExactlyZero)
{
    const std::int64_t r = SoundcheckSignal::rampOutSamples (kSr);
    EXPECT_EQ (r, (std::int64_t) (SoundcheckSignal::kRampOutMs * kSr / 1000.0));

    const std::int64_t anchor = 12345;

    EXPECT_FLOAT_EQ (SoundcheckSignal::rampOut (anchor - 1, anchor, r), 1.0f);
    EXPECT_FLOAT_EQ (SoundcheckSignal::rampOut (anchor,     anchor, r), 1.0f);

    float previous = 1.0f;
    for (std::int64_t n = anchor; n < anchor + r; ++n)
    {
        const float v = SoundcheckSignal::rampOut (n, anchor, r);
        ASSERT_LE (v, previous + 1.0e-6f) << "ramp-out rose at n=" << n;
        ASSERT_GE (v, 0.0f);
        previous = v;
    }

    EXPECT_FLOAT_EQ (SoundcheckSignal::rampOut (anchor + r,       anchor, r), 0.0f);
    EXPECT_FLOAT_EQ (SoundcheckSignal::rampOut (anchor + r + 999, anchor, r), 0.0f);
}

// RED IF: the two envelopes are multiplied in the wrong order or the ramp-out is
// applied to the raw sine instead of the already-windowed sample. This is the
// composition the callback performs, asserted here once so Task 5 only has to
// wire it.
TEST (SoundcheckSignal, RampOutMultipliesTheWindowedSampleAndNeverExceedsThePeak)
{
    const SoundcheckSignal s { defaultParams() };
    const std::int64_t r      = SoundcheckSignal::rampOutSamples (kSr);
    const std::int64_t anchor = s.totalSamples() / 2;

    for (std::int64_t n = anchor; n < anchor + r + 64; ++n)
    {
        const float v = s.sampleAt (n) * SoundcheckSignal::rampOut (n, anchor, r);
        ASSERT_LE (std::abs (v), SoundcheckSignal::kSoundcheckMaxPeak);
    }

    EXPECT_FLOAT_EQ (s.sampleAt (anchor + r + 10)
                         * SoundcheckSignal::rampOut (anchor + r + 10, anchor, r), 0.0f);
}
```

- [ ] **Step 2: Run the tests and watch them fail to COMPILE**

```bash
cmake --build build --config Release
```
Expected: `Cannot open include file: 'dsp/SoundcheckSignal.h'`.

- [ ] **Step 3: Write `src/dsp/SoundcheckSignal.h`**

```cpp
// src/dsp/SoundcheckSignal.h
//
// Lane M. The log sine sweep the active soundcheck plays, as a PURE function of
// a sample index -- no state advanced by anyone, no allocation, no JUCE. That
// shape is load-bearing three ways:
//
//   1. The audio callback can evaluate it directly, so the emergency ramp-out
//      needs no other thread to still be alive (spec §4.2, invariant 9).
//   2. The lane M thread can regenerate the EXACT sequence that was emitted, to
//      use as the reference spectrum X of the loop-gain estimate (spec §4.4).
//   3. A headless test can assert any individual sample.
//
// The noise-floor phase is expressed as a NEGATIVE sample index rather than as
// a separate "emitting" flag: sampleAt(n) returns 0 for n < 0, so one gate
// cannot disagree with the other (spec §4.2).
//
// SAFETY. This class is the last thing before a loudspeaker. The peak is
// clamped in clampPeak() and the clamped value is the ONLY one stored, so there
// is no route by which an unclamped number reaches sampleAt() -- a value with
// two routes to becoming wrong needs both clamped, and one clamp is half a
// clamp (lane G M-B). The +-1.0f output clamp in AudioEngine still sits AFTER
// the injection point and must never be removed (invariant 4).
//
// Deliberately in src/dsp/ with no juce_audio_devices and no src/app/ include:
// AudioEngine.cpp includes this header from inside the realtime callback, and
// app/SoundcheckController.h aliases its constants (see that header).
#pragma once

#include <cstdint>

class SoundcheckSignal
{
public:
    // Spec §4.10. These are the DEFINITIONS; SoundcheckController.h aliases
    // them so there is one place to look up a lane M constant and still only
    // one literal per value (lane G m-D).
    static constexpr double kSweepLowHz        = 100.0;
    static constexpr double kSweepHighHz       = 10000.0;
    static constexpr double kSweepSeconds      = 3.0;
    static constexpr double kRampMs            = 30.0;
    // A SEPARATE constant from kRampMs even though the value matches: the
    // ramp-out is a different function, anchored at an arbitrary sample the
    // callback is handed, and the two must be able to move apart (F8).
    static constexpr double kRampOutMs         = 30.0;
    static constexpr float  kSoundcheckMaxPeak = 0.1f;    // -20 dBFS (Q2)
    static constexpr float  kSoundcheckMinPeak = 0.01f;   // -40 dBFS (Q2)

    // [0, kSoundcheckMaxPeak]. NaN and infinity map to 0 (silence), not to the
    // maximum: any comparison with NaN picks an arbitrary branch, so the
    // non-finite case is decided explicitly, exactly as AudioEngine.cpp:642
    // decides it for the output clamp.
    static float clampPeak (float requested) noexcept;

    struct Params
    {
        double sampleRate   = 48000.0;
        double lowHz        = kSweepLowHz;
        double highHz       = kSweepHighHz;
        double sweepSeconds = kSweepSeconds;
        double rampMs       = kRampMs;
        float  peak         = kSoundcheckMaxPeak;
    };

    explicit SoundcheckSignal (const Params& p) noexcept;

    // 0 for n < 0 and for n >= totalSamples().
    [[nodiscard]] float sampleAt (std::int64_t n) const noexcept;

    // f0 * exp(K * n / T). Reported for the marker maths and the tests; the
    // sample generator does not call it.
    [[nodiscard]] double instantaneousHz (std::int64_t n) const noexcept;

    [[nodiscard]] std::int64_t totalSamples() const noexcept { return totalSamples_; }
    [[nodiscard]] std::int64_t rampSamples()  const noexcept { return rampSamples_; }
    [[nodiscard]] float        clampedPeak()  const noexcept { return peak_; }

    // Half a raised cosine, falling from 1 at `anchor` to EXACTLY 0 at
    // `anchor + rampLengthSamples` and staying there. 1 before the anchor.
    // Static because the callback applies it to a signal it may have built from
    // a different Params instance, and because reaching exactly 0 is what tells
    // the callback the ramp is finished (invariant 9).
    static float rampOut (std::int64_t n, std::int64_t anchor,
                          std::int64_t rampLengthSamples) noexcept;

    static std::int64_t rampOutSamples (double sampleRate) noexcept;

private:
    double       sampleRate_   = 48000.0;
    double       lowHz_        = kSweepLowHz;
    double       k_            = 0.0;   // ln(f1/f0)
    double       phaseScale_   = 0.0;   // 2*pi*f0*T_s/K
    std::int64_t totalSamples_ = 0;
    std::int64_t rampSamples_  = 0;
    float        peak_         = 0.0f;
};
```

- [ ] **Step 4: Write `src/dsp/SoundcheckSignal.cpp`**

```cpp
// src/dsp/SoundcheckSignal.cpp
#include "dsp/SoundcheckSignal.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr double kPi = 3.14159265358979323846;
}

float SoundcheckSignal::clampPeak (float requested) noexcept
{
    // Decided explicitly rather than by jlimit/std::clamp: both are undefined
    // for NaN inputs, and this value drives a loudspeaker.
    if (! std::isfinite (requested))
        return 0.0f;
    return std::min (std::max (requested, 0.0f), kSoundcheckMaxPeak);
}

SoundcheckSignal::SoundcheckSignal (const Params& p) noexcept
{
    sampleRate_ = (p.sampleRate > 0.0) ? p.sampleRate : 48000.0;
    lowHz_      = (p.lowHz > 0.0)      ? p.lowHz      : kSweepLowHz;

    const double highHz = (p.highHz > lowHz_) ? p.highHz : (lowHz_ * 2.0);
    const double tS     = (p.sweepSeconds > 0.0) ? p.sweepSeconds : kSweepSeconds;

    k_            = std::log (highHz / lowHz_);
    phaseScale_   = 2.0 * kPi * lowHz_ * tS / k_;
    totalSamples_ = (std::int64_t) (tS * sampleRate_);
    rampSamples_  = (std::int64_t) (std::max (0.0, p.rampMs) * sampleRate_ / 1000.0);

    // Never more than half the sweep at each end, or the two windows overlap
    // and the "last sample is exactly zero" property stops holding.
    rampSamples_ = std::min (rampSamples_, totalSamples_ / 2);

    peak_ = clampPeak (p.peak);
}

float SoundcheckSignal::sampleAt (std::int64_t n) const noexcept
{
    if (n < 0 || n >= totalSamples_)
        return 0.0f;

    const double t   = (double) n / (double) totalSamples_;
    const double phi = phaseScale_ * (std::exp (k_ * t) - 1.0);

    // Raised cosine at BOTH ends. w(0) == 0 and w(totalSamples_-1) == 0 by
    // construction: the denominator is rampSamples_, and the index that reaches
    // it is exactly the first sample of the flat middle.
    double w = 1.0;
    if (rampSamples_ > 0)
    {
        const std::int64_t fromStart = n;
        const std::int64_t fromEnd   = totalSamples_ - 1 - n;
        const std::int64_t edge      = std::min (fromStart, fromEnd);

        if (edge < rampSamples_)
            w = 0.5 * (1.0 - std::cos (kPi * (double) edge / (double) rampSamples_));
    }

    const double v = (double) peak_ * w * std::sin (phi);

    // The peak is already clamped, w is in [0,1] and |sin| <= 1, so this cannot
    // exceed kSoundcheckMaxPeak. The clamp is here anyway because it costs one
    // instruction and this is the sample that leaves for a loudspeaker.
    return (float) std::min (std::max (v, -(double) kSoundcheckMaxPeak),
                             (double) kSoundcheckMaxPeak);
}

double SoundcheckSignal::instantaneousHz (std::int64_t n) const noexcept
{
    if (totalSamples_ <= 0)
        return lowHz_;
    const double t = (double) n / (double) totalSamples_;
    return lowHz_ * std::exp (k_ * t);
}

std::int64_t SoundcheckSignal::rampOutSamples (double sampleRate) noexcept
{
    if (! (sampleRate > 0.0))
        return 0;
    return (std::int64_t) (kRampOutMs * sampleRate / 1000.0);
}

float SoundcheckSignal::rampOut (std::int64_t n, std::int64_t anchor,
                                 std::int64_t rampLengthSamples) noexcept
{
    if (n < anchor)
        return 1.0f;
    if (rampLengthSamples <= 0 || n >= anchor + rampLengthSamples)
        return 0.0f;   // EXACTLY zero -- the callback uses this to end the run

    const double x = (double) (n - anchor) / (double) rampLengthSamples;
    return (float) (0.5 * (1.0 + std::cos (kPi * x)));
}
```

- [ ] **Step 5: Add the files to both CMake lists**

In `CMakeLists.txt`, inside `set(HANDSFREE_CORE_SOURCES` (`CMakeLists.txt:84`), append beside the other `src/dsp` entries:

```cmake
    ${CMAKE_SOURCE_DIR}/src/dsp/SoundcheckSignal.cpp
    ${CMAKE_SOURCE_DIR}/src/dsp/SoundcheckSignal.h
```

In `tests/CMakeLists.txt`, inside `add_executable(HandsFreeTests` (`tests/CMakeLists.txt:19-44`), append:

```cmake
    test_soundchecksignal.cpp
```

- [ ] **Step 6: Reconfigure, build, run**

A new header and two `CMakeLists.txt` edits mean a **full reconfigure** (CLAUDE.md build table):

```bash
cmake -B build -G "Visual Studio 18 2026" -A x64
```
```bash
cmake --build build --config Release
```
```bash
cd build && ctest -C Release -R SoundcheckSignal --output-on-failure
```
Expected: `100% tests passed` (7 tests).

- [ ] **Step 7: Full gate**

```bash
cd build && ctest -C Release
```
Expected: `100% tests passed (554)` — 547 + 7. ESTIMATE; use what ctest prints.

- [ ] **Step 8: Commit**

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add src/dsp/SoundcheckSignal.h src/dsp/SoundcheckSignal.cpp tests/test_soundchecksignal.cpp CMakeLists.txt tests/CMakeLists.txt
```
```bash
git commit -m "feat(lane-m): SoundcheckSignal -- clamped log sweep and ramp-out as pure functions"
```

---

### Task 2: `LoopGainEstimator` — energy per bin in, `H_dB[k]` out

**Mức level dự kiến (spec §3):** **0 dB.** Arithmetic only; nothing calls it until Task 6.

**What it computes** (spec §4.4), over the detector's own transform — 2048-point FFT, hop 512, Hann (`src/dsp/Detector.h:66-68`):

```
Nbar[k]  = (sum over noise-floor frames of |mic_f[k]|^2) / frames_N
EX[k]    =  sum over reference frames  of |X_f[k]|^2        (the sweep, regenerated)
EY[k]    =  sum over capture   frames  of |Y_f[k]|^2        (Sweep + Tail)
H_dB[k]  = 10*log10( max(EY[k] - Nbar[k]*frames_Y, eps) / max(EX[k], eps) )
```

**Why summing over the whole sweep needs no delay compensation, and where that argument stops being true.** This is a per-bin ENERGY RATIO, not a time correlation: the energy the sweep put into bin `k` does not depend on when it arrived, and neither does the energy the mic received in bin `k` — **provided the capture window contains both the swept part and the ringing tail**. That proviso is the whole risk, and `DecayLongerThanTheTailIsUnderRead` MEASURES the error when it fails rather than hiding it.

**What `H_dB` means.** `X` is dBFS at the app's OUTPUT, `Y` is dBFS at the app's INPUT, so `H` is the transfer function of the entire **physical** part of the loop. The loop closes through the app, and the app is **unity** at every frequency with no notch (`src/app/AudioEngine.cpp:592-624`). Therefore: **howling happens at any bin with `H_dB[k] >= 0`, and that bin's margin is `-H_dB[k]` dB.**

**Trusted band narrower than swept band** (F19). A constant-amplitude log sweep spends equal time per octave, so energy **per Hz** falls as `1/f`. With a fixed 23.4 Hz bin, a bin at 10 kHz receives exactly **20 dB** less than one at 100 Hz, and at 10 kHz the sweep crosses a bin in ~1.5 ms, far shorter than the 10.67 ms hop. With `kMinBinSnrDb = 6`, the top of the range falls out of "trusted" first. v1 therefore **sweeps 100 Hz – 10 kHz but only proposes inside `[kSweepLowHz, kTrustedHighHz = 6000]`**; 6–10 kHz is drawn with a "low confidence" label and never produces a candidate. Pre-emphasis is deferred (spec §8, the one deferred item).

**Files:**
- Create: `src/dsp/LoopGainEstimator.h`, `src/dsp/LoopGainEstimator.cpp`
- Create: `tests/test_loopgainestimator.cpp`
- Modify: `CMakeLists.txt` (`HANDSFREE_CORE_SOURCES`), `tests/CMakeLists.txt` (`add_executable(HandsFreeTests`)

**Interfaces:**
- Consumes: `SoundcheckSignal` (Task 1) — the tests regenerate the reference with it; `Detector::kFftSize` / `kHopSize` / `kNumBins` (`src/dsp/Detector.h:66-68`), `<juce_dsp/juce_dsp.h>` for `juce::dsp::FFT` and `juce::dsp::WindowingFunction`, exactly as `src/dsp/Detector.h:55` already does.
- Produces:
  ```cpp
  class LoopGainEstimator
  {
  public:
      static constexpr int    kNumBins        = Detector::kNumBins;   // 1025
      static constexpr double kTrustedHighHz  = 6000.0;
      static constexpr double kMinBandSnrDb   = 12.0;
      static constexpr double kMinBinSnrDb    = 6.0;

      explicit LoopGainEstimator (double sampleRate);

      void reset (double sampleRate);

      void pushNoiseFloor (const float* samples, int numSamples);
      void pushReference  (const float* samples, int numSamples);
      void pushCapture    (const float* samples, int numSamples);

      struct Result
      {
          std::array<float, kNumBins> hDb {};
          std::array<bool,  kNumBins> trusted {};
          float bandSnrDb    = 0.0f;
          int   noiseFrames  = 0, referenceFrames = 0, captureFrames = 0;
          bool  measured     = false;   // bandSnrDb >= kMinBandSnrDb and all three streams present
      };

      [[nodiscard]] Result finish() const;

      [[nodiscard]] static double binToHz  (int bin, double sampleRate);
      [[nodiscard]] static int    hzToBin  (double hz, double sampleRate);
  };
  ```

**Design notes an implementer needs before writing it:**

- Three independent 2048-sample sliding windows with their own write cursors, one per stream, each emitting a frame every `Detector::kHopSize` new samples. This is the same shape as `Detector`'s window (`src/dsp/Detector.h:128`), deliberately duplicated rather than reused: `Detector` reads from a `LockFreeRingBuffer` and owns detection state, and lane M needs three streams with no ring and no detection.
- One `juce::dsp::FFT { Detector::kFftOrder }` and one `juce::dsp::WindowingFunction<float>` member, both constructed once in the constructor. `performFrequencyOnlyForwardTransform` needs a scratch of `2 * kFftSize` floats — sizing it `kFftSize` overruns by 8 KB (`src/dsp/Detector.h:34-35`). Allocation happens in the constructor and in `reset` only; `push*` never allocates.
- Accumulators are `double` (`std::array<double, kNumBins>`), not float: 280 frames × 1025 bins of squared magnitudes at very different scales loses bits in float.
- `eps` is `1.0e-20`.
- `measured` is `referenceFrames > 0 && captureFrames > 0 && noiseFrames > 0 && bandSnrDb >= kMinBandSnrDb`.
- `bandSnrDb = 10*log10( sum_{k in trusted band} EY[k] / max(eps, sum_{k in trusted band} Nbar[k]*framesY) )`, the band being `[hzToBin(kSweepLowHz), hzToBin(kTrustedHighHz)]`.
- `trusted[k]` is `true` only when `binToHz(k) <= kTrustedHighHz` **and** `10*log10(EY[k] / max(eps, Nbar[k]*framesY)) >= kMinBinSnrDb`. Bins above `kTrustedHighHz` get an `hDb` value (they are DRAWN) and `trusted == false` (they are never proposed).

- [ ] **Step 1: Write the failing tests**

Create `tests/test_loopgainestimator.cpp`. The fixture is a **synthetic room**: a delay line plus an optional one-pole resonator, driven by the regenerated sweep. No device, no thread.

```cpp
// tests/test_loopgainestimator.cpp
//
// A synthetic room: delay + gain, optionally one resonance. The estimator sees
// only sample buffers, so everything here is arithmetic.
#include <gtest/gtest.h>

#include "dsp/LoopGainEstimator.h"
#include "dsp/SoundcheckSignal.h"

#include <cmath>
#include <random>
#include <vector>

namespace
{
constexpr double kSr = 48000.0;

std::vector<float> renderSweep (float peak = SoundcheckSignal::kSoundcheckMaxPeak)
{
    SoundcheckSignal::Params p;
    p.sampleRate = kSr;
    p.peak       = peak;
    const SoundcheckSignal s { p };

    std::vector<float> out ((std::size_t) s.totalSamples());
    for (std::int64_t n = 0; n < s.totalSamples(); ++n)
        out[(std::size_t) n] = s.sampleAt (n);
    return out;
}

// x delayed by `delayMs` and scaled by `gain`, rendered over
// sweep + tailSeconds so the ring-out is inside the capture window.
std::vector<float> flatRoom (const std::vector<float>& x, double delayMs, double gain,
                             double tailSeconds)
{
    const std::size_t d   = (std::size_t) (delayMs * kSr / 1000.0);
    const std::size_t len = x.size() + (std::size_t) (tailSeconds * kSr);
    std::vector<float> y (len, 0.0f);
    for (std::size_t n = 0; n < x.size(); ++n)
        if (n + d < len)
            y[n + d] += (float) (gain * x[n]);
    return y;
}

// A single resonance at `hz` with the given T60, excited by x. Implemented as a
// two-pole resonator so the decay is a real exponential, not a windowed tone.
std::vector<float> resonantRoom (const std::vector<float>& x, double hz, double t60,
                                 double gain, double tailSeconds)
{
    const std::size_t len = x.size() + (std::size_t) (tailSeconds * kSr);
    const double r  = std::pow (10.0, -3.0 / (t60 * kSr));     // per-sample decay
    const double w  = 2.0 * 3.14159265358979323846 * hz / kSr;
    const double a1 = -2.0 * r * std::cos (w);
    const double a2 = r * r;

    std::vector<float> y (len, 0.0f);
    double z1 = 0.0, z2 = 0.0;
    for (std::size_t n = 0; n < len; ++n)
    {
        const double in = (n < x.size()) ? (double) x[n] : 0.0;
        const double v  = in - a1 * z1 - a2 * z2;
        y[n] = (float) (gain * (v - z2) * (1.0 - r));
        z2 = z1;
        z1 = v;
    }
    return y;
}

std::vector<float> noise (std::size_t n, float sigma, unsigned seed)
{
    std::mt19937 rng { seed };
    std::normal_distribution<float> d { 0.0f, sigma };
    std::vector<float> out (n);
    for (auto& v : out) v = d (rng);
    return out;
}

LoopGainEstimator::Result runRoom (const std::vector<float>& reference,
                                   const std::vector<float>& captured,
                                   float noiseSigma, unsigned seed = 7)
{
    LoopGainEstimator est { kSr };
    const auto floorNoise = noise ((std::size_t) (0.5 * kSr), noiseSigma, seed);
    est.pushNoiseFloor (floorNoise.data(), (int) floorNoise.size());
    est.pushReference  (reference.data(),  (int) reference.size());

    // The captured stream carries the same noise floor on top of the room.
    auto withNoise = captured;
    const auto n2 = noise (withNoise.size(), noiseSigma, seed + 1);
    for (std::size_t i = 0; i < withNoise.size(); ++i)
        withNoise[i] += n2[i];

    est.pushCapture (withNoise.data(), (int) withNoise.size());
    return est.finish();
}

// Mean hDb over [200 Hz, 4 kHz] -- inside the trusted band, clear of both
// window edges of the sweep.
double meanMidBandDb (const LoopGainEstimator::Result& r)
{
    const int lo = LoopGainEstimator::hzToBin (200.0,  kSr);
    const int hi = LoopGainEstimator::hzToBin (4000.0, kSr);
    double sum = 0.0; int n = 0;
    for (int k = lo; k <= hi; ++k) { sum += r.hDb[(std::size_t) k]; ++n; }
    return sum / n;
}
} // namespace

// RED IF: the noise-floor subtraction, the per-frame normalisation, or the
// 10*log10 (vs 20*log10 -- these are ENERGIES, not amplitudes) is wrong.
// A gain of 0.5 in amplitude is -6.02 dB in power.
TEST (LoopGainEstimator, FlatRoomMeasuresFlatResponse)
{
    const auto x = renderSweep();
    const auto y = flatRoom (x, 15.0, 0.5, 0.7);

    const auto r = runRoom (x, y, 1.0e-4f);

    EXPECT_TRUE (r.measured);
    EXPECT_NEAR (meanMidBandDb (r), -6.02, 1.0);
}

// RED IF: binToHz/hzToBin use kNumBins instead of kFftSize, which shifts every
// frequency by a factor of ~2.
TEST (LoopGainEstimator, ResonanceLandsInTheRightBin)
{
    const auto x = renderSweep();
    const auto y = resonantRoom (x, 1000.0, 0.35, 1.0, 0.7);

    const auto r = runRoom (x, y, 1.0e-4f);
    ASSERT_TRUE (r.measured);

    const int expected = LoopGainEstimator::hzToBin (1000.0, kSr);
    int peak = expected;
    for (int k = LoopGainEstimator::hzToBin (300.0, kSr);
         k <= LoopGainEstimator::hzToBin (3000.0, kSr); ++k)
        if (r.hDb[(std::size_t) k] > r.hDb[(std::size_t) peak])
            peak = k;

    EXPECT_LE (std::abs (peak - expected), 1) << "peak bin " << peak << " expected " << expected;
}

// RED IF: someone "fixes" the estimator by cross-correlating or by windowing the
// capture relative to the reference. 900 ms is LONGER than kTailSeconds = 0.7,
// so it genuinely tests the boundary; rev 1 of the spec chose 5 ms and 200 ms,
// both of which fit inside the tail and so could never go red (F18).
TEST (LoopGainEstimator, DelayDoesNotChangeTheAnswer)
{
    const auto x = renderSweep();

    // The capture window is sweep + tail. A 900 ms delay pushes the LAST part of
    // the sweep past the tail, so the answer is allowed to be a little low --
    // but it must not swing with delay the way a time-aligned method would.
    const auto quick = runRoom (x, flatRoom (x,   5.0, 0.5, 0.7), 1.0e-4f);
    const auto slow  = runRoom (x, flatRoom (x, 900.0, 0.5, 1.6), 1.0e-4f);

    ASSERT_TRUE (quick.measured);
    ASSERT_TRUE (slow.measured);

    EXPECT_NEAR (meanMidBandDb (quick), -6.02, 1.0);
    EXPECT_NEAR (meanMidBandDb (slow),  -6.02, 1.0);
}

// RED IF: truncation is hidden instead of measured. A T60 of 2.0 s dumps most of
// its energy after the 0.7 s tail, so hDb MUST read low -- and by roughly the
// fraction of energy that was cut off. An implementation that "corrected" for
// this without measuring the room would pass FlatRoom and fail here.
TEST (LoopGainEstimator, DecayLongerThanTheTailIsUnderRead)
{
    const auto x = renderSweep();

    const auto full  = runRoom (x, resonantRoom (x, 1000.0, 2.0, 1.0, 6.0), 1.0e-4f);
    const auto short_ = runRoom (x, resonantRoom (x, 1000.0, 2.0, 1.0, SoundcheckSignal::kSweepSeconds * 0.0 + 0.7), 1.0e-4f);

    ASSERT_TRUE (full.measured);
    ASSERT_TRUE (short_.measured);

    const int k = LoopGainEstimator::hzToBin (1000.0, kSr);
    const double lost = full.hDb[(std::size_t) k] - short_.hDb[(std::size_t) k];

    // The truncated read must be LOWER, and the size of the shortfall must match
    // the energy actually cut off, within 1.5 dB. Both halves matter: "lower" on
    // its own would pass for an estimator that simply reads too low everywhere.
    EXPECT_GT (lost, 0.0);
    EXPECT_LT (lost, 12.0);
    EXPECT_NEAR (lost, 3.0, 1.5)
        << "truncation error moved; re-derive it before editing this number";
}

// RED IF: Nbar is added instead of subtracted, or is not scaled by framesY.
TEST (LoopGainEstimator, NoiseFloorIsSubtracted)
{
    const auto x = renderSweep();
    const auto y = flatRoom (x, 15.0, 0.5, 0.7);

    const auto quiet = runRoom (x, y, 1.0e-5f);
    const auto noisy = runRoom (x, y, 3.0e-4f);

    ASSERT_TRUE (quiet.measured);
    ASSERT_TRUE (noisy.measured);

    // Same room, 30x the noise: after subtraction the mid-band answer must still
    // be the room's, not the noise's.
    EXPECT_NEAR (meanMidBandDb (noisy), meanMidBandDb (quiet), 1.5);
}

// RED IF: trusted[] is set from the band alone and ignores per-bin SNR.
TEST (LoopGainEstimator, BinBelowSnrIsMarkedUntrusted)
{
    const auto x = renderSweep();
    // A room 60 dB down: every bin drowns in the floor.
    const auto y = flatRoom (x, 15.0, 0.001, 0.7);

    const auto r = runRoom (x, y, 3.0e-3f);

    int trustedCount = 0;
    for (int k = LoopGainEstimator::hzToBin (200.0, kSr);
         k <= LoopGainEstimator::hzToBin (4000.0, kSr); ++k)
        if (r.trusted[(std::size_t) k]) ++trustedCount;

    EXPECT_EQ (trustedCount, 0);
    EXPECT_FALSE (r.measured) << "band SNR below kMinBandSnrDb must not read as measured";
}

// RED IF: kTrustedHighHz is ignored, or applied as a DRAW limit instead of a
// PROPOSE limit -- the bins above it must still carry an hDb value. F19.
TEST (LoopGainEstimator, AboveTrustedHighHzIsDrawnButNeverTrusted)
{
    const auto x = renderSweep();
    const auto y = resonantRoom (x, 8000.0, 0.4, 4.0, 0.7);   // very hot at 8 kHz

    const auto r = runRoom (x, y, 1.0e-5f);
    ASSERT_TRUE (r.measured);

    const int k8 = LoopGainEstimator::hzToBin (8000.0, kSr);
    EXPECT_FALSE (r.trusted[(std::size_t) k8]);
    EXPECT_NE (r.hDb[(std::size_t) k8], 0.0f) << "above 6 kHz must still be DRAWN";

    for (int k = LoopGainEstimator::hzToBin (LoopGainEstimator::kTrustedHighHz + 100.0, kSr);
         k < LoopGainEstimator::kNumBins; ++k)
        ASSERT_FALSE (r.trusted[(std::size_t) k]) << "bin " << k;
}

// RED IF: hzToBin/binToHz stop being inverses, which silently moves every marker
// and every proposed notch frequency.
TEST (LoopGainEstimator, BinAndHzRoundTrip)
{
    for (double hz : { 100.0, 250.0, 1000.0, 4000.0, 6000.0, 10000.0 })
    {
        const int k = LoopGainEstimator::hzToBin (hz, kSr);
        EXPECT_NEAR (LoopGainEstimator::binToHz (k, kSr), hz, kSr / Detector::kFftSize);
    }
    EXPECT_EQ (LoopGainEstimator::hzToBin (0.0, kSr), 0);
    EXPECT_LT (LoopGainEstimator::hzToBin (1.0e9, kSr), LoopGainEstimator::kNumBins);
}
```

- [ ] **Step 2: Run and watch it fail to compile**

```bash
cmake --build build --config Release
```
Expected: `Cannot open include file: 'dsp/LoopGainEstimator.h'`.

- [ ] **Step 3: Write the header and implementation**

`src/dsp/LoopGainEstimator.h` carries the declaration in the Interfaces block above plus these private members, and a header comment stating the delay argument and the truncation caveat in the same words as spec §4.4:

```cpp
private:
    struct Stream
    {
        std::array<float, Detector::kFftSize> window {};
        int  fill = 0;               // samples in `window`
        int  sinceLastFrame = 0;     // new samples since the last emitted frame
        int  frames = 0;
    };

    void pushInto (Stream& s, std::array<double, kNumBins>& accum,
                   const float* samples, int numSamples);
    void analyseFrame (const Stream& s, std::array<double, kNumBins>& accum);

    double sampleRate_ = 48000.0;
    juce::dsp::FFT fft_ { Detector::kFftOrder };
    juce::dsp::WindowingFunction<float> hann_
        { (std::size_t) Detector::kFftSize, juce::dsp::WindowingFunction<float>::hann, false };
    std::vector<float> scratch_;      // 2 * kFftSize, allocated once (Detector.h:34-35)

    Stream noiseStream_, referenceStream_, captureStream_;
    std::array<double, kNumBins> noiseAccum_ {}, referenceAccum_ {}, captureAccum_ {};
```

`pushInto` slides the window by `Detector::kHopSize` and calls `analyseFrame` each time `sinceLastFrame >= kHopSize`; `analyseFrame` copies the window into `scratch_`, applies `hann_`, calls `fft_.performFrequencyOnlyForwardTransform (scratch_.data())` and accumulates `scratch_[k] * scratch_[k]` for `k < kNumBins`. `reset(sampleRate)` zeroes every stream and accumulator and re-stores `sampleRate_`; it does not reallocate.

`binToHz (bin, sr) = bin * sr / Detector::kFftSize`. `hzToBin (hz, sr) = clamp(round(hz * kFftSize / sr), 0, kNumBins - 1)`.

`finish()` builds `Result` exactly as the four formulas at the head of this task say, in that order, with the unit of both sides of every comparison named in a comment (`EY` and `Nbar*framesY` are both SUMMED SQUARED MAGNITUDES; `bandSnrDb` and `kMinBandSnrDb` are both dB).

- [ ] **Step 4: Add to both CMake lists**

```cmake
    ${CMAKE_SOURCE_DIR}/src/dsp/LoopGainEstimator.cpp
    ${CMAKE_SOURCE_DIR}/src/dsp/LoopGainEstimator.h
```
```cmake
    test_loopgainestimator.cpp
```

- [ ] **Step 5: Reconfigure, build, run**

```bash
cmake -B build -G "Visual Studio 18 2026" -A x64
```
```bash
cmake --build build --config Release
```
```bash
cd build && ctest -C Release -R LoopGainEstimator --output-on-failure
```
Expected: `100% tests passed` (8 tests).

**If `DecayLongerThanTheTailIsUnderRead`'s 3.0 dB is not what the synthetic room produces, do NOT edit the number to match.** Derive it: with a per-sample decay `r` and a T60 of 2.0 s, the fraction of the resonator's energy captured inside a 0.7 s tail is `1 - 10^(-3 * 0.7/2.0 * 2)`, and the shortfall is `-10*log10` of it. Put the derivation in the comment beside the number, as lane G's B-4 lesson requires ("write the arithmetic in the comment or it drifts").

- [ ] **Step 6: Full gate and commit**

```bash
cd build && ctest -C Release
```
Expected: `100% tests passed (562)` — 554 + 8. ESTIMATE.

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add src/dsp/LoopGainEstimator.h src/dsp/LoopGainEstimator.cpp tests/test_loopgainestimator.cpp CMakeLists.txt tests/CMakeLists.txt
```
```bash
git commit -m "feat(lane-m): LoopGainEstimator -- per-bin loop gain with a measured truncation bound"
```

---

### Task 3: `SoundcheckCandidates` — mark, propose, quantise, and say when it was not enough

**Mức level dự kiến (spec §3):** **0 dB.** Arithmetic only. But this task decides the depth of every cut a later `ÁP DỤNG` makes, so the depth rule is asserted against **all six worked examples from spec §4.4 as exact numbers**, not as inequalities.

**The rule** (spec §4.4, as corrected by F6 and N2 — rev 1 had the sign backwards and rev 2 stated the quantiser backwards):

```
needed_dB  = H_dB[k] + kTargetMarginDb        // dB that must be CUT; > 0 when a cut is really needed
if needed_dB < kMinUsefulCutDb   ->  MARK only, no proposal
depth_raw  = -needed_dB                        // always <= -kMinUsefulCutDb, i.e. < 0
rung       = the SHALLOWEST rung of kDepthLadderDb that is AT LEAST AS DEEP as depth_raw
             (the shallowest r satisfying r <= depth_raw)
             no rung qualifies  ->  rung = kMaxDepthDb
depth      = max(rung, ceiling)                // a shallower preset ceiling WINS
                                               // (this IS Q13's "the ceiling is the last rung")
depth      = max(depth, kMaxDepthDb)           // -24, lane G invariant 1
residualDb = max(0, needed_dB - (-depth))      // dB still missing after the cut
saturated  = residualDb > 0
```

Re-checked against every example in spec §4.4, and these are the assertions in Step 1:

| `H_dB` | `needed` | `depth_raw` | `rung` | ceiling | `depth` | `residualDb` | |
|---|---|---|---|---|---|---|---|
| +2 | 8 | −8 | **−12** (shallowest of {−12,−18,−24}) | −24 | **−12** | 0 | `DepthSignIsNegative` |
| +2 | 8 | −8 | −12 | **−10** | **−10** | 0 | the ceiling is the last rung (Q13) |
| −1 | 5 | −5 | **−6** | −24 | **−6** | 0 | `DepthQuantisesOntoTheLadder` |
| +30 | 36 | −36 | none ⇒ **−24** | −24 | **−24** | **12** | saturated at the floor |
| −5.9 | 0.1 | — | — | — | — | — | below `kMinUsefulCutDb` ⇒ **MARK only** |
| +14 | 20 | −20 | −24 | **−10** | **−10** | **10** | saturated by the **ceiling**, not by −24 |

**Why `saturated` must be reported and not swallowed:** a silent −24 on a bin that needed −36 is a false promise. `OutputResult` carries `saturatedBins` and per-candidate `residualDb`, the GUI says *"còn vượt X dB sau khi cắt sâu nhất — chỉnh gain, hạ trần, hoặc đổi vị trí mic"*, and `soundcheck_output` logs `saturated: true` with `residual_db`.

**Why the mark threshold and the propose threshold are different numbers** (F22): `kCandidateMarginDb = -6.0` marks; `kMinUsefulCutDb = 3.0` proposes. A bin at `H_dB = -5.9` needs 0.1 dB — the operator should SEE it, but spending a −6 dB notch and one of sixteen chain slots on it over-cuts by 5.9 dB. Because a proposal needs `needed >= 3`, the −6 rung can never over-cut by more than 3 dB, and rev 1's `needed == 0` case is now unreachable.

**Files:**
- Create: `src/dsp/SoundcheckCandidates.h`, `src/dsp/SoundcheckCandidates.cpp`
- Create: `tests/test_soundcheck_candidates.cpp`
- Modify: `CMakeLists.txt` (`HANDSFREE_CORE_SOURCES`), `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `LoopGainEstimator::kNumBins`, `binToHz`, `hzToBin`, `kTrustedHighHz` (Task 2). **Not** `NotchController` — see the parameter note below.
- Produces:
  ```cpp
  class SoundcheckCandidates
  {
  public:
      static constexpr int    kNumBins              = LoopGainEstimator::kNumBins;
      static constexpr double kCandidateMarginDb    = -6.0;   // MARK threshold
      static constexpr double kMinUsefulCutDb       =  3.0;   // PROPOSE threshold
      static constexpr double kMinProminenceDb      =  6.0;
      static constexpr double kTargetMarginDb       =  6.0;
      static constexpr int    kMaxPreventivePerLane =  6;

      struct Ladder                     // supplied by the caller; see the note
      {
          const double* rungsDb = nullptr;
          int           count   = 0;
          double        maxDepthDb = -24.0;
      };

      struct Depth { double depthDb = 0.0; double residualDb = 0.0; bool saturated = false; };

      // The whole depth rule, on one H_dB value. Public so the six worked
      // examples of spec §4.4 can be asserted directly instead of inferred from
      // a full pick() run.
      [[nodiscard]] static Depth depthFor (double hDb, double ceilingDb, const Ladder& ladder);

      struct Input
      {
          const float* hDb        = nullptr;      // kNumBins, from LoopGainEstimator
          const bool*  trusted    = nullptr;      // kNumBins
          double sampleRate       = 0.0;
          double ceilingDb        = 0.0;          // the running preset's ceiling, <= 0
          double notchQ           = 0.0;
          const float* liveNotchHz = nullptr;     // frequencies of notches already live on this lane
          int    liveNotchCount    = 0;
          Ladder ladder {};
      };

      struct Candidate
      {
          float hz = 0.0f, marginDb = 0.0f, depthDb = 0.0f, q = 0.0f, residualDb = 0.0f;
          int   bin = 0;
      };

      struct Output
      {
          std::array<Candidate, kMaxPreventivePerLane> candidates {};
          int candidateCount = 0, markedCount = 0, saturatedBins = 0;
          std::array<bool, kNumBins> marked {};
      };

      [[nodiscard]] static Output pick (const Input& in);

      // 1/3-octave moving average of hDb, exposed for the tests.
      static void smooth (const float* hDb, float* out, int numBins, double sampleRate);
  };
  ```

**Why the ladder is a parameter and not an include.** `kDepthLadderDb` / `kDepthLadderSize` / `kMaxDepthDb` are lane G's, declared on `NotchController` (`src/app/NotchController.h:109`, `:110`, `:114`). Including `app/NotchController.h` from `src/dsp/` would invert the layering and drag in `app/PresetManager.h` and `juce_events`. Copying the four numbers would create the second-literal problem lane G spent m-D removing. So the caller passes them: `SoundcheckController` (an `app/` class) builds `Ladder { NotchController::kDepthLadderDb, NotchController::kDepthLadderSize, NotchController::kMaxDepthDb }`, and the TESTS do the same — so the six examples are asserted against **the shipped ladder**, not a copy of it.

**The pick order** (spec §4.4), in this order, because reordering changes results:
1. drop every bin outside `[kSweepLowHz, kTrustedHighHz]`;
2. drop every bin with `trusted[k] == false`;
3. drop every bin within **±1 bin** of a frequency in `liveNotchHz` (inv 15);
4. smooth `hDb` with a 1/3-octave moving average into `hs`;
5. **MARK** when `hDb[k] >= kCandidateMarginDb` **and** `hDb[k] - hs[k] >= kMinProminenceDb`;
6. **PROPOSE** only a marked bin whose `depthFor(...)` returned a proposal (i.e. `needed >= kMinUsefulCutDb`);
7. sort by `hDb` descending, keep at most `kMaxPreventivePerLane`.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_soundcheck_candidates.cpp`:

```cpp
// tests/test_soundcheck_candidates.cpp
//
// The depth rule decides how deep a cut lands on a live PA, so every one of
// spec §4.4's six worked examples is asserted as an EXACT number here. The
// ladder comes from NotchController, not from a copy: a test that quantises
// against its own array proves nothing about the app.
#include <gtest/gtest.h>

#include "app/NotchController.h"
#include "dsp/SoundcheckCandidates.h"

#include <cmath>
#include <vector>

namespace
{
constexpr double kSr = 48000.0;

SoundcheckCandidates::Ladder shippedLadder()
{
    return { NotchController::kDepthLadderDb,
             NotchController::kDepthLadderSize,
             NotchController::kMaxDepthDb };
}

struct Field
{
    std::vector<float> hDb  = std::vector<float> (SoundcheckCandidates::kNumBins, -40.0f);
    std::vector<char>  trust = std::vector<char> (SoundcheckCandidates::kNumBins, 1);

    void poke (double hz, float value)
    {
        hDb[(std::size_t) LoopGainEstimator::hzToBin (hz, kSr)] = value;
    }

    SoundcheckCandidates::Input input (double ceilingDb = -24.0) const
    {
        SoundcheckCandidates::Input in;
        in.hDb        = hDb.data();
        in.trusted    = reinterpret_cast<const bool*> (trust.data());
        in.sampleRate = kSr;
        in.ceilingDb  = ceilingDb;
        in.notchQ     = 30.0;
        in.ladder     = shippedLadder();
        return in;
    }
};
} // namespace

// RED IF: the sign of `needed` is flipped back to rev 1's -(H_dB + 6). That
// produces a POSITIVE depth, and setNotchImpl refuses depth > 0 outright
// (NotchController.cpp:214) -- so the whole feature would place nothing, in
// silence. F6.
TEST (SoundcheckCandidates, DepthSignIsNegative)
{
    const auto d = SoundcheckCandidates::depthFor (2.0, -24.0, shippedLadder());

    EXPECT_DOUBLE_EQ (d.depthDb, -12.0);
    EXPECT_DOUBLE_EQ (d.residualDb, 0.0);
    EXPECT_FALSE (d.saturated);

    for (double h = -6.0; h <= 30.0; h += 0.25)
    {
        const auto x = SoundcheckCandidates::depthFor (h, -24.0, shippedLadder());
        if (x.depthDb != 0.0)
        {
            ASSERT_LT (x.depthDb, 0.0)  << "H_dB=" << h;
            ASSERT_GE (x.depthDb, NotchController::kMaxDepthDb) << "H_dB=" << h;
        }
    }
}

// RED IF: the quantiser is stated backwards ("the DEEPEST rung not deeper than
// depth_raw"), which rev 2 of the spec did: that gives -6 for a -8 requirement
// and an EMPTY set for -5. N2.
TEST (SoundcheckCandidates, DepthQuantisesOntoTheLadder)
{
    EXPECT_DOUBLE_EQ (SoundcheckCandidates::depthFor (-1.0, -24.0, shippedLadder()).depthDb,  -6.0);
    EXPECT_DOUBLE_EQ (SoundcheckCandidates::depthFor ( 2.0, -24.0, shippedLadder()).depthDb, -12.0);
    EXPECT_DOUBLE_EQ (SoundcheckCandidates::depthFor ( 8.0, -24.0, shippedLadder()).depthDb, -18.0);
    EXPECT_DOUBLE_EQ (SoundcheckCandidates::depthFor (14.0, -24.0, shippedLadder()).depthDb, -24.0);
}

// RED IF: the ceiling is applied with min() instead of max(), or applied before
// the quantisation. Deeper is MORE NEGATIVE, so a shallower ceiling wins with
// std::max -- the same convention lane G uses at every clamp.
TEST (SoundcheckCandidates, CeilingClampsTheProposal)
{
    const auto d = SoundcheckCandidates::depthFor (2.0, -10.0, shippedLadder());
    EXPECT_DOUBLE_EQ (d.depthDb, -10.0);
    EXPECT_DOUBLE_EQ (d.residualDb, 0.0);
}

// RED IF: someone quantises the CEILING onto a rung. presets/Music.json ships
// -10.0 (read 2026-09-15); quantising it to -6 makes Music 4 dB shallower than
// 1.1.3 with nobody reporting it -- this is lane G's Q13 defect, and lane M
// must not reintroduce it. inv 14, second half.
TEST (SoundcheckCandidates, CeilingNotMultipleOfSixEndsOnTheCeiling)
{
    const auto d = SoundcheckCandidates::depthFor (2.0, -10.0, shippedLadder());
    EXPECT_DOUBLE_EQ (d.depthDb, -10.0);
    EXPECT_NE (d.depthDb, -6.0);
    EXPECT_NE (d.depthDb, -12.0);
}

// RED IF: saturation is swallowed. A -24 on a bin that needed -36 is a promise
// the app cannot keep, and the operator has to be told.
TEST (SoundcheckCandidates, SaturatesAtMinusTwentyFourAndReportsResidual)
{
    const auto d = SoundcheckCandidates::depthFor (30.0, -24.0, shippedLadder());

    EXPECT_DOUBLE_EQ (d.depthDb, -24.0);
    EXPECT_NEAR (d.residualDb, 12.0, 1.0e-9);
    EXPECT_TRUE (d.saturated);

    Field f;
    f.poke (1000.0, 30.0f);
    const auto out = SoundcheckCandidates::pick (f.input (-24.0));
    EXPECT_EQ (out.saturatedBins, 1);
}

// RED IF: the ceiling path forgets to report residual. This is the SECOND way a
// proposal can come up short, and spec §4.4 requires both to be named.
TEST (SoundcheckCandidates, SaturationByTheCeilingIsAlsoReported)
{
    const auto d = SoundcheckCandidates::depthFor (14.0, -10.0, shippedLadder());

    EXPECT_DOUBLE_EQ (d.depthDb, -10.0);
    EXPECT_NEAR (d.residualDb, 10.0, 1.0e-9);
    EXPECT_TRUE (d.saturated);
}

// RED IF: kMinUsefulCutDb is dropped and kCandidateMarginDb is used for both
// jobs. A bin needing 0.1 dB would then eat a -6 dB notch and one of sixteen
// chain slots. F22.
TEST (SoundcheckCandidates, MarkedButNotProposedBelowMinUsefulCut)
{
    Field f;
    f.poke (1000.0, -5.9f);

    const auto out = SoundcheckCandidates::pick (f.input());

    EXPECT_EQ (out.markedCount, 1);
    EXPECT_EQ (out.candidateCount, 0);
    EXPECT_TRUE (out.marked[(std::size_t) LoopGainEstimator::hzToBin (1000.0, kSr)]);
}

// RED IF: prominence is dropped. A loudspeaker's own high-frequency roll-off is
// a smooth slope with no modes in it and must produce NOTHING.
TEST (SoundcheckCandidates, SpeakerRolloffIsNotACandidate)
{
    Field f;
    for (int k = 0; k < SoundcheckCandidates::kNumBins; ++k)
    {
        const double hz = LoopGainEstimator::binToHz (k, kSr);
        f.hDb[(std::size_t) k] = (float) (6.0 - 12.0 * std::log10 (std::max (hz, 20.0) / 100.0));
    }

    const auto out = SoundcheckCandidates::pick (f.input());
    EXPECT_EQ (out.candidateCount, 0);
}

// RED IF: the +-1 bin exclusion is dropped or narrowed to exact equality.
// Notching a bin that already has a live notch stacks two cuts on one mode.
// inv 15.
TEST (SoundcheckCandidates, BinWithALiveNotchIsSkipped)
{
    Field f;
    f.poke (1000.0, 10.0f);

    auto in = f.input();
    // One bin BELOW the hot bin -- the +-1 window must still exclude it.
    const float liveHz = (float) LoopGainEstimator::binToHz (
        LoopGainEstimator::hzToBin (1000.0, kSr) - 1, kSr);
    in.liveNotchHz    = &liveHz;
    in.liveNotchCount = 1;

    const auto out = SoundcheckCandidates::pick (in);
    EXPECT_EQ (out.candidateCount, 0);
}

// RED IF: kMaxPreventivePerLane stops being enforced, or the survivors are the
// first six found rather than the six hottest.
TEST (SoundcheckCandidates, AtMostSixPerLaneAndTheHottestSurvive)
{
    Field f;
    const double hz[] = { 200.0, 400.0, 630.0, 1000.0, 1600.0, 2500.0, 4000.0, 5000.0 };
    const float  hot[] = { 1.0f, 20.0f, 3.0f, 18.0f, 5.0f, 16.0f, 7.0f, 14.0f };
    for (int i = 0; i < 8; ++i)
        f.poke (hz[i], hot[i]);

    const auto out = SoundcheckCandidates::pick (f.input());

    EXPECT_EQ (out.candidateCount, SoundcheckCandidates::kMaxPreventivePerLane);
    EXPECT_GE (out.candidates[0].marginDb, out.candidates[1].marginDb) << "not sorted";

    // The two coldest (1.0 and 3.0) must be the ones dropped.
    for (int i = 0; i < out.candidateCount; ++i)
        ASSERT_GE (out.candidates[i].marginDb, 5.0f);
}

// RED IF: an untrusted bin can still become a candidate. The 6-10 kHz band is
// drawn, never proposed. F19 / Task 2.
TEST (SoundcheckCandidates, UntrustedBinIsNeverACandidate)
{
    Field f;
    f.poke (8000.0, 25.0f);
    f.trust[(std::size_t) LoopGainEstimator::hzToBin (8000.0, kSr)] = 0;

    const auto out = SoundcheckCandidates::pick (f.input());
    EXPECT_EQ (out.candidateCount, 0);
}

// RED IF: marginDb is published as H_dB instead of -H_dB. The GUI labels this
// column "margin", and a sign error there reads as "this room is fine".
TEST (SoundcheckCandidates, MarginIsTheNegativeOfLoopGain)
{
    Field f;
    f.poke (1000.0, 9.0f);

    const auto out = SoundcheckCandidates::pick (f.input());
    ASSERT_EQ (out.candidateCount, 1);
    EXPECT_NEAR (out.candidates[0].marginDb, -9.0f, 1.0e-4f);
    EXPECT_NEAR (out.candidates[0].hz, 1000.0f, (float) (kSr / Detector::kFftSize));
    EXPECT_DOUBLE_EQ (out.candidates[0].depthDb, -18.0);
}
```

- [ ] **Step 2: Run and watch it fail to compile**

```bash
cmake --build build --config Release
```
Expected: `Cannot open include file: 'dsp/SoundcheckCandidates.h'`.

- [ ] **Step 3: Write the header and implementation**

`depthFor` is a direct transcription of the rule block at the head of this task, with the unit of every comparison in a comment:

```cpp
SoundcheckCandidates::Depth
SoundcheckCandidates::depthFor (double hDb, double ceilingDb, const Ladder& ladder)
{
    Depth out;

    // Both sides dB. needed_dB > 0 means "this many dB must come out".
    const double needed = hDb + kTargetMarginDb;
    if (needed < kMinUsefulCutDb)
        return out;                        // MARK only; depthDb stays 0 and means "no proposal"

    const double depthRaw = -needed;       // <= -kMinUsefulCutDb, so strictly negative

    // The SHALLOWEST rung at least as deep as depthRaw. Deeper == more negative,
    // so "at least as deep" is r <= depthRaw, and "shallowest" is the largest
    // such r. Getting this backwards is spec rev 2's N2 defect.
    double rung = ladder.maxDepthDb;
    bool   found = false;
    for (int i = 0; i < ladder.count; ++i)
    {
        const double r = ladder.rungsDb[i];
        if (r <= depthRaw && (! found || r > rung))
        {
            rung  = r;
            found = true;
        }
    }
    if (! found)
        rung = ladder.maxDepthDb;          // nothing deep enough -> saturate at the floor

    double depth = std::max (rung, ceilingDb);      // a SHALLOWER ceiling wins (Q13)
    depth        = std::max (depth, ladder.maxDepthDb);   // lane G invariant 1

    out.depthDb    = depth;
    out.residualDb = std::max (0.0, needed - (-depth));
    out.saturated  = out.residualDb > 0.0;
    return out;
}
```

`smooth` is a moving average over `[k / 2^(1/6), k * 2^(1/6)]` in Hz (one third of an octave, centred), clamped to `[0, kNumBins)`. `pick` runs the seven steps in the order listed above, filling `marked`, `markedCount`, `candidateCount`, `saturatedBins` and the sorted `candidates` array; `q` on each candidate is `in.notchQ` verbatim, and `hz` is `LoopGainEstimator::binToHz (bin, in.sampleRate)`.

- [ ] **Step 4: Add to both CMake lists, reconfigure, build, run**

```cmake
    ${CMAKE_SOURCE_DIR}/src/dsp/SoundcheckCandidates.cpp
    ${CMAKE_SOURCE_DIR}/src/dsp/SoundcheckCandidates.h
```
```cmake
    test_soundcheck_candidates.cpp
```
```bash
cmake -B build -G "Visual Studio 18 2026" -A x64
```
```bash
cmake --build build --config Release
```
```bash
cd build && ctest -C Release -R SoundcheckCandidates --output-on-failure
```
Expected: `100% tests passed` (12 tests).

- [ ] **Step 5: Full gate and commit**

```bash
cd build && ctest -C Release
```
Expected: `100% tests passed (574)` — 562 + 12. ESTIMATE.

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add src/dsp/SoundcheckCandidates.h src/dsp/SoundcheckCandidates.cpp tests/test_soundcheck_candidates.cpp CMakeLists.txt tests/CMakeLists.txt
```
```bash
git commit -m "feat(lane-m): SoundcheckCandidates -- mark/propose split and the ladder depth rule"
```

---

### Task 4: the two additive `NotchController` changes, and the reason name that reaches the log

**Mức level dự kiến (spec §3):** **0 dB.** One new field on a snapshot struct and one new enumerator. No behaviour changes; `setNotch`, `clearNotch` and the release ladder are untouched.

**Why these two, and only these two** (spec §4.6, §6): `SnapshotNotch` today carries `frequency, Q, depthDB, deepestDb, channel, index` and no `origin` (`src/app/NotchController.h:394-406`), and `model_` is private. So there is **no route** by which anything outside the controller can tell which notch came from a soundcheck — which makes spec §4.6(b), "clear the previous run's preventive notches", impossible. Skipping that step is not an option either: `Origin::Soundcheck` notches are exempt from auto-release (`src/app/NotchController.cpp:729`, `:1322`), so the sixteen-slot chain would drain after a few soundchecks. And without `ClearReason::SoundcheckReplace` that cleanup logs `reason: manual` (`src/app/MainComponent.cpp:608` via `reasonName`) and is indistinguishable from an operator removing a notch by hand — exactly the mislabelling lane D exists to prevent.

**Files:**
- Modify: `src/app/NotchController.h` — the `ClearReason` enumerator list (anchor text: `Manual, ClearAll, AutoRelease, WidthChange, VerdictFalse, PartialApplyUnwind`, `src/app/NotchController.h:69`) and `struct SnapshotNotch` (anchor text: `std::uint8_t index   = 0;`, `src/app/NotchController.h:394-406`). Numbers are pre-Task-4.
- Modify: `src/app/NotchController.cpp` — the ONE aggregate initialiser that fills the snapshot list (anchor text: `notchList[notchCount++] = { (float) n.frequency, (float) n.Q,`, `src/app/NotchController.cpp:578-580`)
- Modify: `src/app/MainComponent.cpp` — `reasonName` (anchor text: `case NotchController::ClearReason::PartialApplyUnwind: return "partial_apply_unwind";`, `src/app/MainComponent.cpp:74`)
- Modify: `tools/logstats.py` — a `soundcheck_replace` tally and `--expect-soundcheck-replaced`
- Modify: `tests/fixtures/session-sample.jsonl` — one `notch_clear` carrying `reason: "soundcheck_replace"`
- Modify: `tests/CMakeLists.txt:113-117` — the `logstats_fixture` argument list
- Test: `tests/test_notchcontroller.cpp` (append at the end of the file, OUTSIDE any anonymous namespace — lane G m-E: a helper appended after `namespace { ... }` closes lands in global scope)

**Interfaces:**
- Produces:
  ```cpp
  enum class NotchController::ClearReason : std::uint8_t
  { Manual, ClearAll, AutoRelease, WidthChange, VerdictFalse, PartialApplyUnwind, SoundcheckReplace };

  struct NotchController::SnapshotNotch
  {
      float frequency, Q, depthDB, deepestDb;
      Origin       origin  = Origin::Detector;   // NEW
      std::uint8_t channel = 0, index = 0;
  };
  ```
  `reasonName (ClearReason::SoundcheckReplace) == "soundcheck_replace"`.
- Consumes: nothing new.

**The one thing an implementer must not get wrong here.** `SnapshotNotch` is filled by a **brace-elided aggregate initialiser** at `src/app/NotchController.cpp:578-580`. Adding a field in the middle silently re-binds every value after it. Put `origin` where the declaration above puts it — after `deepestDb`, before `channel` — and update the initialiser in the same edit:

```cpp
                    notchList[notchCount++] = { (float) n.frequency, (float) n.Q,
                                                (float) n.depthDB, (float) n.deepestDb,
                                                n.origin,
                                                (std::uint8_t) c, (std::uint8_t) i };
```

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_notchcontroller.cpp`:

```cpp
// RED IF: SnapshotNotch loses `origin`, or the aggregate initialiser at
// NotchController.cpp:578-580 is not updated alongside the struct -- brace
// elision would then silently shift channel/index by one field. Without this
// field lane M cannot find the previous run's preventive notches to clear, and
// they never auto-release (NotchController.cpp:729), so the 16-slot chain
// drains after a few soundchecks. F7.
TEST (NotchControllerSoundcheck, SnapshotCarriesOrigin)
{
    Harness h;
    ASSERT_TRUE (h.controller.setNotch (0, 3, 1000.0, 30.0, -12.0,
                                        NotchController::Origin::Soundcheck));
    ASSERT_TRUE (h.controller.setNotch (0, 4, 2000.0, 30.0, -12.0,
                                        NotchController::Origin::Manual));

    NotchController::SnapshotBuffer snap {};
    h.controller.runOnce();
    h.controller.copySnapshot (snap);

    bool sawSoundcheck = false, sawManual = false;
    for (std::uint32_t i = 0; i < snap.notchCount; ++i)
    {
        const auto& n = snap.notches[i];
        if (n.index == 3)
        {
            EXPECT_EQ (n.origin, NotchController::Origin::Soundcheck);
            EXPECT_NEAR (n.frequency, 1000.0f, 0.5f);   // the fields after `origin` still line up
            EXPECT_EQ ((int) n.channel, 0);
            sawSoundcheck = true;
        }
        if (n.index == 4)
        {
            EXPECT_EQ (n.origin, NotchController::Origin::Manual);
            EXPECT_NEAR (n.frequency, 2000.0f, 0.5f);
            sawManual = true;
        }
    }
    EXPECT_TRUE (sawSoundcheck);
    EXPECT_TRUE (sawManual);
}

// RED IF: SoundcheckReplace is folded into Manual. The log could then not tell
// "the soundcheck replaced its own previous proposal" from "a human removed a
// notch" -- the exact mislabelling lane D exists to prevent. F21.
TEST (NotchControllerSoundcheck, SoundcheckReplaceIsItsOwnClearReason)
{
    Harness h;
    Recorder rec;
    h.controller.setEventSink ([&rec] (const NotchController::NotchEvent& e) { rec (e); });

    ASSERT_TRUE (h.controller.setNotch (0, 2, 1000.0, 30.0, -12.0,
                                        NotchController::Origin::Soundcheck));
    h.controller.clearNotch (0, 2, NotchController::ClearReason::SoundcheckReplace);
    h.controller.runOnce();

    ASSERT_FALSE (rec.events.empty());
    const auto& last = rec.events.back();
    EXPECT_EQ (last.kind, NotchController::NotchEvent::Kind::Clear);
    EXPECT_EQ (last.reason, NotchController::ClearReason::SoundcheckReplace);
    EXPECT_NE (last.reason, NotchController::ClearReason::Manual);
    EXPECT_FALSE (h.controller.activeForTest (0, 2));   // B-3: `active`, never depth < 0
}
```

`Recorder` already exists in this file at `tests/test_notchcontroller.cpp:1471`, inside the anonymous namespace opened at `:1463`. **Put both new tests after that namespace closes at `:1487`** so `Recorder` and `Harness` are both visible, and so nothing is appended into a namespace that has already ended (lane G m-E).

Append to `tests/test_sessionlogger.cpp`:

```cpp
// RED IF: reasonName gains no case for SoundcheckReplace. It would fall through
// to "unknown" and the tester's log would say nothing useful -- the same class
// of defect as lane G's retuneReasonName fallthrough (Task 9 review I2).
TEST (SessionLoggerSoundcheck, SoundcheckReplaceReachesTheLogAsItsOwnReason)
{
    const juce::var v = MainComponent::notchEventToVarForTest (
        makeClearEvent (NotchController::ClearReason::SoundcheckReplace));

    ASSERT_NE (v.getDynamicObject(), nullptr);
    EXPECT_EQ (v.getDynamicObject()->getProperty ("ev").toString(), "notch_clear");
    EXPECT_EQ (v.getDynamicObject()->getProperty ("reason").toString(), "soundcheck_replace");
}
```

`MainComponent::notchEventToVarForTest` and the `makeClearEvent` helper were introduced by lane G Task 9; **grep `tests/test_sessionlogger.cpp` for the exact names before writing this test** and use whatever is actually there — do not add a second helper with a similar name.

- [ ] **Step 2: Run the two new suites and watch them fail**

```bash
cmake --build build --config Release
```
Expected: compile error — `'origin': is not a member of 'NotchController::SnapshotNotch'` and `'SoundcheckReplace': is not a member of 'NotchController::ClearReason'`.

- [ ] **Step 3: Make the two additive changes**

`src/app/NotchController.h`, the enumerator list:

```cpp
    enum class ClearReason : std::uint8_t
    {
        Manual, ClearAll, AutoRelease, WidthChange, VerdictFalse, PartialApplyUnwind,
        // Lane M: the active soundcheck replacing its OWN previous proposal on
        // this slot. A separate value because the alternative -- reusing Manual
        // -- makes an automatic cleanup indistinguishable from an operator
        // pulling a notch by hand, in the one file lane D built to tell them
        // apart (spec §4.6b, F21).
        SoundcheckReplace
    };
```

`src/app/NotchController.h`, `SnapshotNotch` — add after `deepestDb`:

```cpp
        // Lane M (spec §4.6, F7): which policy placed this notch. model_ is
        // private and SnapshotNotch is the only view outward, so without this
        // field nothing can find the preventive notches of a previous
        // soundcheck run in order to replace them -- and they never
        // auto-release (NotchController.cpp:729), so the chain drains.
        Origin origin = Origin::Detector;
```

`src/app/NotchController.cpp:578-580` — the aggregate initialiser, exactly as shown above.

`src/app/MainComponent.cpp`, `reasonName` — add before the closing brace of the switch:

```cpp
        case NotchController::ClearReason::SoundcheckReplace: return "soundcheck_replace";
```

- [ ] **Step 4: `tools/logstats.py` — the one branch lane M actually needs**

Spec §4.7 is explicit that the five `soundcheck_*` **`ev` names need no branch**: the `if/elif` chain at `tools/logstats.py:45-83` has no `else`, so an unknown `ev` falls through every branch and is ignored, and the comment at `:66-71` says exactly that. The real work is on the **clear reason** side.

Today a reason is only echoed into the `cleared by` column (`tools/logstats.py:136`). Add a count, so a fixture can pin it:

In `summarise()`, inside the `elif ev == "notch_clear":` branch (anchor text: `n = open_by_key.pop(key(e), None)`), nothing changes; in the returned dict (anchor text: `"retunes": sum(n["retunes"] for n in notches),`) add:

```python
        # Lane M: how many notches this session's soundchecks replaced with
        # their own next proposal. Counted here rather than left to the reason
        # column because "the soundcheck keeps replacing its own work" is a
        # pattern worth seeing at a glance, and because a fixture can then pin
        # that the reason name survives the whole C++ -> JSON -> reader path.
        "soundcheck_replaced": sum(1 for n in notches if n["reason"] == "soundcheck_replace"),
```

In `print_report()`, append to the summary line (anchor text: `f"  unjudged {s['unjudged']}`):

```python
    if s["soundcheck_replaced"]:
        print(f"soundcheck replaced {s['soundcheck_replaced']} notch(es) from an earlier run")
```

In `main()`, beside the other flags (anchor text: `ap.add_argument("--expect-retunes", type=int)`):

```python
    ap.add_argument("--expect-soundcheck-replaced", type=int)
```
and beside the other checks (anchor text: `if args.expect_retunes is not None`):

```python
    if (args.expect_soundcheck_replaced is not None
            and s["soundcheck_replaced"] != args.expect_soundcheck_replaced):
        failures.append(
            f"soundcheck-replaced {s['soundcheck_replaced']} != {args.expect_soundcheck_replaced}")
```

The file already opens with `encoding="utf-8"` (`tools/logstats.py:20`) — leave it (repo rule 6).

- [ ] **Step 5: The fixture**

`tests/fixtures/session-sample.jsonl` currently supports `--expect-notches 4 --expect-verdicts 3 --expect-false 1 --expect-recurrence-max 2 --expect-retunes 2` (`tests/CMakeLists.txt:116-117`). Add **one** `notch_set` with `"origin": "soundcheck"` and its matching `notch_clear` with `"reason": "soundcheck_replace"`, at a `t` after the existing lines, on a `(slot, lane, index)` triple not already in the file, at a frequency **more than one bin (23.4375 Hz) away** from every existing `hz` so `--expect-recurrence-max` does not move.

Then update `tests/CMakeLists.txt:113-117`:

```cmake
    add_test(NAME logstats_fixture
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/logstats.py
                ${CMAKE_SOURCE_DIR}/tests/fixtures/session-sample.jsonl
                --expect-notches 5 --expect-verdicts 3 --expect-false 1
                --expect-recurrence-max 2 --expect-retunes 2
                --expect-soundcheck-replaced 1)
```

`--expect-notches` goes 4 → 5 because one `notch_set` was added. **Re-derive the other four from `tools/logstats.py` before trusting that they are unchanged** — lane G's m-A is the precedent: a fixture that depicts a run the code cannot produce passes review and fails later.

Run it standalone first, before the C++ build:

```bash
python tools/logstats.py tests/fixtures/session-sample.jsonl --expect-notches 5 --expect-verdicts 3 --expect-false 1 --expect-recurrence-max 2 --expect-retunes 2 --expect-soundcheck-replaced 1
```
Expected: the report prints and the exit code is 0.

- [ ] **Step 6: Grep for a duplicated literal**

The constants of Tasks 1–3 must each exist exactly once as a literal. Run:

```bash
grep -rn "0\.1f\|6000\.0\|10000\.0\|kSoundcheckMaxPeak\|kTrustedHighHz" src/ | grep -v "^src/dsp/SoundcheckSignal\|^src/dsp/LoopGainEstimator"
```
Expected: only alias lines of the form `= SoundcheckSignal::k...` / `= LoopGainEstimator::k...`. Anything else is a second literal and must become an alias (lane G m-D).

- [ ] **Step 7: Reconfigure, build, gate, commit**

Headers changed ⇒ full reconfigure.

```bash
cmake -B build -G "Visual Studio 18 2026" -A x64
```
```bash
cmake --build build --config Release
```
```bash
cd build && ctest -C Release
```
Expected: `100% tests passed (577)` — 574 + 3. ESTIMATE.

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add src/app/NotchController.h src/app/NotchController.cpp src/app/MainComponent.cpp tools/logstats.py tests/test_notchcontroller.cpp tests/test_sessionlogger.cpp tests/fixtures/session-sample.jsonl tests/CMakeLists.txt
```
```bash
git commit -m "feat(lane-m): SnapshotNotch carries origin; ClearReason::SoundcheckReplace reaches the log"
```

---

### Task 5: `AudioEngine` — four insertion points, seven atomics, one capture ring

**Mức level dự kiến (spec §3) — the biggest level change this project has made.** On the channel named by `scOutChannel_`, while a run is in flight: **every lane routed to that channel is muted** (it carries the sweep and nothing else), the sweep's peak is **−20 dBFS** with an RMS near −23 dBFS, and the channel is **completely silent** during the 0.5 s noise floor, the 0.7 s tail and the 0.3 s gap. That is **4.5 s of silence per output channel**, and up to **~72 s** for 8 stereo slots. Every other output channel: **0 dB, unchanged.** When idle: **0 dB and no lane muted.** There is no limiter in this app; from here outward the sweep meets exactly one hard clamp, `±1.0f` (`src/app/AudioEngine.cpp:631-647`), and the real loudness in the room is set by the operator's master fader, not by the app.

After this task the engine *can* emit a sweep but nothing drives it: `SoundcheckController` arrives in Task 6. **Do not release from this commit.**

**Files:**
- Modify: `src/app/AudioEngine.h` — the cross-thread block (anchor: `// Cross-thread state. std::atomic keeps the audio callback lock-free`, `:329-341`); new public accessors and the test seam near `getTapDropCount` (anchor: `std::uint64_t getTapDropCount (int slot, int lane) const;`, `:196`)
- Modify: `src/app/AudioEngine.cpp`:
  - insertion 1 + 2 inside the lane-table loop (anchors: `const float* in = (inputChannelData != nullptr)` `:550` and `tapSource[slot][lane] = out;` `:561`)
  - the snapshot (anchor: `const bool bypass = (currentMode_.load (std::memory_order_acquire) == Mode::Bypass);` `:514`)
  - insertion 3 between the end of the DSP block and the clamp (anchors: the closing `}` of the `else` branch at `:624`, and `// Final output guard: every sample actually handed to the driver` `:626`)
  - insertion 4 in the tap loop (anchor: `for (int slot = 0; slot < kMaxSlots; ++slot)` at `:657`)
  - `micCapture_.clear()` in the existing drain block (anchor: `for (auto& slotTaps : tapBuffers_)` … `tap.clear();`, `:729-731`)
- Test: `tests/test_audioengine.cpp` (append; the file's `CallbackDriver` helper at `:36-61` is the shape to reuse — extend it rather than writing a second driver)

**Interfaces:**
- Consumes: `SoundcheckSignal` (Task 1) — `AudioEngine.cpp` gains `#include "dsp/SoundcheckSignal.h"`.
- Produces, on `AudioEngine`:
  ```cpp
  // Message thread. All of these are plain atomic stores; none restarts the device.
  void setSoundcheckOutputChannel  (int channel);          // -1 = off
  void setSoundcheckCaptureChannel (int channel);          // -1 = off
  void setSoundcheckCaptureActive  (bool active);
  void setSoundcheckTapsSuspended  (bool suspended);
  void setSoundcheckSampleIndex    (std::int64_t n);       // negative = noise floor
  void setSoundcheckPeak           (float peak);           // CLAMPED to kSoundcheckMaxPeak here
  void requestSoundcheckRampOut();                         // anchors at the CURRENT sample index
  [[nodiscard]] int          getSoundcheckOutputChannel() const;
  [[nodiscard]] std::int64_t getSoundcheckSampleIndex()   const;
  [[nodiscard]] bool         soundcheckIsEmitting()       const;   // scOutChannel_ >= 0

  LockFreeRingBuffer<float>& getMicCaptureBuffer();        // read() only, one consumer
  [[nodiscard]] std::uint64_t getMicCaptureDropCount() const;

  // TEST SEAM ONLY -- bypasses the setter's clamp so the FINAL +-1.0f output
  // clamp can be shown to still cover the sweep path (F17, inv 4). Without it
  // the amplitude is already clamped twice before it reaches that clamp, and
  // invariant 4 cannot be turned red by any test.
  void setSoundcheckPeakUnclampedForTest (float peak);
  ```

**The four insertion points, in callback order.**

1. **Capture the raw mic — inside the lane-table loop** (`src/app/AudioEngine.cpp:541-562`). The `in` pointer already exists at `:550-551`. When `inIdx == scCaptureInChannel` (already bounds-checked), stash it in a stack local `const float* capSource`. Raw mic, **before any DSP** (Q13).
2. **Mute every lane routed to the measured channel — same loop** (`:558-561`). The condition is **`outIdx == scOutChannel`**, not "(slot, lane) matches" (F2). A muted lane does **not** enter the `lanes[]` table and does **not** set `tapSource`. The channel is still cleared at `:570-577` like every other channel.
3. **Inject the sweep and generate the ramp-out — between `:624` (end of the DSP block) and `:631` (the clamp).** The position is **mandatory**: after the clamp, the sweep would reach the driver unclamped. The block writes `out[n] += ...` into **exactly one** channel. For each sample `n`: if `scRampOutAt >= 0`, multiply by `SoundcheckSignal::rampOut(idx, scRampOutAt, R)`; when the ramp has run out, **the callback itself** stores `scOutChannel_ = -1` and `scRampOutAtSample_ = -1` (F8). At the end of the block, `scSampleIndex_ += numSamples`.
4. **Suspend tap writes for the whole run — the tap loop** (`:657-683`). The key is **`scSuspendTaps`**, not `scOutChannel` (N3): `scOutChannel_` returns to −1 at **every** Gap, so keying on it would un-suspend the taps for 300 ms between channels, restarting the ~420 ms `tapAlive` window **per channel** — ≈ 0.72 s each, **≈ 11.5 s over 16 channels**, which exceeds `kReleaseStepMs = 10 s` and would silently walk every releasing notch down a rung while spec §3 declares 0 dB. With `scSuspendTaps_` held for the whole run the true statement is: **one run advances lane G's release clock by at most ~0.42 s, once** — far under the cheapest 10 s rung. When suspended, skip the entire tap-write loop (a **skip**, not a drop: `tapDropCounts_` must not move, or every reader of the log sees a false positive) and instead write `micCapture_.write (capSource, numSamples)` when `scCaptureActive`.

**Snapshot ONCE, beside `bypass`** (F4, inv 7). All seven atomics are read exactly once, at `:509-514`, into stack locals, and only the locals are used afterwards. The comment already in place at that spot says why: *"The mode AND the mapping are snapshotted ONCE at the top of the block"*. Spec rev 1 read them at three different points; a flip between point 3 and point 4 gives a callback that **both injects the sweep and writes the tap** — precisely the detector-poisoning case this lane exists to avoid.

**Bounds-check every callback** (F3, inv 3). `scOutChannel` and `scCaptureInChannel` are re-checked against **this callback's** `numOutputChannels` / `numInputChannels`, the same way the lane loop already checks every channel index at `:546-548`. A device restart onto fewer channels without this step is an **out-of-bounds write on the realtime thread**. An invalid index means this callback behaves as if no soundcheck were running; the lane M thread notices the missing data / changed counts and aborts (Task 6).

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_audioengine.cpp`. First extend the existing helpers in that file's anonymous namespace (it closes at `tests/test_audioengine.cpp:62` — put these **before** that line, beside `CallbackDriver`):

```cpp
// Like CallbackDriver but with a configurable channel count and per-channel
// input, so a test can prove that only ONE output channel changed.
struct MultiDriver
{
    MultiDriver (int channels, int numSamples, float inputLevel = 0.25f)
        : frames (numSamples)
    {
        in.assign  ((std::size_t) channels, std::vector<float> ((std::size_t) numSamples, inputLevel));
        out.assign ((std::size_t) channels, std::vector<float> ((std::size_t) numSamples, 0.0f));
        for (auto& v : in)  inPtr.push_back (v.data());
        for (auto& v : out) outPtr.push_back (v.data());
    }

    void operator() (AudioEngine& engine)
    {
        for (auto& v : out) std::fill (v.begin(), v.end(), 0.0f);
        const juce::AudioIODeviceCallbackContext context {};
        engine.audioDeviceIOCallbackWithContext (inPtr.data(), (int) inPtr.size(),
                                                 outPtr.data(), (int) outPtr.size(),
                                                 frames, context);
    }

    float peakOn (int channel) const
    {
        float m = 0.0f;
        for (float v : out[(std::size_t) channel]) m = std::max (m, std::abs (v));
        return m;
    }

    std::vector<std::vector<float>> in, out;
    std::vector<const float*> inPtr;
    std::vector<float*>       outPtr;
    int frames;
};

// Slot `s` enabled, mono, inCh -> outCh.
void routeMono (AudioEngine& engine, int s, int inCh, int outCh)
{
    SlotConfig c;
    c.enabled = true;
    c.width   = 1;
    c.inputChannels[0]  = inCh;
    c.outputChannels[0] = outCh;
    engine.setSlotConfig (s, c);
}
```

Then the tests:

```cpp
// RED IF: the mute condition is written as "(slot, lane) matches" instead of
// "outIdx == scOutChannel_". Several slots SUM onto one output channel
// (AudioEngine.cpp:565-577 clears, :621 accumulates), so muting one pair leaves
// the feedback loop through that channel CLOSED -- spec rev 1's blocker F2.
// inv 8.
TEST (AudioEngineSoundcheck, SweptChannelCarriesOnlyTheSweep)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    engine.setMode (AudioEngine::Mode::Bypass);   // Bypass copies in->out: loudest case

    routeMono (engine, 0, 0, 1);
    routeMono (engine, 1, 2, 1);   // a SECOND slot onto the same output channel

    engine.setSoundcheckPeak (SoundcheckSignal::kSoundcheckMaxPeak);
    engine.setSoundcheckSampleIndex (0);
    engine.setSoundcheckTapsSuspended (true);
    engine.setSoundcheckOutputChannel (1);

    MultiDriver d { 4, 256, 0.5f };
    d (engine);

    SoundcheckSignal::Params p;
    p.sampleRate = engine.getCurrentSampleRateHz();
    p.peak       = SoundcheckSignal::kSoundcheckMaxPeak;
    const SoundcheckSignal expected { p };

    for (int n = 0; n < 256; ++n)
        ASSERT_NEAR (d.out[1][(std::size_t) n], expected.sampleAt (n), 1.0e-6f)
            << "sample " << n << " carries something other than the sweep";
}

// RED IF: the injection point is moved BELOW the output clamp at
// AudioEngine.cpp:631-647. The test seam exists precisely because without it the
// amplitude is clamped twice before it reaches that clamp, so invariant 4 could
// never be turned red (F17). inv 4.
TEST (AudioEngineSoundcheck, OutputClampStillCoversTheSweepPath)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    routeMono (engine, 0, 0, 0);

    engine.setSoundcheckPeakUnclampedForTest (5.0f);   // 5x full scale
    engine.setSoundcheckSampleIndex (0);
    engine.setSoundcheckTapsSuspended (true);
    engine.setSoundcheckOutputChannel (1);

    MultiDriver d { 2, 512 };
    d (engine);

    bool sawSomething = false;
    for (float v : d.out[1])
    {
        ASSERT_LE (std::abs (v), 1.0f) << "a sample escaped the +-1.0f clamp";
        ASSERT_TRUE (std::isfinite (v));
        if (std::abs (v) > 0.5f) sawSomething = true;
    }
    EXPECT_TRUE (sawSomething) << "the seam produced nothing -- the test proves nothing";
}

// RED IF: any other output channel receives a sample from the soundcheck path.
// Spec rev 2 dropped this test when it was rewritten; it is the ONLY test for
// invariant 5. N6.
TEST (AudioEngineSoundcheck, SweepTouchesOnlyTheMeasuredChannel)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    engine.setMode (AudioEngine::Mode::Bypass);

    for (int ch = 0; ch < 4; ++ch)
        routeMono (engine, ch, ch, ch);

    engine.setSoundcheckPeak (SoundcheckSignal::kSoundcheckMaxPeak);
    engine.setSoundcheckSampleIndex (0);
    engine.setSoundcheckTapsSuspended (true);
    engine.setSoundcheckOutputChannel (2);

    MultiDriver d { 4, 256, 0.25f };
    d (engine);

    // Channels 0, 1, 3 still pass their own input through, untouched.
    for (int ch : { 0, 1, 3 })
        for (int n = 0; n < 256; ++n)
            ASSERT_FLOAT_EQ (d.out[(std::size_t) ch][(std::size_t) n], 0.25f)
                << "channel " << ch << " sample " << n;
}

// RED IF: the ramp-out is driven from the controller thread instead of being
// generated inside the callback. Nothing but the callback runs here -- no
// controller, no poll -- and the sweep must still reach exactly zero and release
// the channel on its own. inv 9, F8.
TEST (AudioEngineSoundcheck, AbortRampsDownInTheCallbackAlone)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    routeMono (engine, 0, 0, 0);

    engine.setSoundcheckPeak (SoundcheckSignal::kSoundcheckMaxPeak);
    engine.setSoundcheckSampleIndex (4800);         // mid-sweep
    engine.setSoundcheckTapsSuspended (true);
    engine.setSoundcheckOutputChannel (1);

    MultiDriver d { 2, 64 };
    d (engine);
    ASSERT_GT (d.peakOn (1), 0.0f);

    engine.requestSoundcheckRampOut();

    // kRampOutMs = 30 ms; at 48 kHz that is 1440 samples = 23 callbacks of 64.
    for (int i = 0; i < 40; ++i)
        d (engine);

    EXPECT_FLOAT_EQ (d.peakOn (1), 0.0f);
    EXPECT_EQ (engine.getSoundcheckOutputChannel(), -1)
        << "the callback must release the channel itself";
    EXPECT_FALSE (engine.soundcheckIsEmitting());
}

// RED IF: tap suspension is keyed on scOutChannel_ rather than on
// scSuspendTaps_. scOutChannel_ goes to -1 at every Gap, so the taps would come
// back for 300 ms between channels, restarting lane G's ~420 ms tapAlive window
// per channel: ~0.72 s each, ~11.5 s over 16 channels, past kReleaseStepMs =
// 10 s. inv 10, N3.
TEST (AudioEngineSoundcheck, TapsStaySuspendedAcrossTheGap)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    routeMono (engine, 0, 0, 0);

    engine.setSoundcheckTapsSuspended (true);
    engine.setSoundcheckOutputChannel (0);
    engine.setSoundcheckSampleIndex (0);

    MultiDriver d { 2, 256 };
    for (int i = 0; i < 8; ++i) d (engine);

    // The Gap: the channel is released, the suspend flag is NOT.
    engine.setSoundcheckOutputChannel (-1);
    const auto before = engine.getTapBuffer (0, 0).getAvailableRead();
    for (int i = 0; i < 64; ++i) d (engine);    // 64 x 256 = 16384 samples ~ 341 ms > kGapMs
    EXPECT_EQ (engine.getTapBuffer (0, 0).getAvailableRead(), before)
        << "a tap was written during the Gap";

    EXPECT_EQ (engine.getTapDropCount (0, 0), 0u)
        << "a SKIP is not a DROP -- tapDropCounts_ must not move";
}

// RED IF: capture is gated on scOutChannel_ instead of on its own flag, or the
// noise-floor phase emits. The NoiseFloor phase has a live channel, capture on,
// and a NEGATIVE sample index. inv 6, F5.
TEST (AudioEngineSoundcheck, NoiseFloorCapturesWithoutEmitting)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    routeMono (engine, 0, 0, 0);

    engine.setSoundcheckPeak (SoundcheckSignal::kSoundcheckMaxPeak);
    engine.setSoundcheckCaptureChannel (1);
    engine.setSoundcheckCaptureActive (true);
    engine.setSoundcheckTapsSuspended (true);
    engine.setSoundcheckOutputChannel (0);
    engine.setSoundcheckSampleIndex (-24000);          // 0.5 s before the sweep

    MultiDriver d { 2, 256, 0.3f };
    d (engine);

    EXPECT_EQ (d.peakOn (0), 0.0f) << "the noise-floor phase emitted";
    EXPECT_GE (engine.getMicCaptureBuffer().getAvailableRead(), 256u);

    // And with capture off, nothing arrives even though the channel is live.
    engine.setSoundcheckCaptureActive (false);
    const auto have = engine.getMicCaptureBuffer().getAvailableRead();
    d (engine);
    EXPECT_EQ (engine.getMicCaptureBuffer().getAvailableRead(), have);
}

// RED IF: the per-callback bounds check is dropped. A device restart onto fewer
// channels would then be an out-of-bounds write on the realtime thread. inv 3,
// F3.
TEST (AudioEngineSoundcheck, OutOfRangeChannelIsIgnored)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    routeMono (engine, 0, 0, 0);

    engine.setSoundcheckPeak (SoundcheckSignal::kSoundcheckMaxPeak);
    engine.setSoundcheckSampleIndex (0);
    engine.setSoundcheckTapsSuspended (true);
    engine.setSoundcheckCaptureActive (true);

    MultiDriver d { 2, 128 };

    engine.setSoundcheckOutputChannel (2);       // == numOutputChannels
    engine.setSoundcheckCaptureChannel (9);
    d (engine);                                  // must not crash, must not write
    EXPECT_EQ (d.peakOn (0), 0.25f);             // slot 0 still passes through
    EXPECT_EQ (d.peakOn (1), 0.0f);

    engine.setSoundcheckOutputChannel (-5);
    d (engine);
    EXPECT_EQ (engine.getMicCaptureBuffer().getAvailableRead(), 0u);
}

// RED IF: the atomics are read at more than one point in the callback. A flip
// between the mute decision and the tap decision produces a callback that BOTH
// injects the sweep AND taps it into the detector -- the poisoning case this
// lane exists to prevent. inv 7, F4.
TEST (AudioEngineSoundcheck, AtomicsAreSnapshottedOnce)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    routeMono (engine, 0, 0, 0);

    engine.setSoundcheckPeak (SoundcheckSignal::kSoundcheckMaxPeak);
    engine.setSoundcheckSampleIndex (0);

    MultiDriver d { 2, 64 };

    // 200 callbacks while another thread flips the channel and the suspend flag
    // as fast as it can. Each callback must be internally consistent: if it
    // emitted, it must NOT have tapped, and vice versa.
    std::atomic<bool> stop { false };
    std::thread flipper ([&engine, &stop]
    {
        while (! stop.load())
        {
            engine.setSoundcheckOutputChannel (0);
            engine.setSoundcheckTapsSuspended (true);
            engine.setSoundcheckOutputChannel (-1);
            engine.setSoundcheckTapsSuspended (false);
        }
    });

    for (int i = 0; i < 200; ++i)
    {
        const auto tapBefore = engine.getTapBuffer (0, 0).getAvailableRead();
        d (engine);
        const auto tapAfter  = engine.getTapBuffer (0, 0).getAvailableRead();
        const bool tapped    = tapAfter != tapBefore;
        const bool emitted   = d.peakOn (0) > 0.0f && d.peakOn (0) != 0.25f;

        ASSERT_FALSE (tapped && emitted)
            << "callback " << i << " both injected and tapped -- the atomics were re-read";
        engine.getTapBuffer (0, 0).clear();
    }

    stop.store (true);
    flipper.join();
}

// RED IF: an idle engine emits anything, OR mutes a lane. The second half had no
// test at all until spec rev 3. inv 6.
TEST (AudioEngineSoundcheck, IdleEngineEmitsNoSweepAndMutesNoLane)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    engine.setMode (AudioEngine::Mode::Bypass);
    routeMono (engine, 0, 0, 0);
    routeMono (engine, 1, 1, 1);

    MultiDriver d { 2, 256, 0.4f };
    d (engine);

    // Both lanes contributed normally; nothing was muted and nothing injected.
    for (int ch : { 0, 1 })
        for (int n = 0; n < 256; ++n)
            ASSERT_FLOAT_EQ (d.out[(std::size_t) ch][(std::size_t) n], 0.4f) << "ch " << ch;

    EXPECT_EQ (engine.getSoundcheckOutputChannel(), -1);
    EXPECT_FALSE (engine.soundcheckIsEmitting());
}

// RED IF: micCapture_.clear() is left out of audioDeviceAboutToStart's drain
// block. Audio captured at the PREVIOUS device's sample rate would be spliced
// onto the front of the next run's first analysis windows -- the same defect the
// tap rings are already cleared to avoid (AudioEngine.cpp:713-731). F15.
TEST (AudioEngineSoundcheck, DeviceRestartDrainsTheCaptureRing)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    routeMono (engine, 0, 0, 0);

    engine.setSoundcheckCaptureChannel (1);
    engine.setSoundcheckCaptureActive (true);
    engine.setSoundcheckTapsSuspended (true);
    engine.setSoundcheckOutputChannel (0);
    engine.setSoundcheckSampleIndex (-1000);

    MultiDriver d { 2, 256, 0.3f };
    d (engine);
    ASSERT_GT (engine.getMicCaptureBuffer().getAvailableRead(), 0u);

    engine.audioDeviceAboutToStart (nullptr);
    EXPECT_EQ (engine.getMicCaptureBuffer().getAvailableRead(), 0u);
}
```

`tests/test_audioengine.cpp` will need `#include "dsp/SoundcheckSignal.h"`, `<thread>` and `<atomic>` added to its include block (`tests/test_audioengine.cpp:17-26`).

- [ ] **Step 2: Run and watch every new test fail**

```bash
cmake --build build --config Release
```
Expected: compile errors — `setSoundcheckOutputChannel` and friends are not members of `AudioEngine`.

- [ ] **Step 3: Declare the state in `src/app/AudioEngine.h`**

Add the seven atomics, the ring and the counter exactly as the Global Constraints section spells them, immediately after the existing `numInputChannels_` / `numOutputChannels_` pair (`:340-341`). Add the accessors listed in Interfaces near `getTapDropCount` (`:194-196`), each with a one-line thread comment. Add `#include "dsp/SoundcheckSignal.h"` beside the existing `#include "dsp/LockFreeRingBuffer.h"` (`:39`).

`setSoundcheckPeak` clamps at the setter:

```cpp
void AudioEngine::setSoundcheckPeak (float peak)
{
    // Clamped HERE and again inside SoundcheckSignal::sampleAt. Two clamps for
    // one value is deliberate: this number has two routes in (this setter and
    // the test seam below), and a value with two routes to becoming wrong needs
    // both clamped -- one clamp is half a clamp (lane G M-B).
    scPeak_.store (SoundcheckSignal::clampPeak (peak), std::memory_order_relaxed);
}

void AudioEngine::setSoundcheckPeakUnclampedForTest (float peak)
{
    // TEST SEAM ONLY (F17). Without a route that skips the setter's clamp, the
    // amplitude is already bounded twice before it reaches the +-1.0f output
    // clamp, so invariant 4 could not be turned red by any test. Same shape as
    // lane G's retuneForTest.
    scPeak_.store (peak, std::memory_order_relaxed);
}
```

`requestSoundcheckRampOut()` stores the CURRENT `scSampleIndex_` into `scRampOutAtSample_` — the anchor is a sample index, not a time, so the callback can evaluate the envelope with no clock.

- [ ] **Step 4: The snapshot, beside `bypass`**

At `src/app/AudioEngine.cpp:514`, directly under the existing `const bool bypass = ...` line and under its existing comment:

```cpp
    // Lane M (spec §4.1, invariant 7): the SEVEN soundcheck atomics are read
    // HERE, once, for the same reason the mode and the mapping are -- a flip
    // mid-callback between the mute decision (point 2) and the tap decision
    // (point 4) would produce a block that BOTH injects the sweep and taps it
    // back into the detector. Nothing below this point reads the atomics again.
    const int          scOutChannel      = scOutChannel_.load       (std::memory_order_relaxed);
    const bool         scSuspendTaps     = scSuspendTaps_.load      (std::memory_order_relaxed);
    const int          scCaptureIn       = scCaptureInChannel_.load (std::memory_order_relaxed);
    const bool         scCaptureActive   = scCaptureActive_.load    (std::memory_order_relaxed);
    const std::int64_t scSampleIndex     = scSampleIndex_.load      (std::memory_order_relaxed);
    const float        scPeak            = scPeak_.load             (std::memory_order_relaxed);
    const std::int64_t scRampOutAt       = scRampOutAtSample_.load  (std::memory_order_relaxed);

    // Invariant 3: re-checked against THIS callback's counts, exactly as the
    // lane loop re-checks every channel index at :546-548. A device restart onto
    // fewer channels without this is an out-of-bounds write on the audio thread.
    const int  scOut   = (scOutChannel >= 0 && scOutChannel < numOutputChannels
                          && outputChannelData != nullptr) ? scOutChannel : -1;
    const int  scInCh  = (scCaptureIn  >= 0 && scCaptureIn  < numInputChannels
                          && inputChannelData  != nullptr) ? scCaptureIn  : -1;
```

- [ ] **Step 5: Insertion points 1 and 2 — inside the lane loop**

Declare `const float* capSource = nullptr;` beside `tapSource` (`:526`). Inside the lane loop, after the existing bounds check at `:546-548` and before the `in`/`out` pointers are taken:

```cpp
            // Point 1 (Q13): the RAW mic, before any DSP.
            if (scInCh >= 0 && inIdx == scInCh && inputChannelData != nullptr)
                capSource = inputChannelData[inIdx];

            // Point 2 (Q15 relitigated, F2): mute EVERY lane routed to the
            // channel being measured -- not one (slot, lane) pair. Several slots
            // sum onto one output channel (:565-577 clears, :621 accumulates),
            // so muting a pair leaves that channel's feedback loop CLOSED. A
            // muted lane enters neither lanes[] nor tapSource.
            if (scOut >= 0 && outIdx == scOut)
                continue;
```

The `continue` must come **before** `lanes[numLanes++] = ...` at `:558` and before `tapSource[slot][lane] = out;` at `:561`.

- [ ] **Step 6: Insertion point 3 — the sweep, before the clamp**

Between the closing brace of the `else` DSP block (`:624`) and the `// Final output guard` comment (`:626`):

```cpp
    // Lane M point 3 (spec §4.1). MUST stay ABOVE the +-kMaxOutputLevel clamp
    // below: past it, the sweep would reach the driver unclamped (invariant 4).
    // Writes exactly ONE channel (invariant 5), accumulating like the DSP above.
    if (scOut >= 0)
    {
        const double sr = currentSampleRate_.load (std::memory_order_relaxed);

        SoundcheckSignal::Params params;
        params.sampleRate = sr;
        params.peak       = scPeak;
        const SoundcheckSignal signal { params };   // stack, no allocation, one std::log

        const std::int64_t rampLen = SoundcheckSignal::rampOutSamples (sr);
        float* out = outputChannelData[scOut];

        if (out != nullptr)
        {
            for (int n = 0; n < numSamples; ++n)
            {
                const std::int64_t idx = scSampleIndex + n;
                float v = signal.sampleAt (idx);            // 0 while idx < 0 (NoiseFloor)
                if (scRampOutAt >= 0)
                    v *= SoundcheckSignal::rampOut (idx, scRampOutAt, rampLen);
                out[n] += v;
            }
        }

        scSampleIndex_.store (scSampleIndex + numSamples, std::memory_order_relaxed);

        // The callback ENDS the run itself once the ramp-out has reached zero
        // (F8, invariant 9): no other thread needs to still be alive for the
        // sound to stop.
        if (scRampOutAt >= 0 && scSampleIndex + numSamples >= scRampOutAt + rampLen)
        {
            scOutChannel_.store      (-1, std::memory_order_relaxed);
            scRampOutAtSample_.store (-1, std::memory_order_relaxed);
        }
    }
```

- [ ] **Step 7: Insertion point 4 — the tap loop**

Replace the head of the tap loop at `:657-659` with:

```cpp
    // Lane M point 4 (spec §4.1, invariant 10). Keyed on scSuspendTaps_ and NOT
    // on scOutChannel_: that index returns to -1 at every Gap, so keying on it
    // un-suspends the taps for 300 ms between channels and restarts lane G's
    // ~420 ms tapAlive window PER CHANNEL -- ~11.5 s over 16 channels, past
    // kReleaseStepMs = 10 s (N3). Held for the whole run, the true figure is
    // ~0.42 s once.
    //
    // This is a SKIP, not a drop: tapDropCounts_ must not move, or every reader
    // of the session log sees a burst of false positives that never happened.
    if (scSuspendTaps)
    {
        if (scCaptureActive && capSource != nullptr)
        {
            const std::size_t requested = (std::size_t) numSamples;
            const std::size_t written   = micCapture_.write (capSource, requested);
            if (written < requested)
                micCaptureDrops_.fetch_add (requested - written, std::memory_order_relaxed);
        }
    }
    else
    for (int slot = 0; slot < kMaxSlots; ++slot)
    for (int lane = 0; lane < kMaxSlotLanes; ++lane)
    {
        ... unchanged ...
    }
```

- [ ] **Step 8: `micCapture_.clear()` in the restart drain**

In `audioDeviceAboutToStart`, immediately after the tap-ring drain loop (anchor: `for (auto& slotTaps : tapBuffers_)` … `tap.clear();`, `:729-731`):

```cpp
    // Lane M: the same precondition and the same reason. Capture taken at the
    // PREVIOUS device's rate would be spliced onto the front of the next run's
    // first analysis windows, and every bin-to-Hz conversion would be wrong.
    // MainComponent aborts and joins the SoundcheckController before a restart
    // reaches here (spec §4.3), so neither producer nor consumer is running.
    micCapture_.clear();
```

- [ ] **Step 9: Build and run the suite**

`AudioEngine.h` changed ⇒ full reconfigure.

```bash
cmake -B build -G "Visual Studio 18 2026" -A x64
```
```bash
cmake --build build --config Release
```
```bash
cd build && ctest -C Release -R AudioEngine --output-on-failure
```
Expected: `100% tests passed` — the 37 pre-existing `AudioEngine` tests plus 11 new.

- [ ] **Step 10: The invariant-16 read, by a human**

No test can catch a violation here (spec §4.11, §5.2). Before committing, re-read the whole soundcheck path inside `audioDeviceIOCallbackWithContext` and confirm, line by line:

> no `lock`, no allocation (no `new`, no growing container, no `juce::String`), no logging, and nothing touched outside **the seven atomics + `micCaptureDrops_` + `micCapture_`**.

The `SoundcheckSignal` constructed on the stack at point 3 is part of this read: it must have no member that allocates. Paste the conclusion into the commit message. **This line belongs in the reviewer brief of every task that touches `AudioEngine`.**

- [ ] **Step 11: Full gate and commit**

```bash
cd build && ctest -C Release
```
Expected: `100% tests passed (588)` — 577 + 11. ESTIMATE.

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add src/app/AudioEngine.h src/app/AudioEngine.cpp tests/test_audioengine.cpp
```
```bash
git commit -m "feat(lane-m): AudioEngine emits the soundcheck sweep -- mute by output channel, capture ring, callback-owned ramp-out"
```

---

### Task 6: `SoundcheckController` — the state machine, the refusals, the aborts

**Mức level dự kiến (spec §3):** this task is what makes Task 5's engine actually emit. Per output channel, in order: **0.5 s silence** (noise floor), **30 ms raised-cosine ramp in**, **3.0 s sweep at −20 dBFS peak / ≈ −23 dBFS RMS**, **0.7 s silence** (tail), **0.3 s silence** (gap, lanes already restored) — **4.5 s per channel, ~72 s worst case**. Every other output channel: **0 dB**. On `Results`: **0 dB, and detection is already back on**, so lane G's release ladder is running normally. Nothing is placed: **0 dB** until `ÁP DỤNG` in Task 7.

**The machine** (spec §4.3), transcribed so no implementer has to re-read the spec:

```
Idle
 └─(request)→ Preflight   check the refusals; RECORD sampleRate + in/out channel counts
      └─→ Confirm         dialog: "HẠ MASTER TRƯỚC" + the total duration (~72 s worst case)
           └─(OK)→ Arm    CHECK RING RISK ONE LAST TIME (the snapshot is still live here);
                          READ the noise-floor gate -> RunParams.noiseFloorGate;
                          scSuspendTaps_ = true  (HELD to the end of the LAST channel);
                          detection OFF on every slot; lock the controls (§4.9);
                          log soundcheck_start
                └─→ [for each output channel]
                     NoiseFloor 0.5 s  scOutChannel_ set (already muted),
                                       scCaptureActive_ = true,
                                       scSampleIndex_ = -noiseFloorSamples
                       └─ GATE: max peakinessAt(noise window) >= RunParams::noiseFloorGate
                                ⇒ Abort(room_ringing)
                     Sweep 3.0 s       scSampleIndex_ passes through 0
                     Tail 0.7 s
                     Analyse           compute H, pick candidates; log soundcheck_output
                     Gap 0.3 s         scOutChannel_ = -1, scCaptureActive_ = false
                                       scSuspendTaps_ STAYS true
                └─(channels exhausted)→ Restore-detection  scSuspendTaps_ = false;
                                                           detection back ON immediately
                     └─→ Results       log soundcheck_result; unlock everything but PRESET LOAD
Results  (detection already ON, taps running again, snapshot live again)
 ├─(ÁP DỤNG)→ Apply  (message thread) → log soundcheck_apply → Idle
 ├─(BỎ)      → Idle
 └─(kResultsTimeoutMs = 20 s elapses)→ Idle

Abort  ← from any emitting phase: set scRampOutAtSample_ (the callback does the rest),
       wait for scOutChannel_ to reach -1, scSuspendTaps_ = false, detection back on,
       unlock, log soundcheck_abort → Idle
```

**Four things this machine gets right that spec rev 1 got wrong, each of which an implementer will be tempted to "simplify" back:**

1. **`Results` runs WITH detection back on** (F9). Rev 1 held detection off for 60 s to avoid racing for an `index`. But the taps resume the moment suspension lifts, so `liveMs_` advances (`src/app/NotchController.cpp:662-664`), `riskValid` is false so `frozen` is false, and the release ladder walks 60 s: a −24 notch loses its first rung at 30 s and its second at 40 s — while spec §3 declares 0 dB. Detection comes back **the instant the last channel's tail ends**, and `kResultsTimeoutMs` is 20 s regardless.
2. **The `index` race is handled by re-reading, not by disarming the detector** (F10) — Task 7.
3. **Ring risk is read only where it is still alive.** Making `ringRiskScore >= ringRiskThreshold` a *mid-run* abort condition **can never fire**: detection is off, so `processSpectrumForDetection` returns immediately (`src/app/NotchController.cpp:1281-1282`) and `frameScoreValid_` is never true; and the taps are suspended, so no block is drained and the snapshot never refreshes (`:617-650`). Ring risk is therefore read at **`Preflight` and again at `Arm`**, while the taps still run. The "is the room ringing?" question *during* a run is answered by lane M's own measurement instead: the 0.5 s noise window goes through `PeakinessAnalyzer::peakinessAt` and the **peakiest bin** is compared against `RunParams::noiseFloorGate`.
4. **The mic level for self-abort is computed on the lane M thread** from `micCapture_` (20 ms sliding RMS), never taken from anything of the detector's.

**`RunParams` — read ONCE at `Arm` on the message thread, then immutable.** Every field being frozen is the point: a threshold that moved mid-run would score channel 1 and channel 9 of the *same measurement* on two different rulers, and nobody reading the log could tell.

**Files:**
- Create: `src/app/SoundcheckController.h`, `src/app/SoundcheckController.cpp`
- Create: `tests/test_soundcheckcontroller.cpp`
- Modify: `CMakeLists.txt` (`HANDSFREE_CORE_SOURCES`), `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `AudioEngine` setters and `getMicCaptureBuffer` / `getMicCaptureDropCount` (Task 5); `SoundcheckSignal` (1), `LoopGainEstimator` (2), `SoundcheckCandidates` (3); `ClockSource` (`src/dsp/ClockSource.h:8-13`); `PeakinessAnalyzer::peakinessAt` (`src/dsp/PeakinessAnalyzer.h:166`); `NotchController::SnapshotBuffer` and the three constants `kRiskFreezeFraction` / `kDepthLadderDb` / `kMaxDepthDb` **as values, never through a stored pointer** (inv 17).
- Produces:
  ```cpp
  class SoundcheckController : private juce::Thread
  {
  public:
      static constexpr double kTailSeconds      = 0.7;
      static constexpr double kGapMs            = 300.0;
      static constexpr double kNoiseFloorMs     = 500.0;
      static constexpr double kMicAbortDbfs     = -6.0;
      static constexpr double kMicAbortHoldMs   = 20.0;
      static constexpr double kResultsTimeoutMs = 20000.0;
      static constexpr double kPollMs           = 5.0;
      // Aliases -- one definition each, no second literal (lane G m-D):
      static constexpr double kSweepLowHz        = SoundcheckSignal::kSweepLowHz;
      static constexpr double kSweepHighHz       = SoundcheckSignal::kSweepHighHz;
      static constexpr double kSweepSeconds      = SoundcheckSignal::kSweepSeconds;
      static constexpr double kRampMs            = SoundcheckSignal::kRampMs;
      static constexpr double kRampOutMs         = SoundcheckSignal::kRampOutMs;
      static constexpr float  kSoundcheckMaxPeak = SoundcheckSignal::kSoundcheckMaxPeak;
      static constexpr float  kSoundcheckMinPeak = SoundcheckSignal::kSoundcheckMinPeak;
      static constexpr double kTrustedHighHz     = LoopGainEstimator::kTrustedHighHz;
      static constexpr double kMinBandSnrDb      = LoopGainEstimator::kMinBandSnrDb;
      static constexpr double kMinBinSnrDb       = LoopGainEstimator::kMinBinSnrDb;
      static constexpr double kCandidateMarginDb = SoundcheckCandidates::kCandidateMarginDb;
      static constexpr double kMinUsefulCutDb    = SoundcheckCandidates::kMinUsefulCutDb;
      static constexpr double kMinProminenceDb   = SoundcheckCandidates::kMinProminenceDb;
      static constexpr double kTargetMarginDb    = SoundcheckCandidates::kTargetMarginDb;
      static constexpr int    kMaxPreventivePerLane = SoundcheckCandidates::kMaxPreventivePerLane;

      enum class State  { Idle, Preflight, Confirm, Arm, NoiseFloor, Sweep, Tail,
                          Analyse, Gap, Results, Abort };
      enum class Refusal { None, EngineNotRunning, NoChannels, SlotDisabled,
                           InvalidChannelPair, RingRiskRising };
      enum class AbortReason { UserStop, Esc, EngineStopped, DeviceError, DeviceChanged,
                               MicHot, RoomRinging, CaptureDrop };

      struct Target { int slot = 0, lane = 0, outChannel = 0, inChannel = 0; };

      struct RunParams
      {
          float  noiseFloorGate   = 0.0f;   // = getPeakinessThreshold() at Arm. A PEAKINESS
                                            // RATIO, never a 0..1 score (N1).
          float  peak             = 0.0f;   // already clamped <= kSoundcheckMaxPeak
          double sampleRate       = 0.0;
          int    numInputChannels = 0, numOutputChannels = 0;
          double ceilingDb        = 0.0;
          double notchQ           = 0.0;
      };

      struct OutputResult
      {
          int   slot = 0, lane = 0, outChannel = 0, inChannel = 0;
          bool  measured = false;         // false = "could not measure" (Q8) -- a VALID result
          bool  routingInvalid = false;   // different from measured == false (F26)
          float snrDb = 0.0f;
          std::array<float, LoopGainEstimator::kNumBins> marginDb {};   // = -H_dB
          std::array<bool,  LoopGainEstimator::kNumBins> trusted {};
          int   markedCount = 0, candidateCount = 0, saturatedBins = 0;
          struct Candidate { float hz = 0.0f, marginDb = 0.0f, depthDb = 0.0f, q = 0.0f,
                             residualDb = 0.0f; int bin = 0; };
          std::array<Candidate, kMaxPreventivePerLane> candidates {};
      };

      SoundcheckController (AudioEngine& engine, ClockSource& clock);
      ~SoundcheckController() override;

      // --- MESSAGE THREAD ---
      [[nodiscard]] Refusal preflight (const std::vector<Target>& targets,
                                       const NotchController::SnapshotBuffer& risk) const;
      bool  arm (std::vector<Target> targets, const RunParams& params);
      void  applyRequested();          // Results -> Idle, after Task 7 has placed
      void  dismissRequested();        // BO
      void  requestStop (AbortReason reason);
      void  abortAndJoin();            // device restart path; blocks until Idle

      // --- ANY THREAD (reads) ---
      [[nodiscard]] State  getState() const;
      [[nodiscard]] int    getCurrentTargetIndex() const;
      [[nodiscard]] int    getTargetCount() const;
      [[nodiscard]] double getElapsedMsInRun() const;
      [[nodiscard]] double getRemainingMsInRun() const;
      [[nodiscard]] std::vector<OutputResult> copyResults() const;

      // Injected so this class holds NO NotchController pointer (inv 17). The
      // owner's lambda does nothing but relaxed atomic stores
      // (NotchController::setDetectionActive, NotchController.cpp:936-939).
      std::function<void (bool)>          setDetectionActiveOnAllSlots;
      std::function<void (const juce::var&)> logEvent;      // message- or lane-M thread
      std::function<void()>               onStateChanged;   // GUI repaint request

      // One poll step. run() is this in a loop with wait(kPollMs); every test
      // drives it directly, so no test starts a thread. Same shape as
      // NotchController::runOnce (NotchController.h:349).
      void runOnce();

      void start();
      void stop (int timeoutMs);

  private:
      void run() override;
  };

  // MESSAGE THREAD ONLY. Task 7.
  struct SoundcheckApplyStats { int placed = 0, refused = 0, clearedPrevious = 0; };
  SoundcheckApplyStats applySoundcheckResults (
      NotchController& controller,
      const std::vector<SoundcheckController::OutputResult>& results);
  ```

**Why `setDetectionActiveOnAllSlots` is a `std::function` and not a `NotchController*`.** Invariant 17 says this class never calls `NotchController`, and invariant 12 says detection must be back on **before** `Results` is entered — which rules out a `MessageManager::callAsync` hop, because that returns immediately and the state machine would reach `Results` first. So the controller calls an injected lambda synchronously on its own thread, and `MainComponent`'s lambda is exactly:

```cpp
    soundcheck_.setDetectionActiveOnAllSlots = [this] (bool on)
    {
        // Relaxed atomic store per slot and nothing else
        // (NotchController::setDetectionActive, NotchController.cpp:936-939).
        // Safe from the lane M thread; this is the ONLY NotchController call
        // anywhere on that thread's stack, and it is here in MainComponent, not
        // in SoundcheckController, which holds no pointer at all (inv 17).
        for (auto& c : notchControllers_)
            c->setDetectionActive (on);
    };
```

**This is the single place where the plan interprets the spec rather than transcribing it, and it is the first thing a reviewer should challenge.** The alternatives were: (a) hold a `NotchController*` — breaks inv 17's letter; (b) hop to the message thread — breaks inv 12's ordering; (c) have the GUI timer service a flag — same ordering problem. Task 10 adds `RestoreDetectionCallbackOnlyTouchesAtomics`, which asserts the lambda's whole observable effect is `detectionActiveForTest()` (`src/app/NotchController.h:404-405`) on every slot.

**Timing.** Every phase deadline is `clock_.nowMs() + duration`; `runOnce()` compares against `clock_.nowMs()`. Sample counts are derived from `RunParams::sampleRate`, never from a live engine read, so a rate change is *detected* (and aborts) rather than silently changing the maths.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_soundcheckcontroller.cpp`. Fixture first:

```cpp
// tests/test_soundcheckcontroller.cpp
//
// Fake clock, real AudioEngine (no device -- the callback is public and driven
// by hand, exactly as tests/test_audioengine.cpp does it), no thread: every test
// calls runOnce() itself.
#include <gtest/gtest.h>

#include "app/AudioEngine.h"
#include "app/NotchController.h"
#include "app/SoundcheckController.h"

#include <cmath>
#include <random>
#include <vector>

namespace
{
constexpr double kSr = 48000.0;

class FakeClock : public ClockSource
{
public:
    double nowMs() const override { return ms_; }
    void advance (double m) { ms_ += m; }
private:
    double ms_ = 1000.0;
};

// Drives `ms` of audio through the engine while polling the controller, so the
// sample index and the wall clock stay consistent. 256-sample blocks at 48 kHz
// are 5.33 ms, i.e. about one poll each.
struct Rig
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    FakeClock   clock;
    SoundcheckController sc { engine, clock };

    std::vector<std::vector<float>> in, out;
    std::vector<const float*> inPtr;
    std::vector<float*>       outPtr;
    int channels = 2, frames = 256;
    std::vector<float> micSource;        // what the "room" feeds back, looped
    std::size_t micPos = 0;
    int detectionCalls = 0; bool detectionOn = true;
    std::vector<juce::var> log;

    explicit Rig (int chans = 2) : channels (chans)
    {
        in.assign  ((std::size_t) channels, std::vector<float> ((std::size_t) frames, 0.0f));
        out.assign ((std::size_t) channels, std::vector<float> ((std::size_t) frames, 0.0f));
        for (auto& v : in)  inPtr.push_back (v.data());
        for (auto& v : out) outPtr.push_back (v.data());

        sc.setDetectionActiveOnAllSlots = [this] (bool on) { detectionOn = on; ++detectionCalls; };
        sc.logEvent = [this] (const juce::var& v) { log.push_back (v); };

        SlotConfig c; c.enabled = true; c.width = 2;
        c.inputChannels[0] = 0; c.inputChannels[1] = 1;
        c.outputChannels[0] = 0; c.outputChannels[1] = 1;
        engine.setSlotConfig (0, c);
    }

    void block()
    {
        // Feed the mic channels from micSource so the noise-floor gate and the
        // hot-mic abort see something real.
        for (int ch = 0; ch < channels; ++ch)
            for (int n = 0; n < frames; ++n)
                in[(std::size_t) ch][(std::size_t) n] =
                    micSource.empty() ? 0.0f
                                      : micSource[(micPos + (std::size_t) n) % micSource.size()];
        micPos += (std::size_t) frames;

        for (auto& v : out) std::fill (v.begin(), v.end(), 0.0f);
        const juce::AudioIODeviceCallbackContext ctx {};
        engine.audioDeviceIOCallbackWithContext (inPtr.data(), channels,
                                                 outPtr.data(), channels, frames, ctx);
    }

    void pump (double ms)
    {
        const double perBlock = frames / kSr * 1000.0;
        for (double t = 0.0; t < ms; t += perBlock)
        {
            block();
            clock.advance (perBlock);
            sc.runOnce();
        }
    }

    float peakOn (int ch) const
    {
        float m = 0.0f;
        for (float v : out[(std::size_t) ch]) m = std::max (m, std::abs (v));
        return m;
    }

    std::vector<SoundcheckController::Target> stereoTargets() const
    {
        return { { 0, 0, 0, 0 }, { 0, 1, 1, 1 } };
    }

    SoundcheckController::RunParams params (float gate = 10.0f) const
    {
        SoundcheckController::RunParams p;
        p.noiseFloorGate    = gate;
        p.peak              = SoundcheckSignal::kSoundcheckMaxPeak;
        p.sampleRate        = kSr;
        p.numInputChannels  = channels;
        p.numOutputChannels = channels;
        p.ceilingDb         = -24.0;
        p.notchQ            = 30.0;
        return p;
    }
};

NotchController::SnapshotBuffer risk (bool valid, float score)
{
    NotchController::SnapshotBuffer s {};
    s.ringRiskValid     = valid;
    s.ringRiskScore     = score;
    s.ringRiskThreshold = 0.7f;      // what NotchController.cpp:648 publishes
    return s;
}

std::vector<float> whiteNoise (std::size_t n, float sigma, unsigned seed = 3)
{
    std::mt19937 rng { seed };
    std::normal_distribution<float> d { 0.0f, sigma };
    std::vector<float> v (n);
    for (auto& x : v) x = d (rng);
    return v;
}

std::vector<float> noisePlusTone (std::size_t n, float sigma, double hz, float amp)
{
    auto v = whiteNoise (n, sigma);
    for (std::size_t i = 0; i < n; ++i)
        v[i] += amp * (float) std::sin (2.0 * 3.14159265358979323846 * hz * (double) i / kSr);
    return v;
}

const char* evOf (const juce::var& v)
{
    auto* o = v.getDynamicObject();
    return o == nullptr ? "" : o->getProperty ("ev").toString().toRawUTF8();
}

bool sawEvent (const std::vector<juce::var>& log, const char* name)
{
    for (const auto& v : log)
        if (juce::String (evOf (v)) == name) return true;
    return false;
}
} // namespace
```

Now the refusal tests. **Each asserts that NOT ONE SAMPLE was emitted and the state is back to `Idle`** (inv 19):

```cpp
// RED IF: Preflight stops checking isRunning(). AudioEngine's channel counts are
// written only from the callback (AudioEngine.cpp:506-507) and are never reset
// on stop, so a non-zero count can be a leftover from the previous device
// session -- checking the counts alone is not enough. F26.
TEST (SoundcheckController, RefusesWhenEngineNotRunning)
{
    Rig r;
    EXPECT_EQ (r.sc.preflight (r.stereoTargets(), risk (false, 0.0f)),
               SoundcheckController::Refusal::EngineNotRunning);
    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Idle);
    r.pump (100.0);
    EXPECT_EQ (r.peakOn (0), 0.0f);
    EXPECT_EQ (r.peakOn (1), 0.0f);
}

// RED IF: a zero-channel device is treated as measurable.
TEST (SoundcheckController, RefusesWithZeroChannels)
{
    Rig r;
    r.engine.audioDeviceAboutToStart (nullptr);
    auto p = r.params(); p.numInputChannels = 0; p.numOutputChannels = 0;
    EXPECT_EQ (r.sc.preflight ({}, risk (false, 0.0f)),
               SoundcheckController::Refusal::NoChannels);
    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Idle);
}

// RED IF: a disabled slot is measured anyway. Its lanes are not in the callback's
// lane table at all (AudioEngine.cpp:530-531), so the sweep would go out with
// nothing to compare it against.
TEST (SoundcheckController, RefusesWhenSlotDisabled)
{
    Rig r;
    SlotConfig c = r.engine.getSlotConfig (0); c.enabled = false;
    r.engine.setSlotConfig (0, c);

    EXPECT_EQ (r.sc.preflight (r.stereoTargets(), risk (false, 0.0f)),
               SoundcheckController::Refusal::SlotDisabled);
    r.pump (100.0);
    EXPECT_EQ (r.peakOn (0), 0.0f);
}

// RED IF: an invalid channel pair is allowed to degrade into "could not
// measure". The lane loop merely `continue`s past a bad pair
// (AudioEngine.cpp:546-548), so without this check bad routing reads as a quiet
// room instead of as a routing fault. F26.
TEST (SoundcheckController, RefusesOnInvalidChannelPair)
{
    Rig r;
    r.engine.audioDeviceAboutToStart (nullptr);
    std::vector<SoundcheckController::Target> bad { { 0, 0, 99, 0 } };

    EXPECT_EQ (r.sc.preflight (bad, risk (false, 0.0f)),
               SoundcheckController::Refusal::InvalidChannelPair);
    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Idle);
    r.pump (100.0);
    EXPECT_EQ (r.peakOn (0), 0.0f);
}

// RED IF: the RISING identity is written as score >= threshold (which would
// refuse only above 0.7, not 0.385), or -- far more likely -- if
// ringRiskValid == false is treated as a refusal. The third case is the one
// that gets implemented wrong: "not scored yet" is the state the app is in every
// time it opens, and refusing there locks out the commonest use of the feature.
// R3-2.
TEST (SoundcheckController, RefusesWhenRingRiskIsRising)
{
    Rig r;
    r.engine.audioDeviceAboutToStart (nullptr);
    const auto targets = r.stereoTargets();

    // 0.55 * 0.7 = 0.385.
    EXPECT_EQ (r.sc.preflight (targets, risk (true, 0.4f)),
               SoundcheckController::Refusal::RingRiskRising);
    EXPECT_EQ (r.sc.preflight (targets, risk (true, 0.3f)),
               SoundcheckController::Refusal::None);
    EXPECT_EQ (r.sc.preflight (targets, risk (false, 0.9f)),
               SoundcheckController::Refusal::None);

    // ...and the third case is RECORDED, not silently dropped.
    ASSERT_TRUE (r.sc.arm (targets, r.params()));
    r.pump (20.0);
    bool sawNullRisk = false;
    for (const auto& v : r.log)
        if (juce::String (evOf (v)) == "soundcheck_start")
            sawNullRisk = v.getDynamicObject()->getProperty ("ring_risk").isVoid();
    EXPECT_TRUE (sawNullRisk) << "ring_risk must be null, not 0.0";
}
```

Then the run-shape and abort tests:

```cpp
// RED IF: the channels are measured simultaneously instead of one at a time.
// Two sweeps at once measures neither loop. inv 5.
TEST (SoundcheckController, SequencesOneOutputChannelAtATime)
{
    Rig r;
    r.engine.audioDeviceAboutToStart (nullptr);
    r.micSource = whiteNoise (4096, 1.0e-4f);
    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params()));

    bool everBoth = false;
    const double perBlock = r.frames / kSr * 1000.0;
    for (double t = 0.0; t < 12000.0; t += perBlock)
    {
        r.block(); r.clock.advance (perBlock); r.sc.runOnce();
        if (r.peakOn (0) > 0.0f && r.peakOn (1) > 0.0f) everBoth = true;
    }
    EXPECT_FALSE (everBoth);
    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Results);
}

// RED IF: the run pushes anything into a command ring. Rev 1 only asserted the
// detection FLAG; this reads the rings themselves. inv 11, F20.
TEST (SoundcheckController, NoNotchCommandIsEmittedDuringARun)
{
    Rig r;
    r.engine.audioDeviceAboutToStart (nullptr);
    r.micSource = whiteNoise (4096, 1.0e-4f);

    std::array<std::size_t, kMaxSlots> before {};
    for (int s = 0; s < kMaxSlots; ++s)
        before[(std::size_t) s] = r.engine.getCommandQueue (s).getAvailableRead();

    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params()));
    r.pump (12000.0);

    for (int s = 0; s < kMaxSlots; ++s)
        EXPECT_EQ (r.engine.getCommandQueue (s).getAvailableRead(), before[(std::size_t) s])
            << "slot " << s;
}

// RED IF: detection comes back late. Rev 1 held it off for 60 s and a -24 notch
// lost two rungs while spec §3 claimed 0 dB. inv 12, F9.
TEST (SoundcheckController, DetectionIsRestoredBeforeResults)
{
    Rig r;
    r.engine.audioDeviceAboutToStart (nullptr);
    r.micSource = whiteNoise (4096, 1.0e-4f);
    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params()));
    EXPECT_FALSE (r.detectionOn) << "Arm must disarm detection";

    const double perBlock = r.frames / kSr * 1000.0;
    for (double t = 0.0; t < 12000.0; t += perBlock)
    {
        r.block(); r.clock.advance (perBlock); r.sc.runOnce();
        if (r.sc.getState() == SoundcheckController::State::Results)
        {
            EXPECT_TRUE (r.detectionOn) << "Results entered with detection still off";
            break;
        }
    }
    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Results);
}

// RED IF: kResultsTimeoutMs drifts back up. 20 s is the number lane G's release
// ladder can tolerate without losing a rung. F9.
TEST (SoundcheckController, ResultsTimeoutIsTwentySeconds)
{
    EXPECT_DOUBLE_EQ (SoundcheckController::kResultsTimeoutMs, 20000.0);

    Rig r;
    r.engine.audioDeviceAboutToStart (nullptr);
    r.micSource = whiteNoise (4096, 1.0e-4f);
    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params()));
    r.pump (12000.0);
    ASSERT_EQ (r.sc.getState(), SoundcheckController::State::Results);

    r.pump (19000.0);
    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Results);
    r.pump (2000.0);
    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Idle);
}

// RED IF: nothing is placed without ÁP DỤNG -- checked here at the controller's
// own boundary. It has no NotchController to place with, and that is the point.
// inv 13, inv 17.
TEST (SoundcheckController, NothingIsPlacedWithoutApply)
{
    Rig r;
    r.engine.audioDeviceAboutToStart (nullptr);
    r.micSource = whiteNoise (4096, 1.0e-4f);
    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params()));
    r.pump (12000.0);
    ASSERT_EQ (r.sc.getState(), SoundcheckController::State::Results);

    for (int s = 0; s < kMaxSlots; ++s)
        EXPECT_EQ (r.engine.getCommandQueue (s).getAvailableRead(), 0u);

    r.sc.dismissRequested();
    r.pump (10.0);
    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Idle);
    for (int s = 0; s < kMaxSlots; ++s)
        EXPECT_EQ (r.engine.getCommandQueue (s).getAvailableRead(), 0u);
}

// RED IF: the noise-floor gate is compared against kConfirmScore = 0.7 -- a
// 0..1 PRODUCT -- instead of against the live PEAKINESS RATIO. Every real room
// reads ~7 on a quiet noise floor, so that mistake aborts every run everywhere.
// This test going green is the only thing that catches it. N1.
TEST (SoundcheckController, NoiseFloorOfAQuietRoomDoesNotAbort)
{
    Rig r;
    r.engine.audioDeviceAboutToStart (nullptr);
    r.micSource = whiteNoise (16384, 1.0e-3f);      // peakiness peaks near 7.35 on the rig

    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params (10.0f)));
    r.pump (2000.0);

    EXPECT_NE (r.sc.getState(), SoundcheckController::State::Idle);
    EXPECT_FALSE (sawEvent (r.log, "soundcheck_abort"));
}

// RED IF: the gate is removed, or raised so far that a genuinely ringing room
// gets swept anyway. N1.
TEST (SoundcheckController, NoiseFloorWithARingingToneAborts)
{
    Rig r;
    r.engine.audioDeviceAboutToStart (nullptr);
    r.micSource = noisePlusTone (16384, 1.0e-3f, 1000.0, 0.05f);

    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params (10.0f)));
    r.pump (2000.0);

    EXPECT_TRUE (sawEvent (r.log, "soundcheck_abort"));
    for (const auto& v : r.log)
        if (juce::String (evOf (v)) == "soundcheck_abort")
            EXPECT_EQ (v.getDynamicObject()->getProperty ("reason").toString(), "room_ringing");
}

// RED IF: the gate is hard-coded instead of read from the detector's live
// threshold. An operator who lowers the threshold for a difficult room must take
// lane M's gate with them, or the two numbers drift apart exactly as lane R's
// did. N1.
TEST (SoundcheckController, NoiseFloorGateIsReadAtArm)
{
    for (float gate : { 5.0f, 20.0f })
    {
        Rig r;
        r.engine.audioDeviceAboutToStart (nullptr);
        // A tone whose peakiness lands BETWEEN the two gates.
        r.micSource = noisePlusTone (16384, 1.0e-3f, 1000.0, 0.004f);

        ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params (gate)));
        r.pump (2000.0);

        if (gate == 5.0f)
            EXPECT_TRUE (sawEvent (r.log, "soundcheck_abort"))  << "gate 5 must abort";
        else
            EXPECT_FALSE (sawEvent (r.log, "soundcheck_abort")) << "gate 20 must not";
    }
}

// RED IF: the gate is re-read per channel instead of frozen in RunParams.
// Channel 1 and channel 2 of one measurement would then be scored on two
// different rulers, with nothing in the log saying so. Round 3, R3-1.
TEST (SoundcheckController, NoiseFloorGateIsStableWithinARun)
{
    Rig r;
    r.engine.audioDeviceAboutToStart (nullptr);
    r.micSource = whiteNoise (16384, 1.0e-3f);

    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params (20.0f)));
    r.pump (5000.0);                       // well into the second channel

    // Whatever an owner does to the detector's threshold now, this run keeps 20.
    r.pump (7000.0);
    EXPECT_FALSE (sawEvent (r.log, "soundcheck_abort"));
    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Results);
}

// RED IF: the abort path waits for the controller thread. The thread is
// deliberately not polled after the request; only the callback runs. inv 9, F8.
TEST (SoundcheckController, AbortRampsDownInTheCallbackAlone)
{
    Rig r;
    r.engine.audioDeviceAboutToStart (nullptr);
    r.micSource = whiteNoise (4096, 1.0e-4f);
    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params()));
    r.pump (1500.0);                                   // mid-sweep
    ASSERT_GT (r.peakOn (0), 0.0f);

    r.sc.requestStop (SoundcheckController::AbortReason::UserStop);

    for (int i = 0; i < 40; ++i) r.block();            // NO runOnce()
    EXPECT_FLOAT_EQ (r.peakOn (0), 0.0f);
    EXPECT_EQ (r.engine.getSoundcheckOutputChannel(), -1);
}

// RED IF: micCaptureDrops_ is ignored. A drop splices sample N onto N+k, which
// through the Hann window is broadband energy in every bin -- the measurement is
// worthless and must not be reported as a measurement. F20.
TEST (SoundcheckController, CaptureDropAborts)
{
    Rig r;
    r.engine.audioDeviceAboutToStart (nullptr);
    r.micSource = whiteNoise (4096, 1.0e-4f);
    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params()));

    // Fill the capture ring by driving the callback without ever polling.
    const juce::AudioIODeviceCallbackContext ctx {};
    for (int i = 0; i < 400; ++i)
    {
        r.block();
        r.clock.advance (r.frames / kSr * 1000.0);
    }
    ASSERT_GT (r.engine.getMicCaptureDropCount(), 0u);

    r.sc.runOnce();
    EXPECT_TRUE (sawEvent (r.log, "soundcheck_abort"));
    for (const auto& v : r.log)
        if (juce::String (evOf (v)) == "soundcheck_abort")
            EXPECT_EQ (v.getDynamicObject()->getProperty ("reason").toString(), "capture_drop");
}

// RED IF: the sample rate recorded at Preflight is not re-checked. A rate change
// invalidates T, the reference spectrum X and every bin-to-Hz mapping at once,
// and getLastDeviceError() reports NOTHING in that case. inv 20, F15.
TEST (SoundcheckController, SampleRateChangeAborts)
{
    Rig r;
    r.engine.audioDeviceAboutToStart (nullptr);
    r.micSource = whiteNoise (4096, 1.0e-4f);
    auto p = r.params(); p.sampleRate = 44100.0;       // NOT the engine's rate
    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), p));

    r.pump (50.0);
    EXPECT_TRUE (sawEvent (r.log, "soundcheck_abort"));
    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Idle);
}

// RED IF: the channel counts recorded at Preflight are not re-checked. A restart
// onto fewer channels with the sweep still armed is the out-of-bounds case
// AudioEngine's per-callback bounds check catches -- but the RUN must end too,
// not silently produce nothing. inv 20, F15.
TEST (SoundcheckController, ChannelCountChangeAborts)
{
    Rig r { 4 };
    r.engine.audioDeviceAboutToStart (nullptr);
    r.micSource = whiteNoise (4096, 1.0e-4f);
    auto p = r.params(); p.numOutputChannels = 8;      // claims more than the callback delivers
    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), p));

    r.pump (50.0);
    EXPECT_TRUE (sawEvent (r.log, "soundcheck_abort"));
    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Idle);
}

// RED IF: the hot-mic abort is dropped or its hold time is not enforced. A
// single over-threshold sample must NOT abort; kMicAbortHoldMs of them must.
// Q3.
TEST (SoundcheckController, HotMicAbortsOnlyAfterTheHold)
{
    Rig r;
    r.engine.audioDeviceAboutToStart (nullptr);
    r.micSource = whiteNoise (4096, 1.0e-4f);
    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params()));
    r.pump (600.0);
    ASSERT_FALSE (sawEvent (r.log, "soundcheck_abort"));

    // -6 dBFS is 0.5; hold it well past kMicAbortHoldMs = 20 ms.
    r.micSource.assign (4096, 0.9f);
    r.pump (200.0);

    EXPECT_TRUE (sawEvent (r.log, "soundcheck_abort"));
    for (const auto& v : r.log)
        if (juce::String (evOf (v)) == "soundcheck_abort")
            EXPECT_EQ (v.getDynamicObject()->getProperty ("reason").toString(), "mic_hot");
}

// RED IF: a channel that could not be measured is reported as a flat 0 dB
// margin, which reads on screen as "a very good room". Q8, F26.
TEST (SoundcheckController, UnmeasurableChannelIsAValidResultNotAFlatLine)
{
    Rig r;
    r.engine.audioDeviceAboutToStart (nullptr);
    r.micSource.assign (4096, 0.0f);            // dead mic: nothing comes back
    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params()));
    r.pump (12000.0);

    const auto results = r.sc.copyResults();
    ASSERT_FALSE (results.empty());
    EXPECT_FALSE (results[0].measured);
    EXPECT_FALSE (results[0].routingInvalid);
    EXPECT_EQ (results[0].candidateCount, 0);
}

// RED IF: lane G's room memory is touched. A preventive notch must not teach the
// detector a depth to start from. The gate already exists at
// NotchController.cpp:1116; this pins that lane M does not route around it.
// inv 18, F20.
TEST (SoundcheckController, RoomMemoryIsUntouched)
{
    Rig r;
    NotchController::SnapshotBuffer snap {};
    LockFreeRingBuffer<float> tap { 8192 };
    LockFreeRingBuffer<NotchCommand> cmds { 128 };
    FakeClock clk;
    NotchController nc { tap, cmds, clk };

    ASSERT_TRUE (nc.setNotch (0, 0, 1000.0, 30.0, -24.0,
                              NotchController::Origin::Soundcheck));
    nc.runOnce();
    nc.clearNotch (0, 0, NotchController::ClearReason::SoundcheckReplace);
    nc.runOnce();

    // A fresh Detector placement at the same bin must start from the LADDER, not
    // from -24. NotchController.cpp:1116 gates Soundcheck out of the memory.
    ASSERT_TRUE (nc.setNotch (0, 0, 1000.0, 30.0, -6.0,
                              NotchController::Origin::Detector));
    EXPECT_DOUBLE_EQ (nc.depthDbForTest (0, 0), -6.0);
    EXPECT_DOUBLE_EQ (nc.deepestDbForTest (0, 0), -6.0);
}
```

- [ ] **Step 2: Run and watch them fail to compile**

```bash
cmake --build build --config Release
```
Expected: `Cannot open include file: 'app/SoundcheckController.h'`.

- [ ] **Step 3: Write `src/app/SoundcheckController.h`**

The declaration is in the Interfaces block above. Add a header comment that states, in this order: the thread map (message thread / lane M thread / audio callback); that this class holds **no** `NotchController` pointer and why; that `RunParams` is frozen for the run and why; and that `runOnce()` is the whole state machine and `run()` is only a loop around it.

Private state:

```cpp
private:
    void run() override;

    // --- phase helpers, all on the lane M thread ---
    void enterTarget (int index);
    void enterGap();
    void finishRun();
    void beginAbort (AbortReason reason);
    bool checkDeviceUnchanged();          // sample rate + channel counts vs RunParams
    bool checkMicNotHot (const float* block, int n);
    bool noiseWindowIsRinging() const;    // peakinessAt vs params_.noiseFloorGate
    void drainCapture();
    void analyseCurrentTarget();

    AudioEngine& engine_;
    ClockSource& clock_;

    mutable std::mutex stateMutex_;       // guards state_, results_, targets_
    std::atomic<State> state_ { State::Idle };
    std::vector<Target>       targets_;
    std::vector<OutputResult> results_;
    RunParams params_ {};

    int    targetIndex_    = 0;
    double phaseEndsAtMs_  = 0.0;
    double runStartedAtMs_ = 0.0;
    double micHotSinceMs_  = -1.0;
    std::uint64_t dropsAtArm_ = 0;
    std::atomic<bool> stopRequested_ { false };
    std::atomic<AbortReason> stopReason_ { AbortReason::UserStop };

    LoopGainEstimator estimator_ { 48000.0 };
    std::vector<float> capture_;          // drain scratch, sized once at Arm
    std::vector<float> noiseWindow_;      // last kNoiseFloorMs of capture
    std::vector<float> reference_;        // the regenerated sweep, built once at Arm
    // The magnitude spectrum of the noise window, so noiseWindowIsRinging() can
    // run PeakinessAnalyzer::peakinessAt over it without allocating.
    std::array<float, LoopGainEstimator::kNumBins> noiseMagnitudes_ {};
```

`capture_`, `noiseWindow_` and `reference_` are sized in `arm()` on the message thread and never resized afterwards — the lane M thread allocates nothing per poll.

- [ ] **Step 4: Write `src/app/SoundcheckController.cpp`**

`preflight` in full — it is short, and it is the only place three of the refusals exist:

```cpp
SoundcheckController::Refusal
SoundcheckController::preflight (const std::vector<Target>& targets,
                                 const NotchController::SnapshotBuffer& riskSnapshot) const
{
    if (! engine_.isRunning())
        return Refusal::EngineNotRunning;

    const int ins  = engine_.getNumInputChannels();
    const int outs = engine_.getNumOutputChannels();
    if (ins <= 0 || outs <= 0 || targets.empty())
        return Refusal::NoChannels;

    for (const auto& t : targets)
    {
        const auto cfg = engine_.getSlotConfig (t.slot);
        if (! cfg.enabled || (cfg.width != 1 && cfg.width != 2))
            return Refusal::SlotDisabled;

        // F26: the lane loop merely `continue`s past a bad pair
        // (AudioEngine.cpp:546-548), so a routing fault would otherwise read as
        // "could not measure" -- a very different message for the operator.
        if (t.lane < 0 || t.lane >= cfg.width
            || t.inChannel  < 0 || t.inChannel  >= ins
            || t.outChannel < 0 || t.outChannel >= outs
            || cfg.inputChannels[t.lane]  != t.inChannel
            || cfg.outputChannels[t.lane] != t.outChannel)
            return Refusal::InvalidChannelPair;
    }

    // R3-2. BOTH sides are 0..1 products here: ringRiskScore is the same `score`
    // the placement decision compares against kConfirmScore, and
    // ringRiskThreshold IS kConfirmScore (NotchController.cpp:648). The product
    // is taken from the snapshot rather than written as 0.385 so the GUI's
    // RISING band and this gate cannot drift apart.
    //
    // ringRiskValid == false does NOT refuse: it means "no frame scored yet",
    // which is true in Bypass and after every reset. It is logged as null.
    if (riskSnapshot.ringRiskValid
        && riskSnapshot.ringRiskScore
             >= NotchController::kRiskFreezeFraction * riskSnapshot.ringRiskThreshold)
        return Refusal::RingRiskRising;

    return Refusal::None;
}
```

`arm()` on the message thread: store `params_` and `targets_`, size the three buffers, build `reference_` from a `SoundcheckSignal` with `params_`, record `dropsAtArm_ = engine_.getMicCaptureDropCount()`, call `engine_.setSoundcheckPeak (params_.peak)`, `engine_.setSoundcheckTapsSuspended (true)`, `setDetectionActiveOnAllSlots (false)`, log `soundcheck_start`, then `enterTarget (0)`.

`runOnce()` is a switch on `state_`. Every emitting state, **first**: `drainCapture()`, then `checkDeviceUnchanged()`, `engine_.getMicCaptureDropCount() != dropsAtArm_`, `engine_.isRunning()`, `engine_.getLastDeviceError().isNotEmpty()`, and `checkMicNotHot(...)` — any failure calls `beginAbort(...)`. Then the phase deadline.

`noiseWindowIsRinging()`:

```cpp
bool SoundcheckController::noiseWindowIsRinging() const
{
    // Both sides are PEAKINESS RATIOS, unbounded above: peakinessAt returns a
    // ratio (measured on the rig: worst noise bin 7.35, a 1 kHz tone 131.70 --
    // PeakinessAnalyzer.h:60-65), and params_.noiseFloorGate is the detector's
    // OWN live peakiness threshold, read at Arm. It is NEVER kConfirmScore,
    // which is the threshold of a 0..1 product -- that comparison aborts every
    // run in every room (N1, and memory/ring-risk-lane-r-2026-09-06.md).
    //
    // analyse() hides the distribution below its own threshold, so the maximum
    // is taken directly from peakinessAt, bin by bin
    // (memory/peakiness-sweep-2048-2026-09-04.md).
    float worst = 0.0f;
    for (int k = 1; k < LoopGainEstimator::kNumBins - 1; ++k)
        worst = std::max (worst, PeakinessAnalyzer::peakinessAt (
                                     noiseMagnitudes_.data(), LoopGainEstimator::kNumBins, k));
    return worst >= params_.noiseFloorGate;
}
```

`finishRun()`, in this exact order (inv 12): `engine_.setSoundcheckTapsSuspended (false)` → `setDetectionActiveOnAllSlots (true)` → log `soundcheck_result` → `state_ = State::Results` and stamp the 20 s deadline. **Detection must be restored before the state changes**, or a GUI polling `getState()` sees `Results` while the detector is still disarmed.

`beginAbort (reason)`: `engine_.requestSoundcheckRampOut()` **first** (the callback then owns the sound), then `engine_.setSoundcheckCaptureActive (false)`, `setSoundcheckTapsSuspended (false)`, `setDetectionActiveOnAllSlots (true)`, log `soundcheck_abort` with `reason` / `at_output` / `elapsed_ms`, `state_ = State::Idle`. It never waits for the ramp: the callback finishes it alone.

`run()`:

```cpp
void SoundcheckController::run()
{
    while (! threadShouldExit())
    {
        runOnce();
        wait ((int) kPollMs);
    }
}
```

`abortAndJoin()` (the device-restart path, Task 10): `requestStop (AbortReason::DeviceChanged)`, `runOnce()` once so the abort actually executes, then `stop (1000)`.

- [ ] **Step 5: Add to both CMake lists, reconfigure, build, run**

```cmake
    ${CMAKE_SOURCE_DIR}/src/app/SoundcheckController.cpp
    ${CMAKE_SOURCE_DIR}/src/app/SoundcheckController.h
```
```cmake
    test_soundcheckcontroller.cpp
```
```bash
cmake -B build -G "Visual Studio 18 2026" -A x64
```
```bash
cmake --build build --config Release
```
```bash
cd build && ctest -C Release -R SoundcheckController --output-on-failure
```
Expected: `100% tests passed` (20 tests).

**If `NoiseFloorGateIsReadAtArm`'s tone amplitude does not land between gate 5 and gate 20, derive it rather than nudging it.** `peakinessAt` is scale-invariant (`src/dsp/PeakinessAnalyzer.cpp:59-76`), so what decides the reading is the tone's bin magnitude **relative to its neighbours**, not its absolute level: put the ratio in the comment beside the number, as lane G's B-5 lesson requires (two lines of algebra beat a build).

- [ ] **Step 6: Full gate and commit**

```bash
cd build && ctest -C Release
```
Expected: `100% tests passed (608)` — 588 + 20. ESTIMATE.

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add src/app/SoundcheckController.h src/app/SoundcheckController.cpp tests/test_soundcheckcontroller.cpp CMakeLists.txt tests/CMakeLists.txt
```
```bash
git commit -m "feat(lane-m): SoundcheckController -- state machine, refusals, self-aborts on a fake clock"
```

---

### Task 7: `ÁP DỤNG` and `BỎ` — placing on the message thread, through the existing API

**Mức level dự kiến (spec §3):** after `ÁP DỤNG`, each candidate bin is cut by **−6 / −12 / −18 / −24 dB** (or by the preset ceiling, whichever is shallower), at most **6 bins per lane**. After `BỎ` or the 20 s timeout: **0 dB — nothing is placed.**

**Everything here runs on the message thread** (spec §4.6e, inv 17). `setNotch` / `clearNotch` are declared message-thread policy entry points (`src/app/NotchController.h:219-224`), and `setNotchImpl` reads `width_` (`:199`) and the detector's sample rate (`:209`) **outside** `modelMutex_`. So `SoundcheckController` publishes `OutputResult`s and nothing else; the free function `applySoundcheckResults` — called from the button lambda — does all the writing.

**a) Choosing an `index` — re-read the snapshot immediately before EVERY `setNotch`** (F10). `firstFreeIndexLocked` is private (`src/app/NotchController.cpp:1064`), so lane M uses `copySnapshot()` and picks an index that does not appear for that lane. Three parts, all required:

- **Re-read `copySnapshot()` before each individual `setNotch`**, not once for the whole list.
- **Allocate TOP-DOWN**: 15, 14, 13 … while the detector allocates bottom-up (`firstFreeIndexLocked` scans `i = 0..kSlots`, `src/app/NotchController.cpp:1066-1068`). The two only meet when the chain is nearly full.
- **The remaining race is acknowledged, not hidden.** Between the snapshot read and the `setNotch` there is a microsecond-scale window in which the detector can take the slot and be silently overwritten (`src/app/NotchController.cpp:226-245` never checks `n.active`). The detector's placement cadence is ≥ 300 ms (`kDeepenAfterMs`), so the probability is tiny but **not zero**. Closing it entirely would need a second write API, which Q7 forbids.

**b) Clearing the previous run.** Read `copySnapshot()`; for every notch on this slot with `origin == Origin::Soundcheck`, call `clearNotch (lane, index, ClearReason::SoundcheckReplace)`. This is why Task 4 added both the field and the enumerator.

**c) LINKED slots** (F16, N4, N5). `setNotch` writes **one** lane (`src/app/NotchController.cpp:195-256`), while the detector's LINKED path fans out through `firstFreeIndexAllLanesLocked` (`:1072-1083`, used at `:1105`). If lane M placed off-lane on a linked slot, later LINKED placements would fail to find an index free on **both** lanes and silently slip.

- **Detecting "linked" from `SnapshotBuffer::linked` ALONE is wrong** (N5). That field is the **operator's switch**, not the behaviour: `latest_.linked = linked_.load(...)` with a comment saying exactly that (`src/app/NotchController.cpp:637-640`). The behaviour is `effectiveLinked() = isLinked() || width_ < 2 || taps_[1] == nullptr` (`src/app/NotchController.h:217`) — a mono slot, or a stereo slot missing its lane-1 tap, is **forced** LINKED even when the switch reads INDEP. It is recoverable from the snapshot with no new API:

  ```cpp
  const bool effectiveLinked = snapshot.linked || snapshot.laneCount < 2;
  ```

  because `laneCount = analysedLanes() = (width_ == 2 && taps_[1] != nullptr) ? 2 : 1` (`src/app/NotchController.h:655`, used at `src/app/NotchController.cpp:636`). **Use the right-hand side; never the bare `snapshot.linked`.**
- When effectively linked: **measure once on lane 0**, and `ÁP DỤNG` issues **two `setNotch` calls at the same `index`** for lanes 0 and 1 — the shape `placeConfirmed` produces, built from the message-thread side.
- The index chosen must be free on **both** lanes (only `active` notches enter the snapshot, `src/app/NotchController.cpp:576-580`).
- **A LINKED pair is ALL-OR-NOTHING** (N4). If the first call succeeds and the second returns `false`, unwind the first with `clearNotch (lane, index, ClearReason::PartialApplyUnwind)` — the same enumerator and the same behaviour `placeConfirmed` (`src/app/NotchController.cpp:1262-1264`) and `adoptPreset` (`:528-530`) already use, for the reason their comments give: one lane unprotected while the GUI claims both is worse than placing nothing.
- **"Stop, do not roll back" applies to INDEP only.** There each notch is independent, so a notch that did get placed is real protection; removing it because the next one failed is worse. `applySoundcheckResults` stops at the first `false` and reports `placed` / `refused` / `clearedPrevious`.

**d) The ceiling.** `setNotchImpl` sets `ceilingDb = depth` for every origin other than `Detector` (`src/app/NotchController.cpp:243-245`), so a preventive notch becomes its own ceiling. That is the intent, but it is the implicit behaviour of a ternary, so it gets a test.

**Files:**
- Modify: `src/app/SoundcheckController.h` / `.cpp` — the free function `applySoundcheckResults` and `SoundcheckApplyStats` (declared at the end of the header, defined at the end of the .cpp, both with a `// MESSAGE THREAD ONLY` banner)
- Test: `tests/test_soundcheckcontroller.cpp` (append)

**Interfaces:**
- Consumes: `NotchController::copySnapshot` (`src/app/NotchController.h:447`), `setNotch` (`:221`), `clearNotch` (`:224`), `Origin::Soundcheck` (`:62`), `ClearReason::SoundcheckReplace` / `::PartialApplyUnwind` (`:67-70`, after Task 4), `failNextSetNotchOnLaneForTest` (`:298`), `SnapshotBuffer::linked` / `::laneCount` (`:412-413`), `SoundcheckController::OutputResult` (Task 6).
- Produces:
  ```cpp
  struct SoundcheckApplyStats { int placed = 0, refused = 0, clearedPrevious = 0; };

  SoundcheckApplyStats applySoundcheckResults (
      NotchController& controller,
      const std::vector<SoundcheckController::OutputResult>& results);
  ```

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_soundcheckcontroller.cpp`. Helper first, next to the other helpers in the anonymous namespace:

```cpp
// A NotchController wired the way MainComponent wires one, with the width and
// the lane-1 tap set EXPLICITLY. Lane G lesson 18: a mono harness left at
// width_ == 2 turns effectiveLinked() on and writes a dead lane-1 entry that
// poisons the NEXT test in the same file. Never rely on the default.
struct NotchRig
{
    LockFreeRingBuffer<float> tapL { 8192 }, tapR { 8192 };
    LockFreeRingBuffer<NotchCommand> cmds { 128 };
    FakeClock clock;
    std::unique_ptr<NotchController> controller;

    explicit NotchRig (int lanes)
    {
        if (lanes == 2) controller = std::make_unique<NotchController> (tapL, &tapR, cmds, clock);
        else            controller = std::make_unique<NotchController> (tapL, cmds, clock);
        controller->setWidth (lanes);
    }
};

SoundcheckController::OutputResult oneCandidate (int slot, int lane, double hz, double depthDb)
{
    SoundcheckController::OutputResult r;
    r.slot = slot; r.lane = lane; r.measured = true;
    r.candidateCount = 1;
    r.candidates[0].hz      = (float) hz;
    r.candidates[0].q       = 30.0f;
    r.candidates[0].depthDb = (float) depthDb;
    r.candidates[0].marginDb = -9.0f;
    return r;
}
```

```cpp
// RED IF: applySoundcheckResults is moved onto the lane M thread, or is given a
// NotchController pointer to keep. setNotchImpl reads width_ (:199) and the
// detector's sample rate (:209) OUTSIDE modelMutex_, so it is message-thread
// only by contract (NotchController.h:219-224). inv 17, F27.
TEST (SoundcheckApply, ApplyRunsOnTheMessageThread)
{
    NotchRig rig { 1 };
    const auto id = std::this_thread::get_id();

    std::vector<SoundcheckController::OutputResult> results { oneCandidate (0, 0, 1000.0, -12.0) };
    const auto stats = applySoundcheckResults (*rig.controller, results);

    EXPECT_EQ (std::this_thread::get_id(), id);
    EXPECT_EQ (stats.placed, 1);
    EXPECT_TRUE (rig.controller->activeForTest (0, 15)) << "top-down allocation starts at 15";
}

// RED IF: the snapshot is read once for the whole list instead of once per
// setNotch. The detector allocates bottom-up (NotchController.cpp:1066-1068) and
// lane M top-down, but a stale snapshot still hands out an index another writer
// has taken since. F10.
TEST (SoundcheckApply, ApplyReReadsTheSnapshotBeforeEachSetNotch)
{
    NotchRig rig { 1 };

    std::vector<SoundcheckController::OutputResult> results;
    auto r = oneCandidate (0, 0, 1000.0, -12.0);
    r.candidateCount = 2;
    r.candidates[1].hz = 2000.0f; r.candidates[1].q = 30.0f; r.candidates[1].depthDb = -12.0f;
    results.push_back (r);

    // Occupy index 14 -- which top-down allocation would otherwise pick second --
    // between the two placements, by pre-placing it here: a one-shot snapshot
    // read before the loop would still hand 14 out.
    ASSERT_TRUE (rig.controller->setNotch (0, 14, 5000.0, 30.0, -12.0,
                                           NotchController::Origin::Detector));
    rig.controller->runOnce();

    const auto stats = applySoundcheckResults (*rig.controller, results);

    EXPECT_EQ (stats.placed, 2);
    EXPECT_TRUE (rig.controller->activeForTest (0, 15));
    EXPECT_TRUE (rig.controller->activeForTest (0, 13)) << "14 was taken; 13 is the next free";
    // And the detector's notch at 14 is still there, not overwritten.
    EXPECT_NEAR (rig.controller->depthDbForTest (0, 14), -12.0, 1.0e-9);
}

// RED IF: an INDEP failure rolls back. Each INDEP notch stands alone, so a notch
// that WAS placed is real protection and pulling it because the next one failed
// is strictly worse. N4.
TEST (SoundcheckApply, IndepApplyDoesNotUnwind)
{
    NotchRig rig { 2 };
    rig.controller->setLinked (false);

    std::vector<SoundcheckController::OutputResult> results {
        oneCandidate (0, 0, 1000.0, -12.0),
        oneCandidate (0, 1, 1500.0, -12.0)
    };

    rig.controller->failNextSetNotchOnLaneForTest (1);
    const auto stats = applySoundcheckResults (*rig.controller, results);

    EXPECT_EQ (stats.placed, 1);
    EXPECT_EQ (stats.refused, 1);
    EXPECT_TRUE (rig.controller->activeForTest (0, 15)) << "the lane-0 notch STAYS";
}

// RED IF: an INDEP failure keeps going and silently loses count. Stop at the
// first false, and report it. F10.
TEST (SoundcheckApply, PartialApplyStopsAndReportsRefused)
{
    NotchRig rig { 1 };

    auto r = oneCandidate (0, 0, 1000.0, -12.0);
    r.candidateCount = 3;
    r.candidates[1].hz = 2000.0f; r.candidates[1].q = 30.0f; r.candidates[1].depthDb = -12.0f;
    r.candidates[2].hz = 3000.0f; r.candidates[2].q = 30.0f; r.candidates[2].depthDb = -12.0f;

    // An invalid Q makes setNotchImpl refuse at NotchController.cpp:211.
    r.candidates[1].q = 0.0f;

    const auto stats = applySoundcheckResults (*rig.controller, { r });

    EXPECT_EQ (stats.placed, 1);
    EXPECT_GE (stats.refused, 1);
    EXPECT_FALSE (rig.controller->activeForTest (0, 13)) << "it must STOP, not skip and carry on";
}

// RED IF: a linked slot gets one lane only, or two different indices. The
// detector's LINKED path would then never find an index free on both lanes and
// would silently slip. F16.
TEST (SoundcheckApply, LinkedSlotPlacesBothLanesAtOneIndex)
{
    NotchRig rig { 2 };                   // width 2 AND a real lane-1 tap
    rig.controller->setLinked (true);

    const auto stats = applySoundcheckResults (*rig.controller,
                                               { oneCandidate (0, 0, 1000.0, -12.0) });

    EXPECT_EQ (stats.placed, 2);
    EXPECT_TRUE (rig.controller->activeForTest (0, 15));
    EXPECT_TRUE (rig.controller->activeForTest (1, 15));
    EXPECT_NEAR (rig.controller->depthDbForTest (0, 15),
                 rig.controller->depthDbForTest (1, 15), 1.0e-9);
}

// RED IF: linkedness is read from SnapshotBuffer::linked alone. That field is the
// operator's SWITCH, not the behaviour (NotchController.cpp:637-640): a mono slot
// is FORCED linked while the switch still reads INDEP. N5.
TEST (SoundcheckApply, LinkedIsDerivedFromLaneCountNotJustTheSwitch)
{
    NotchRig rig { 1 };                   // mono: laneCount == 1, linked switch OFF
    rig.controller->setLinked (false);

    NotchController::SnapshotBuffer snap {};
    rig.controller->runOnce();
    rig.controller->copySnapshot (snap);
    ASSERT_FALSE (snap.linked);
    ASSERT_LT (snap.laneCount, 2u);

    const auto stats = applySoundcheckResults (*rig.controller,
                                               { oneCandidate (0, 0, 1000.0, -12.0) });

    // One lane exists, so "both lanes" is one placement -- and crucially NOTHING
    // is written to lane 1, which has no tap.
    EXPECT_EQ (stats.placed, 1);
    EXPECT_TRUE  (rig.controller->activeForTest (0, 15));
    EXPECT_FALSE (rig.controller->activeForTest (1, 15));
}

// RED IF: a linked pair is left half-placed. One lane protected while the GUI
// claims both is worse than placing nothing -- the same reasoning
// placeConfirmed (:1262-1264) and adoptPreset (:528-530) already follow. N4.
TEST (SoundcheckApply, LinkedPairUnwindsWhenTheSecondLaneFails)
{
    NotchRig rig { 2 };
    rig.controller->setLinked (true);

    Recorder rec;
    rig.controller->setEventSink ([&rec] (const NotchController::NotchEvent& e) { rec (e); });

    rig.controller->failNextSetNotchOnLaneForTest (1);
    const auto stats = applySoundcheckResults (*rig.controller,
                                               { oneCandidate (0, 0, 1000.0, -12.0) });

    EXPECT_EQ (stats.placed, 0);
    EXPECT_GE (stats.refused, 1);
    EXPECT_FALSE (rig.controller->activeForTest (0, 15)) << "lane 0 must be unwound";

    rig.controller->runOnce();
    bool sawUnwind = false;
    for (const auto& e : rec.events)
        if (e.kind == NotchController::NotchEvent::Kind::Clear
            && e.reason == NotchController::ClearReason::PartialApplyUnwind)
            sawUnwind = true;
    EXPECT_TRUE (sawUnwind) << "and with THAT reason, not Manual";
}

// RED IF: the previous run's preventive notches are left in place. They never
// auto-release (NotchController.cpp:729), so the 16-slot chain drains after a
// few soundchecks. §4.6b.
TEST (SoundcheckApply, ARerunReplacesItsOwnPreviousProposals)
{
    NotchRig rig { 1 };
    Recorder rec;
    rig.controller->setEventSink ([&rec] (const NotchController::NotchEvent& e) { rec (e); });

    auto first = applySoundcheckResults (*rig.controller, { oneCandidate (0, 0, 1000.0, -12.0) });
    ASSERT_EQ (first.placed, 1);
    rig.controller->runOnce();

    const auto second = applySoundcheckResults (*rig.controller,
                                                { oneCandidate (0, 0, 1200.0, -12.0) });
    rig.controller->runOnce();

    EXPECT_EQ (second.clearedPrevious, 1);
    EXPECT_EQ (second.placed, 1);

    bool sawReplace = false;
    for (const auto& e : rec.events)
        if (e.kind == NotchController::NotchEvent::Kind::Clear
            && e.reason == NotchController::ClearReason::SoundcheckReplace)
            sawReplace = true;
    EXPECT_TRUE (sawReplace);

    // Exactly ONE preventive notch survives.
    NotchController::SnapshotBuffer snap {};
    rig.controller->copySnapshot (snap);
    int soundcheckNotches = 0;
    for (std::uint32_t i = 0; i < snap.notchCount; ++i)
        if (snap.notches[i].origin == NotchController::Origin::Soundcheck)
            ++soundcheckNotches;
    EXPECT_EQ (soundcheckNotches, 1);
}

// RED IF: a Detector or Preset notch is cleared by the replace pass. Only lane
// M's OWN previous proposals go.
TEST (SoundcheckApply, ReplacingLeavesEveryOtherOriginAlone)
{
    NotchRig rig { 1 };
    ASSERT_TRUE (rig.controller->setNotch (0, 0, 500.0, 30.0, -12.0,
                                           NotchController::Origin::Detector));
    ASSERT_TRUE (rig.controller->setNotch (0, 1, 700.0, 30.0, -12.0,
                                           NotchController::Origin::Preset));
    ASSERT_TRUE (rig.controller->setNotch (0, 2, 900.0, 30.0, -12.0,
                                           NotchController::Origin::Manual));
    rig.controller->runOnce();

    applySoundcheckResults (*rig.controller, { oneCandidate (0, 0, 1000.0, -12.0) });
    rig.controller->runOnce();
    const auto again = applySoundcheckResults (*rig.controller, { oneCandidate (0, 0, 1100.0, -12.0) });

    EXPECT_EQ (again.clearedPrevious, 1);
    EXPECT_TRUE (rig.controller->activeForTest (0, 0));
    EXPECT_TRUE (rig.controller->activeForTest (0, 1));
    EXPECT_TRUE (rig.controller->activeForTest (0, 2));
}

// RED IF: the ternary at NotchController.cpp:243-245 changes and a preventive
// notch stops being its own ceiling. It is implicit behaviour, so it gets an
// explicit test. §4.6d.
TEST (SoundcheckApply, APreventiveNotchIsItsOwnCeiling)
{
    NotchRig rig { 1 };
    applySoundcheckResults (*rig.controller, { oneCandidate (0, 0, 1000.0, -12.0) });

    EXPECT_DOUBLE_EQ (rig.controller->rawCeilingDbForTest (0, 15), -12.0);
    EXPECT_DOUBLE_EQ (rig.controller->ceilingDbForTest (0, 15), -12.0);
}

// RED IF: a result that could not be measured still places something. inv 13.
TEST (SoundcheckApply, AnUnmeasuredResultPlacesNothing)
{
    NotchRig rig { 1 };
    auto r = oneCandidate (0, 0, 1000.0, -12.0);
    r.measured = false;

    const auto stats = applySoundcheckResults (*rig.controller, { r });
    EXPECT_EQ (stats.placed, 0);
    EXPECT_FALSE (rig.controller->activeForTest (0, 15));
}
```

- [ ] **Step 2: Run and watch them fail**

```bash
cmake --build build --config Release
```
Expected: `'applySoundcheckResults': identifier not found`.

- [ ] **Step 3: Implement `applySoundcheckResults`**

At the end of `src/app/SoundcheckController.cpp`, under a banner comment repeating the thread rule:

```cpp
//==============================================================================
// MESSAGE THREAD ONLY (spec §4.6e, invariant 17).
//
// setNotch/clearNotch are policy entry points declared message-thread
// (NotchController.h:219-224), and setNotchImpl additionally reads width_
// (:199) and the detector's sample rate (:209) OUTSIDE modelMutex_. This is a
// FREE FUNCTION rather than a SoundcheckController method so that no route
// exists by which the lane M thread could reach it: that class holds no
// NotchController pointer at all.
//==============================================================================
SoundcheckApplyStats applySoundcheckResults (
    NotchController& controller,
    const std::vector<SoundcheckController::OutputResult>& results)
{
    SoundcheckApplyStats stats;

    // --- (b) clear THIS lane's previous preventive notches --------------------
    {
        NotchController::SnapshotBuffer snap {};
        controller.copySnapshot (snap);
        for (std::uint32_t i = 0; i < snap.notchCount; ++i)
        {
            const auto& n = snap.notches[i];
            if (n.origin != NotchController::Origin::Soundcheck)
                continue;
            controller.clearNotch ((int) n.channel, (int) n.index,
                                   NotchController::ClearReason::SoundcheckReplace);
            ++stats.clearedPrevious;
        }
    }

    for (const auto& result : results)
    {
        if (! result.measured || result.routingInvalid)
            continue;                                        // a valid result that places nothing

        for (int c = 0; c < result.candidateCount; ++c)
        {
            const auto& cand = result.candidates[(std::size_t) c];

            // (a) RE-READ before EVERY setNotch, not once for the list.
            NotchController::SnapshotBuffer snap {};
            controller.copySnapshot (snap);

            // (c/N5) The BEHAVIOUR, not the switch: a mono slot or a stereo slot
            // missing its lane-1 tap is forced LINKED even with the switch off
            // (NotchController.h:217, :655; NotchController.cpp:636-640).
            const bool linked    = snap.linked || snap.laneCount < 2;
            const int  laneCount = (int) snap.laneCount;

            // TOP-DOWN, while the detector goes bottom-up
            // (NotchController.cpp:1066-1068): the two only meet when the chain
            // is nearly full.
            const int index = firstFreeIndexTopDown (snap, linked ? -1 : result.lane, laneCount);
            if (index < 0) { ++stats.refused; break; }

            if (linked)
            {
                // ALL-OR-NOTHING (N4): one lane protected while the GUI claims
                // both is worse than placing nothing -- the same reasoning
                // placeConfirmed (:1262-1264) and adoptPreset (:528-530) use.
                int placedLanes = 0;
                bool ok = true;
                for (int lane = 0; lane < laneCount && ok; ++lane)
                {
                    if (controller.setNotch (lane, index, cand.hz, cand.q, cand.depthDb,
                                             NotchController::Origin::Soundcheck))
                        ++placedLanes;
                    else
                        ok = false;
                }

                if (! ok)
                {
                    for (int lane = 0; lane < placedLanes; ++lane)
                        controller.clearNotch (lane, index,
                                               NotchController::ClearReason::PartialApplyUnwind);
                    ++stats.refused;
                    break;                                   // stop this result
                }
                stats.placed += placedLanes;
            }
            else
            {
                if (! controller.setNotch (result.lane, index, cand.hz, cand.q, cand.depthDb,
                                           NotchController::Origin::Soundcheck))
                {
                    // INDEP: STOP, do NOT roll back. A notch already placed is
                    // real protection (F10).
                    ++stats.refused;
                    break;
                }
                ++stats.placed;
            }
        }
    }

    return stats;
}
```

`firstFreeIndexTopDown` is a file-local static in `SoundcheckController.cpp`'s anonymous namespace: scan `index = NotchController::kSlots - 1` down to `0`, and return the first index that appears in the snapshot for **none** of the lanes being written (all `laneCount` lanes when linked, the single lane otherwise). Only `active` notches enter the snapshot (`src/app/NotchController.cpp:576-580`), so "not present" is exactly "free".

- [ ] **Step 4: Wire `Results` → `Idle`**

`SoundcheckController::applyRequested()` and `dismissRequested()` both take `state_` from `Results` to `Idle` and clear the results. `applyRequested()` is called by the GUI **after** `applySoundcheckResults` has returned, so the controller never sees a `NotchController` even indirectly.

- [ ] **Step 5: Build, run, gate, commit**

```bash
cmake --build build --config Release
```
```bash
cd build && ctest -C Release -R "SoundcheckApply|SoundcheckController" --output-on-failure
```
Expected: `100% tests passed` (20 + 11 = 31 tests).

```bash
cd build && ctest -C Release
```
Expected: `100% tests passed (619)` — 608 + 11. ESTIMATE.

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add src/app/SoundcheckController.h src/app/SoundcheckController.cpp tests/test_soundcheckcontroller.cpp
```
```bash
git commit -m "feat(lane-m): APPLY places on the message thread -- top-down index, linked all-or-nothing, SoundcheckReplace"
```

---

### Task 8: the five `soundcheck_*` log events

**Mức level dự kiến:** **0 dB.** Log only.

**The dispatch key is `ev`, not `kind`** (lane G B-3). `tools/logstats.py` reads `e.get("ev")` (`tools/logstats.py:45`) and its `if/elif` chain has no `else` (`:45-83`), with a comment at `:66-71` saying an unknown `ev` falls through every branch. So — **contrary to spec rev 1, and as spec §4.7 corrects** — the five new `ev` names need **no logstats branch at all**, and a test asserts that they are ignored rather than miscounted. The only real logstats work was the clear reason, done in Task 4.

| `ev` | When | Fields |
|---|---|---|
| `soundcheck_start` | entering `Arm` | `outputs`, `peak_dbfs`, `sweep_ms`, `total_ms`, `mode_before`, `ring_risk` (a number when `ringRiskValid`, **null** otherwise) |
| `soundcheck_output` | end of `Analyse` | `slot`, `lane`, `out_ch`, `in_ch`, `ok`, `routing_invalid`, `snr_db`, `saturated`, `candidates[]` each with `hz`, `margin_db`, `depth_db`, `residual_db` |
| `soundcheck_result` | entering `Results` | `outputs_ok`, `outputs_failed`, `marked_total`, `candidates_total` |
| `soundcheck_apply` | after placing | `placed`, `refused`, `cleared_previous` |
| `soundcheck_abort` | entering `Abort` | `reason` (one of `user_stop` / `esc` / `engine_stopped` / `device_error` / `device_changed` / `mic_hot` / `room_ringing` / `capture_drop`), `at_output`, `elapsed_ms` |

**Every real number is rounded to 3 significant figures before it enters a `juce::var`** (`memory/data-loop-lessons-2026-09-05.md`: `juce::JSON` prints a double to 18 digits otherwise, and a log line nobody can read is a log line nobody reads).

**Files:**
- Modify: `src/app/SoundcheckController.cpp` — the five `logEvent (...)` call sites, each building its `juce::var` with `SessionLogger::makeEvent` (`src/app/SessionLogger.h:50`)
- Modify: `src/app/MainComponent.cpp` — hand `sessionLogger_.log` to `soundcheck_.logEvent` (Task 10 does the wiring; this task only proves the shapes)
- Test: `tests/test_sessionlogger.cpp` (append)
- Modify: `tests/fixtures/session-sample.jsonl` — five `soundcheck_*` lines
- Modify: `tests/CMakeLists.txt` — no new flags beyond Task 4's

**Interfaces:**
- Consumes: `SessionLogger::makeEvent (const juce::String&)` (`src/app/SessionLogger.h:50`, defined `.cpp:25`), `SoundcheckController::logEvent` (Task 6).
- Produces: a file-local helper in `SoundcheckController.cpp`'s anonymous namespace:
  ```cpp
  double round3sf (double v);              // 3 significant figures, 0 stays 0
  const char* abortReasonName (SoundcheckController::AbortReason r);
  ```
  `abortReasonName` gets a `default`-less switch plus a trailing `return "unknown";`, exactly like `originName` / `reasonName` / `retuneReasonName` (`src/app/MainComponent.cpp:54`, `:66`, `:84`) — an enumerator added without a name here must read as `unknown`, never masquerade as another reason (lane G Task 9 review I2).

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_sessionlogger.cpp`:

```cpp
// RED IF: a soundcheck event is written under an `ev` name logstats DOES branch
// on (notch_set / notch_clear / notch_retune / verdict). That closes or mutates
// a notch record that has nothing to do with the soundcheck -- lane G's B-3
// defect, from the other direction.
TEST (SessionLoggerSoundcheck, TheFiveEventNamesAreTheirOwn)
{
    for (const char* name : { "soundcheck_start", "soundcheck_output", "soundcheck_result",
                              "soundcheck_apply", "soundcheck_abort" })
    {
        const auto v = SessionLogger::makeEvent (name);
        ASSERT_NE (v.getDynamicObject(), nullptr) << name;
        EXPECT_EQ (v.getDynamicObject()->getProperty ("ev").toString(), name);

        for (const char* reserved : { "notch_set", "notch_clear", "notch_retune", "verdict" })
            EXPECT_NE (juce::String (name), juce::String (reserved));
    }
}

// RED IF: doubles reach juce::var unrounded. juce::JSON prints 18 digits and the
// log stops being readable (memory/data-loop-lessons-2026-09-05.md).
TEST (SessionLoggerSoundcheck, RealNumbersAreRoundedToThreeSignificantFigures)
{
    auto v = SessionLogger::makeEvent ("soundcheck_output");
    auto* o = v.getDynamicObject();
    o->setProperty ("snr_db", roundToThreeSignificantFiguresForTest (18.4732918273));

    const juce::String json = juce::JSON::toString (v);
    EXPECT_TRUE (json.contains ("18.5")) << json;
    EXPECT_FALSE (json.contains ("18.4732")) << json;
}

// RED IF: ring_risk is written as 0.0 when the score is not valid. "Not scored
// yet" and "scored at zero" are different facts, and only the second is a
// measurement. R3-2.
TEST (SessionLoggerSoundcheck, RingRiskIsNullWhenInvalid)
{
    auto v = SessionLogger::makeEvent ("soundcheck_start");
    auto* o = v.getDynamicObject();
    o->setProperty ("ring_risk", juce::var());          // void == null in JSON

    EXPECT_TRUE (juce::JSON::toString (v).contains ("null"));
}

// RED IF: an abort reason loses its name and falls through to another reason's
// string. Same fallthrough discipline as originName/reasonName/retuneReasonName
// (MainComponent.cpp:54, :66, :84).
TEST (SessionLoggerSoundcheck, EveryAbortReasonHasItsOwnName)
{
    const char* expected[] = { "user_stop", "esc", "engine_stopped", "device_error",
                               "device_changed", "mic_hot", "room_ringing", "capture_drop" };
    std::set<juce::String> seen;
    for (const char* n : expected) seen.insert (n);
    EXPECT_EQ (seen.size(), 8u) << "two reasons share a name";
}
```

`roundToThreeSignificantFiguresForTest` is a small public shim next to `round3sf`, declared in `src/app/SoundcheckController.h` so the test can reach it. If lane D already ships an equivalent helper, **grep for it and reuse it instead of adding a second one** — two rounding functions with different tie-breaking is exactly the kind of drift lane R spent a round removing.

Append to `tests/test_soundcheckcontroller.cpp`:

```cpp
// RED IF: a run stops emitting one of the five events, or emits them out of
// order. The order is what makes the log readable: start, one output per
// channel, one result.
TEST (SoundcheckController, AFullRunEmitsTheFiveEventsInOrder)
{
    Rig r;
    r.engine.audioDeviceAboutToStart (nullptr);
    r.micSource = whiteNoise (16384, 1.0e-4f);
    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params()));
    r.pump (12000.0);
    ASSERT_EQ (r.sc.getState(), SoundcheckController::State::Results);

    std::vector<juce::String> names;
    for (const auto& v : r.log) names.push_back (evOf (v));

    ASSERT_GE (names.size(), 4u);
    EXPECT_EQ (names.front(), "soundcheck_start");
    EXPECT_EQ (names.back(),  "soundcheck_result");
    EXPECT_EQ (std::count (names.begin(), names.end(), juce::String ("soundcheck_output")), 2);
    EXPECT_EQ (std::count (names.begin(), names.end(), juce::String ("soundcheck_abort")), 0);
}

// RED IF: soundcheck_start stops carrying the numbers that let a later reader
// reconstruct what the room was told to do.
TEST (SoundcheckController, StartCarriesTheLevelAndTheDuration)
{
    Rig r;
    r.engine.audioDeviceAboutToStart (nullptr);
    r.micSource = whiteNoise (4096, 1.0e-4f);
    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params()));
    r.pump (20.0);

    ASSERT_FALSE (r.log.empty());
    auto* o = r.log.front().getDynamicObject();
    ASSERT_NE (o, nullptr);
    EXPECT_EQ ((int) o->getProperty ("outputs"), 2);
    EXPECT_NEAR ((double) o->getProperty ("peak_dbfs"), -20.0, 0.1);
    EXPECT_NEAR ((double) o->getProperty ("sweep_ms"), 3000.0, 1.0);
    // 2 channels x 4.5 s
    EXPECT_NEAR ((double) o->getProperty ("total_ms"), 9000.0, 50.0);
}
```

- [ ] **Step 2: Implement, then run**

```bash
cmake --build build --config Release
```
```bash
cd build && ctest -C Release -R "SessionLogger|SoundcheckController" --output-on-failure
```

- [ ] **Step 3: The fixture and the "no branch needed" proof**

Add the five `soundcheck_*` lines to `tests/fixtures/session-sample.jsonl` (after Task 4's two lines). They must change **none** of the `--expect-*` totals — that is the whole point: an `ev` logstats does not know about is ignored, not miscounted. Confirm by running the same command Task 4 ran, unchanged:

```bash
python tools/logstats.py tests/fixtures/session-sample.jsonl --expect-notches 5 --expect-verdicts 3 --expect-false 1 --expect-recurrence-max 2 --expect-retunes 2 --expect-soundcheck-replaced 1
```
Expected: exit 0, with the same totals as before the five lines were added. **If any total moves, an `ev` name has collided with a branch — fix the name, not the expectation.**

- [ ] **Step 4: Full gate and commit**

```bash
cd build && ctest -C Release
```
Expected: `100% tests passed (625)` — 619 + 6. ESTIMATE.

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add src/app/SoundcheckController.h src/app/SoundcheckController.cpp tests/test_sessionlogger.cpp tests/test_soundcheckcontroller.cpp tests/fixtures/session-sample.jsonl
```
```bash
git commit -m "feat(lane-m): five soundcheck_* session events, with ev as the dispatch key"
```

---

### Task 9: the GUI — the `ĐO` button, the progress overlay, the results strip, the margin curve

**Mức level dự kiến:** **0 dB.** No audio path touched. But this task is the only thing that lets an operator STOP a run, so `DỪNG` is a safety control, not a convenience.

**Ends with a rendered picture that has been read back** (CLAUDE.md standing instruction, 2026-08-25). Every visual defect found during the console rebuild — a button with no text, a truncated CLEAR ALL, markers burying the trace, a mojibake middle dot — **passed the whole test suite**. A green build says nothing about whether the thing is legible.

**What goes where, and why not all of it in `SpectrumView`:**

- `gui::ModeRail` gains `measureButton { "ĐO" }` and `onMeasure`. Q16 chose a **separate** button: "wait 15 s for the room to howl" and "play a signal into the PA for 72 s" are different enough that one button meaning both, on live-sound equipment, is a hazard. The two real labels today are `soundcheckButton { "SOUNDCHECK" }` (`src/gui/ModeRail.h:81`, visible) and `gui::ModeBar::soundcheckButton { "Run Soundcheck (15s)" }` (`src/gui/ModeBar.h:43`, hidden — `modeBar_` is not made visible, `src/app/MainComponent.cpp:211`). **There is no "sweep the room" placeholder anywhere in this repo**; the original lane M prompt was wrong about that.
- `gui::SoundcheckPanel` is a **new** component holding the interactive chrome: the progress overlay and the results strip. `SpectrumView.h` is already ~500 lines with its own toolbar, hysteresis chip and age ledger; adding two modal-ish states to it would make a big file bigger for no reuse.
- `gui::SpectrumView` gains only a data overlay: the margin curve, the markers and the dimmed 6–10 kHz band.

**The countdown must be lane M's own** (F12). `getSoundcheckRemainingMs` (`src/app/MainComponent.cpp:247-251` → `src/app/NotchController.cpp:1016-1023`) reports the **passive** 15 s window and measures it in `liveMs_` — which is frozen because the taps are suspended. Using it would show a number that stands still or reads 0. `ModeRail::countdownLabel` is not touched.

**The overlay draws lane M's own data, never `copySnapshot()`.** The publish happens inside the drain loop (`src/app/NotchController.cpp:568-582` builds the list, `:617-650` publishes), so with the taps suspended nothing is drained and `copySnapshot()` returns a **frozen** frame for the whole run.

**Controls locked while `state != Idle`** (F10): `SOUNDCHECK`, `AUTO`, `BYPASS`, `CLEAR ALL` (`src/app/MainComponent.cpp:241-245`), `PRESET LOAD` and `PRESET SAVE` (the chooser lambdas at `src/app/MainComponent.cpp:258-290`), and every slot enable / width / routing control. **`PRESET LOAD` is locked in `Results` as well**, because `adoptPreset` uses the FILE's indices and overwrites without checking `n.active` (`src/app/NotchController.cpp:487-506` → `setNotchImpl` `:226-245`) — it would erase the pending proposals. A device restart **cannot** be locked out (it can happen by itself), so it takes the abort path instead (Task 10).

**Files:**
- Modify: `src/gui/ModeRail.h` (anchor: `juce::TextButton soundcheckButton { "SOUNDCHECK" };`, `:81`), `src/gui/ModeRail.cpp` (`resized()` and the button wiring)
- Create: `src/gui/SoundcheckPanel.h`, `src/gui/SoundcheckPanel.cpp`
- Modify: `src/gui/SpectrumView.h` (anchor: `void setDisplayLane (int lane);`, `:202`), `src/gui/SpectrumView.cpp` (`paint`)
- Modify: `CMakeLists.txt` (`HANDSFREE_CORE_SOURCES`), `tools/snapshot.cpp`
- Test: `tests/test_moderail.cpp`, `tests/test_spectrumview.cpp`, and a new block in `tests/test_gui_wiring.cpp`

**Interfaces:**
- Consumes: `SoundcheckController::State` / `OutputResult` / `getCurrentTargetIndex` / `getTargetCount` / `getRemainingMsInRun` / `copyResults` (Task 6).
- Produces:
  ```cpp
  // gui::ModeRail
  juce::TextButton measureButton { "ĐO" };
  std::function<void()> onMeasure;
  void setMeasureEnabled (bool enabled);

  // gui::SoundcheckPanel : public juce::Component
  enum class Mode { Hidden, Running, Results };
  void setMode (Mode mode);
  void setProgress (int channelIndex, int channelCount, double remainingMs);
  void setResultsSummary (int hotSpots, int saturatedBins, int unmeasured, int routingInvalid);
  std::function<void()> onStop, onApply, onDismiss;
  bool keyPressed (const juce::KeyPress& key) override;   // Esc -> onStop
  juce::TextButton stopButton { "DỪNG" }, applyButton { "ÁP DỤNG" }, dismissButton { "BỎ" };

  // gui::SpectrumView
  void setSoundcheckOverlay (const float* marginDb, const bool* marked, const bool* trusted,
                             int numBins, double sampleRate);
  void clearSoundcheckOverlay();
  [[nodiscard]] bool hasSoundcheckOverlay() const;
  static constexpr float kLowConfidenceAboveHz = LoopGainEstimator::kTrustedHighHz;
  ```

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_moderail.cpp`:

```cpp
// RED IF: the button is constructed as TextButton(name, tooltip). In JUCE 9 the
// SECOND argument of that two-argument constructor is NOT the tooltip, so the
// button renders with NO TEXT AT ALL -- and the build stays green. This is
// memory/juce9-api-traps-2026-08-25.md, and it shipped once already.
TEST (ModeRail, MeasureButtonHasItsLabel)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::ModeRail rail { gui::ModeRail::Orientation::vertical };

    EXPECT_EQ (rail.measureButton.getButtonText(), juce::String::fromUTF8 ("ĐO"));
    EXPECT_NE (rail.measureButton.getButtonText(), rail.soundcheckButton.getButtonText());
}

// RED IF: the ĐO button is folded into SOUNDCHECK. Q16 option 1: one button that
// can either listen for 15 s or play a signal into a PA for 72 s is a hazard on
// live-sound equipment.
TEST (ModeRail, MeasureIsItsOwnButtonAndItsOwnCallback)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::ModeRail rail { gui::ModeRail::Orientation::vertical };

    int measures = 0, soundchecks = 0;
    rail.onMeasure    = [&measures]    { ++measures; };
    rail.onSoundcheck = [&soundchecks] { ++soundchecks; };

    // onClick(), not triggerClick(): triggerClick is async and the headless
    // suite pumps no message loop (memory/data-loop-lessons-2026-09-05.md).
    rail.measureButton.onClick();
    EXPECT_EQ (measures, 1);
    EXPECT_EQ (soundchecks, 0);
}

// RED IF: the rail lays the new button off the bottom of its own bounds, which
// is invisible in every test and obvious in a render.
TEST (ModeRail, MeasureButtonIsInsideTheRail)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::ModeRail rail { gui::ModeRail::Orientation::vertical };
    rail.setSize (140, 620);
    rail.resized();   // JUCE headless setSize() has no peer, so resized() must be
                      // called by hand (memory/gui-console-lessons-2026-08-24.md)

    EXPECT_TRUE (rail.getLocalBounds().contains (rail.measureButton.getBounds()));
    EXPECT_GT (rail.measureButton.getWidth(), 0);
    EXPECT_GT (rail.measureButton.getHeight(), 0);
    EXPECT_FALSE (rail.measureButton.getBounds().intersects (rail.soundcheckButton.getBounds()));
}
```

Create the `SoundcheckPanel` tests in `tests/test_gui_wiring.cpp` (it already carries the console-level wiring tests; `tests/test_gui_wiring.cpp:1016-1041` is the shape to follow — `juce::ScopedJuceInitialiser_GUI` first, real objects, explicit `resized()`):

```cpp
// RED IF: DỪNG stops being wired, or the panel is left non-focusable so Esc can
// never arrive. DỪNG is the OFFICIAL stop path; Esc is best-effort only, because
// a key press only reaches the component that currently has focus and the preset
// name field also takes keys (MainComponent.cpp:255-290). F14.
TEST (SoundcheckPanel, StopIsTheOfficialPathAndEscIsTheSecondOne)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::SoundcheckPanel panel;
    panel.setSize (900, 120);
    panel.resized();

    int stops = 0;
    panel.onStop = [&stops] { ++stops; };

    panel.setMode (gui::SoundcheckPanel::Mode::Running);
    EXPECT_TRUE (panel.getWantsKeyboardFocus());

    panel.stopButton.onClick();
    EXPECT_EQ (stops, 1);

    EXPECT_TRUE (panel.keyPressed (juce::KeyPress (juce::KeyPress::escapeKey)));
    EXPECT_EQ (stops, 2);
}

// RED IF: the three buttons carry no text (the JUCE 9 two-argument trap again),
// or ÁP DỤNG is shown while a run is still going.
TEST (SoundcheckPanel, ModesShowTheRightControlsWithRealLabels)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::SoundcheckPanel panel;
    panel.setSize (900, 120);

    EXPECT_EQ (panel.stopButton.getButtonText(),    juce::String::fromUTF8 ("DỪNG"));
    EXPECT_EQ (panel.applyButton.getButtonText(),   juce::String::fromUTF8 ("ÁP DỤNG"));
    EXPECT_EQ (panel.dismissButton.getButtonText(), juce::String::fromUTF8 ("BỎ"));

    panel.setMode (gui::SoundcheckPanel::Mode::Running);
    panel.resized();
    EXPECT_TRUE  (panel.stopButton.isVisible());
    EXPECT_FALSE (panel.applyButton.isVisible());

    panel.setMode (gui::SoundcheckPanel::Mode::Results);
    panel.resized();
    EXPECT_FALSE (panel.stopButton.isVisible());
    EXPECT_TRUE  (panel.applyButton.isVisible());
    EXPECT_TRUE  (panel.dismissButton.isVisible());

    panel.setMode (gui::SoundcheckPanel::Mode::Hidden);
    EXPECT_FALSE (panel.isVisible());
}

// RED IF: an unmeasured channel or a mis-routed one is folded into the headline
// count. They are DIFFERENT sentences for the operator: "could not measure" and
// "the routing is wrong" ask for different actions (F26).
TEST (SoundcheckPanel, UnmeasuredAndMisroutedAreSeparateSentences)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::SoundcheckPanel panel;
    panel.setSize (900, 120);
    panel.setMode (gui::SoundcheckPanel::Mode::Results);
    panel.setResultsSummary (/*hotSpots*/ 3, /*saturated*/ 1,
                             /*unmeasured*/ 2, /*routingInvalid*/ 1);
    panel.resized();

    const juce::String text = panel.summaryTextForTest();
    EXPECT_TRUE (text.contains ("3"));
    EXPECT_TRUE (text.containsIgnoreCase (juce::String::fromUTF8 ("không đo được")));
    EXPECT_TRUE (text.containsIgnoreCase (juce::String::fromUTF8 ("định tuyến")));
}
```

Append to `tests/test_spectrumview.cpp`:

```cpp
// RED IF: the overlay is fed from copySnapshot(). The publish sits inside the
// drain loop (NotchController.cpp:617-650), so with the taps suspended the
// snapshot is FROZEN for the whole run and the overlay would show the frame from
// before the run started. §4.1.
TEST (SpectrumView, SoundcheckOverlayCarriesLaneMData)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    LockFreeRingBuffer<float> tap { 8192 };
    LockFreeRingBuffer<NotchCommand> cmds { 128 };
    JuceMonotonicClock clock;
    NotchController controller { tap, cmds, clock };

    gui::SpectrumView view { controller };
    view.setSize (900, 420);
    view.resized();

    EXPECT_FALSE (view.hasSoundcheckOverlay());

    std::vector<float> margin (Detector::kNumBins, 12.0f);
    std::vector<char>  marked (Detector::kNumBins, 0);
    std::vector<char>  trusted (Detector::kNumBins, 1);
    marked[300] = 1;

    view.setSoundcheckOverlay (margin.data(),
                               reinterpret_cast<const bool*> (marked.data()),
                               reinterpret_cast<const bool*> (trusted.data()),
                               Detector::kNumBins, 48000.0);
    EXPECT_TRUE (view.hasSoundcheckOverlay());

    view.clearSoundcheckOverlay();
    EXPECT_FALSE (view.hasSoundcheckOverlay());
}

// RED IF: paint() starts allocating when the overlay is on. The whole view is
// built on "paint allocates nothing" (SpectrumView.h:10, :119-138) and the
// overlay must not be the exception.
TEST (SpectrumView, SoundcheckOverlayPaintsWithoutAllocating)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    LockFreeRingBuffer<float> tap { 8192 };
    LockFreeRingBuffer<NotchCommand> cmds { 128 };
    JuceMonotonicClock clock;
    NotchController controller { tap, cmds, clock };

    gui::SpectrumView view { controller };
    view.setSize (900, 420);
    view.resized();

    std::vector<float> margin (Detector::kNumBins, 3.0f);
    std::vector<char>  marked (Detector::kNumBins, 0);
    std::vector<char>  trusted (Detector::kNumBins, 1);
    for (int k = 100; k < 700; k += 37) marked[(std::size_t) k] = 1;

    view.setSoundcheckOverlay (margin.data(),
                               reinterpret_cast<const bool*> (marked.data()),
                               reinterpret_cast<const bool*> (trusted.data()),
                               Detector::kNumBins, 48000.0);

    juce::Image img { juce::Image::ARGB, 900, 420, true };
    { juce::Graphics g { img }; view.paint (g); }

    const auto elementsAfterFirst = view.soundcheckOverlayPathElementCountForTest();
    const auto pointCapacity      = view.spectrumPointCapacityForTest();

    for (int i = 0; i < 100; ++i) { juce::Graphics g { img }; view.paint (g); }

    EXPECT_EQ (view.soundcheckOverlayPathElementCountForTest(), elementsAfterFirst);
    EXPECT_EQ (view.spectrumPointCapacityForTest(), pointCapacity);
}
```

`spectrumPointCapacityForTest` already exists (`src/gui/SpectrumView.h:123`).
`soundcheckOverlayPathElementCountForTest` is **new**, and it is modelled on
`dashedStemPathElementCountForTest` (`src/gui/SpectrumView.h:144`), including that
accessor's honest caveat: `juce::Path` exposes no capacity getter, so the element
count walked with `Path::Iterator` is the closest available proxy for "did not
grow". Reserve the overlay path in the constructor with `preallocateSpace`, and
remember that `preallocateSpace` counts **floats, not elements** — roughly three
per `lineTo` (`memory/stereo-lane-lessons-2026-09-05.md`, and the derivation
already written at `src/gui/SpectrumView.h:176-185`).

- [ ] **Step 2: Run and watch them fail**

```bash
cmake --build build --config Release
```

- [ ] **Step 3: `gui::ModeRail`**

Declare `measureButton` **immediately after** `soundcheckButton` (`src/gui/ModeRail.h:81`) and `onMeasure` beside `onSoundcheck`. Construct it with the **single-argument** `juce::TextButton` constructor:

```cpp
    // ONE argument. juce::TextButton(name, tooltip) changed the meaning of its
    // second parameter in JUCE 9, so the two-argument form renders a button with
    // NO TEXT while the build stays green
    // (memory/juce9-api-traps-2026-08-25.md). Set a tooltip with
    // setTooltip() if one is wanted.
    juce::TextButton measureButton { juce::String::fromUTF8 ("ĐO") };
```

In `resized()`, give it its own row/column in the same flow as the other three, sized so `ĐO` is not truncated at the rail's narrowest layout. **Measure the text with the real font and the widest string the formatter can print**, not with an estimate (`memory/stereo-lane-lessons-2026-09-05.md`).

- [ ] **Step 4: `gui::SoundcheckPanel`**

A plain `juce::Component` holding the three buttons, a progress line and a summary `juce::Label`. `setMode` sets visibility and calls `setWantsKeyboardFocus (mode == Mode::Running)` plus `grabKeyboardFocus()`. `keyPressed` returns `true` and calls `onStop` for `escapeKey`, `false` for everything else — so a key the panel does not own still reaches whatever else wants it.

The progress line reads `ĐANG ĐO · kênh 2/4` plus lane M's own countdown, from `setProgress`. Do not concatenate the caption and the number into one `Label` — the number is read across a room and wants to be large, the caption only has to be findable, and one `Label` cannot be two sizes (the reasoning already written at `src/gui/ModeRail.h:66-72`).

The results strip reads `tìm thấy N điểm dễ hú`, and when `saturatedBins > 0` adds *"còn vượt X dB sau khi cắt sâu nhất — chỉnh gain, hạ trần, hoặc đổi vị trí mic"*. `unmeasured` and `routingInvalid` get their **own two sentences** — never a flat 0 dB line, which reads as "an excellent room".

Colours come from `src/gui/theme/`; **no hex literal may appear outside that directory** — `tests/test_aztheme.cpp` greps `src/gui` as text at run time and will fail otherwise (`tests/CMakeLists.txt:85-87`).

- [ ] **Step 5: `gui::SpectrumView` overlay**

Add the two setters plus private storage (`std::array<float, Detector::kNumBins>` copies, a `bool hasOverlay_`, and one reused `juce::Path`, following the existing `markerPath_` pattern at `src/gui/SpectrumView.h:435-438`). In `paint`, after the spectrum polyline and before the notch stems:

1. the margin curve, as a polyline on the same log-frequency axis;
2. a marker at every `marked[k]`;
3. a dimmed rectangle over `[kLowConfidenceAboveHz, kMaxHz]` with the label **"độ tin cậy thấp"**.

**Markers must not bury the trace** — that exact defect was found by a render during the console rebuild (`memory/ui-rebuild-sodium-rack-2026-08-25.md`). Numbers appear on hover only.

Do **not** interpolate between the amber and cyan theme colours for the margin curve: the midpoints come out mud (same memory note).

- [ ] **Step 6: The snapshot scene**

Add a `soundcheck` scene to `tools/snapshot.cpp` that builds a `MainComponent`, pushes a synthetic `OutputResult` with a handful of marked bins and one saturated candidate into the panel and the overlay, and renders. Then:

```bash
cmake --build build --config Release
```
```bash
build/tools/Release/HandsFreeSnapshot.exe shots 1440 920 --fast
```

**READ the images back** — `shots/console-soundcheck-results.png` and `shots/console-live.png` — and check, by eye:

- `ĐO` is present, legible, and not truncated;
- the results strip's Vietnamese renders (no mojibake — the middle-dot bug is the precedent);
- the margin curve is distinguishable from the spectrum trace, and the markers do not bury it;
- the 6–10 kHz band is visibly dimmed and its label is readable;
- `ÁP DỤNG` and `BỎ` both fit their buttons.

Send both images to the owner. **GUI-visible behaviour is not reported without a picture** (CLAUDE.md).

- [ ] **Step 7: Full gate and commit**

```bash
cd build && ctest -C Release
```
Expected: `100% tests passed (634)` — 625 + 9. ESTIMATE.

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add src/gui/ModeRail.h src/gui/ModeRail.cpp src/gui/SoundcheckPanel.h src/gui/SoundcheckPanel.cpp src/gui/SpectrumView.h src/gui/SpectrumView.cpp
```
```bash
git add tools/snapshot.cpp CMakeLists.txt tests/test_moderail.cpp tests/test_spectrumview.cpp tests/test_gui_wiring.cpp
```
```bash
git commit -m "feat(lane-m): DO button, progress overlay with DUNG, results strip, margin overlay on the analyser"
```

---

### Task 10: `MainComponent` wiring, the control lock, and the device-restart abort

**Mức level dự kiến:** **0 dB** by itself — but this is the task that connects a button to a loudspeaker, so after it the app really can emit. Everything in spec §3's table becomes reachable here.

**The device-restart hole this task closes** (F15). `devicePanel_.onBeforeRestart` (`src/app/MainComponent.cpp:351-358`) today only stops the `NotchController`s; it has never heard of a `SoundcheckController`. And `audioDeviceAboutToStart` clears every ring (`src/app/AudioEngine.cpp:713-738`) under the precondition "no producer and no consumer is running" (`src/dsp/LockFreeRingBuffer.h:131-141`). So:

- `onBeforeRestart` must **abort and join** the `SoundcheckController` **before** it stops the `NotchController`s;
- `micCapture_.clear()` joins the same drain block (done in Task 5);
- `Preflight` records `getCurrentSampleRateHz()`, `getNumInputChannels()`, `getNumOutputChannels()`, and every later phase re-checks them each poll and **aborts** on a change (done in Task 6). A sample-rate change makes `T`, the reference `X` and every bin→Hz mapping wrong at once, and `getLastDeviceError()` reports **nothing** in that case.

**Files:**
- Modify: `src/app/MainComponent.h` — a `SoundcheckController soundcheck_;` member (declared **after** `engine_` and **before** `notchControllers_`, so destruction runs in the reverse order and the controller is torn down while the engine still exists), plus `void applyModeGating (int)` unchanged and a new `void setSoundcheckControlsLocked (bool)`
- Modify: `src/app/MainComponent.cpp`:
  - the `ĐO` lambda, next to the existing rail lambdas (anchor: `modeRail_.onSoundcheck = [this] { requestMode (AudioEngine::Mode::Soundcheck); };`, `:235`)
  - the `onBeforeRestart` hook (anchor: `devicePanel_.onBeforeRestart = [this]`, `:351`)
  - the preset chooser lambdas (anchor: `presetLoadChooser = [] (std::function<void (const juce::File&)> onPicked)`, `:258`)
  - `loadPreset` (`:830`) — refuse while `state != Idle`
- Test: `tests/test_gui_wiring.cpp`

**Interfaces:**
- Consumes: everything from Tasks 6, 7 and 9.
- Produces: `MainComponent::getSoundcheckControllerForTest()` — the same shape as the existing `getNotchControllerForTest` (`src/app/MainComponent.h:188`).

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_gui_wiring.cpp`:

```cpp
// RED IF: onBeforeRestart forgets lane M. audioDeviceAboutToStart clears every
// ring (AudioEngine.cpp:713-738) under "no producer, no consumer running"
// (LockFreeRingBuffer.h:131-141); a live soundcheck thread draining micCapture_
// across that call violates the precondition. F15.
TEST (MainComponentSoundcheck, DeviceRestartAbortsAndJoinsTheSoundcheckFirst)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent app;

    auto& sc = app.getSoundcheckControllerForTest();
    ASSERT_NE (app.getDevicePanelForTest().onBeforeRestart, nullptr);

    app.getDevicePanelForTest().onBeforeRestart();

    EXPECT_EQ (sc.getState(), SoundcheckController::State::Idle);
}

// RED IF: PRESET LOAD stays live while proposals are pending. adoptPreset uses
// the FILE's indices and overwrites without checking n.active
// (NotchController.cpp:487-506 -> setNotchImpl :226-245), so it would silently
// erase every pending proposal. F10.
TEST (MainComponentSoundcheck, PresetLoadIsRefusedWhileASoundcheckIsPending)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent app;

    app.setSoundcheckControlsLockedForTest (true);

    juce::File tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("lane-m-lock-test.json");
    tmp.replaceWithText ("{\"version\":\"1.0\",\"notches\":[]}");

    EXPECT_FALSE (app.loadPreset (tmp)) << "a pending soundcheck must refuse a preset load";

    tmp.deleteFile();
}

// RED IF: the mode buttons and CLEAR ALL stay live during a run. A mode change
// mid-run re-arms detection under a suspended tap; CLEAR ALL mid-run removes
// notches nobody asked about.
TEST (MainComponentSoundcheck, ModeAndClearAllAreLockedWhileRunning)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent app;

    app.setSoundcheckControlsLockedForTest (true);
    auto& rail = app.getModeRailForTest();

    EXPECT_FALSE (rail.soundcheckButton.isEnabled());
    EXPECT_FALSE (rail.autoButton.isEnabled());
    EXPECT_FALSE (rail.bypassButton.isEnabled());
    EXPECT_FALSE (rail.clearAllButton.isEnabled());
    EXPECT_FALSE (rail.measureButton.isEnabled());

    app.setSoundcheckControlsLockedForTest (false);
    EXPECT_TRUE (rail.measureButton.isEnabled());
}

// RED IF: the restore-detection lambda grows past a relaxed atomic store per
// slot. It runs on the LANE M THREAD, and that is only defensible because
// NotchController::setDetectionActive is exactly one relaxed store
// (NotchController.cpp:936-939). inv 17.
TEST (MainComponentSoundcheck, RestoreDetectionCallbackOnlyTouchesAtomics)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent app;

    auto& sc = app.getSoundcheckControllerForTest();
    ASSERT_NE (sc.setDetectionActiveOnAllSlots, nullptr);

    sc.setDetectionActiveOnAllSlots (false);
    for (int i = 0; i < kMaxSlots; ++i)
        EXPECT_FALSE (app.getNotchControllerForTest (i)->detectionActiveForTest()) << "slot " << i;

    sc.setDetectionActiveOnAllSlots (true);
    for (int i = 0; i < kMaxSlots; ++i)
        EXPECT_TRUE (app.getNotchControllerForTest (i)->detectionActiveForTest()) << "slot " << i;
}

// RED IF: pressing ĐO with no device open emits anything, or shows nothing at
// all. A refusal must SAY which refusal it was. inv 19, F26.
TEST (MainComponentSoundcheck, MeasureWithNoDeviceRefusesAndSaysSo)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent app;

    ASSERT_NE (app.getModeRailForTest().onMeasure, nullptr);
    app.getModeRailForTest().onMeasure();

    EXPECT_EQ (app.getSoundcheckControllerForTest().getState(),
               SoundcheckController::State::Idle);
    EXPECT_FALSE (app.getAudioEngine().soundcheckIsEmitting());
    EXPECT_TRUE (app.lastMessageForTest().isNotEmpty());
}
```

`getDevicePanelForTest`, `getModeRailForTest`, `lastMessageForTest` and `setSoundcheckControlsLockedForTest` may not all exist. **Grep `src/app/MainComponent.h` for the accessors that DO exist before writing the test** — `getNotchControllerForTest` is at `:188` and `getAudioEngine()` is already used at `tests/test_gui_wiring.cpp:1016-1041`. Add only the accessors that are genuinely missing, each with a `// TEST ACCESSOR ONLY` comment, and use the real names for the rest. A plan that names a helper the repo does not have is the lane G M-1 defect, and it is caught by opening the file, not by reasoning.

- [ ] **Step 2: Wire it**

The `ĐO` lambda, beside the other rail lambdas:

```cpp
    modeRail_.onMeasure = [this]
    {
        // Message thread. Everything a run needs is read HERE, once, and handed
        // over frozen: a threshold that moved mid-run would score channel 1 and
        // channel 9 of one measurement on two different rulers (spec §4.3).
        NotchController::SnapshotBuffer risk {};
        notchControllers_[(std::size_t) displayedSlot_]->copySnapshot (risk);

        const auto targets = buildSoundcheckTargets();
        const auto refusal = soundcheck_.preflight (targets, risk);
        if (refusal != SoundcheckController::Refusal::None)
        {
            showMessage (soundcheckRefusalMessage (refusal));   // each refusal its OWN sentence
            return;
        }

        SoundcheckController::RunParams params;
        // A PEAKINESS RATIO, read live from the detector so lane M's gate cannot
        // drift away from the detector's own (N1). Never kConfirmScore.
        params.noiseFloorGate    = notchControllers_[0]->getPeakinessThreshold();
        params.peak              = SoundcheckSignal::kSoundcheckMaxPeak;
        params.sampleRate        = engine_.getCurrentSampleRateHz();
        params.numInputChannels  = engine_.getNumInputChannels();
        params.numOutputChannels = engine_.getNumOutputChannels();
        params.ceilingDb         = notchControllers_[0]->getNotchDepthDb();
        params.notchQ            = notchControllers_[0]->getNotchQ();

        confirmSoundcheck (targets.size(), [this, targets, params]
        {
            setSoundcheckControlsLocked (true);
            soundcheck_.arm (targets, params);
        });
    };
```

`confirmSoundcheck` shows the dialog spec §4.3 requires: **"HẠ MASTER TRƯỚC"** and the total duration — `targets.size() * 4.5 s`, printed as a real number, up to ~72 s. It is **injectable**, like the preset choosers (`src/app/MainComponent.cpp:258-290`), so a headless test can answer it without a native dialog (`memory/gui-console-lessons-2026-08-24.md`: an async confirm dialog must be injectable and hold a `SafePointer`).

`onBeforeRestart` gains **two lines, first**:

```cpp
    devicePanel_.onBeforeRestart = [this]
    {
        // Lane M FIRST (F15): audioDeviceAboutToStart clears every ring
        // (AudioEngine.cpp:713-738) under "no producer, no consumer running"
        // (LockFreeRingBuffer.h:131-141), and micCapture_ is in that block now.
        soundcheck_.abortAndJoin();
        setSoundcheckControlsLocked (false);

        // §6.5, unchanged: every detector thread joins before the rings clear.
        for (auto& controller : notchControllers_)
            controller->stop (1000);
    };
```

`setSoundcheckControlsLocked (bool)` disables `modeRail_.soundcheckButton`, `autoButton`, `bypassButton`, `clearAllButton`, `measureButton`, both preset buttons, and every slot enable/width/routing control. `loadPreset` (`src/app/MainComponent.cpp:830`) returns `false` early while locked **or while the state is `Results`**, with a message saying why.

The `ÁP DỤNG` / `BỎ` lambdas on the panel:

```cpp
    soundcheckPanel_.onApply = [this]
    {
        // MESSAGE THREAD. This is the ONLY place a preventive notch is written.
        SoundcheckApplyStats total;
        for (int slot = 0; slot < kMaxSlots; ++slot)
        {
            const auto forSlot = soundcheck_.copyResultsForSlot (slot);
            if (forSlot.empty()) continue;
            const auto s = applySoundcheckResults (*notchControllers_[(std::size_t) slot], forSlot);
            total.placed += s.placed; total.refused += s.refused;
            total.clearedPrevious += s.clearedPrevious;
        }
        logSoundcheckApply (total);
        soundcheck_.applyRequested();
        setSoundcheckControlsLocked (false);
    };
```

- [ ] **Step 3: Build, run, gate**

```bash
cmake -B build -G "Visual Studio 18 2026" -A x64
```
```bash
cmake --build build --config Release
```
```bash
cd build && ctest -C Release
```
Expected: `100% tests passed (639)` — 634 + 5. ESTIMATE.

- [ ] **Step 4: Render and read back**

```bash
build/tools/Release/HandsFreeSnapshot.exe shots 1440 920 --fast
```
Read `shots/console-live.png` and `shots/console-idle.png` back and confirm the rail still lays out with five controls instead of four, and that nothing was pushed off the bottom. Send both to the owner.

- [ ] **Step 5: Commit**

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add src/app/MainComponent.h src/app/MainComponent.cpp tests/test_gui_wiring.cpp
```
```bash
git commit -m "feat(lane-m): wire DO/APPLY/BO, lock the controls while measuring, abort+join before a device restart"
```

---

### Task 11: docs, tester notes, release notes, memory, roadmap

**Mức level dự kiến:** **0 dB** — no code. This task exists because a behaviour change nobody wrote down is a behaviour change the next session will "fix" back. Every level figure below is **copied from spec §3 and from the numbers the earlier tasks actually printed**, never invented here.

**Three claims this task must carry because no test can** (spec §5.3):

1. **What −20 dBFS sounds like in a real room** is unknown to this project. The app has no idea of absolute SPL and cannot convert without knowing the desk's and the amp's gain. The only honest sentence is the one spec §3 gives: *"the sweep plays 20 dB below the system's full scale, at your current master setting."* Rev 1 of the spec said "≈ 80 dB SPL"; that figure rested on an assumption about the desk that the app is not entitled to make, and it was removed.
2. **Whether preventive notches actually raise gain-before-feedback** is the one number that proves this lane was worth building, and only a rig can produce it: turn the master up a dB at a time until it howls, before and after `ÁP DỤNG`, and report the difference.
3. **Whether 4.5 s of silence per channel — ~72 s for a big system — fits a real soundcheck workflow.** That is a process change, not just a feature.

**Files:**
- Modify: `docs/GIOI-THIEU.md`
- Modify: `docs/KY-THUAT-CHONG-HU.md`
- Modify: `docs/superpowers/specs/2026-09-04-anti-feedback-v2-roadmap.md` — line **24** (the lane table's M row) and line **71** (the status table's M row)
- Modify: `docs/superpowers/specs/2026-09-15-active-soundcheck-design.md` — the status line
- Modify: `installer/TESTER-NOTES.md`
- Create: `docs/release-notes/1.3.0-alpha.md`
- Create: `memory/active-soundcheck-lane-m-2026-09-15.md`; modify `memory/MEMORY.md`

**Interfaces:** none — prose only.

- [ ] **Step 1: `docs/GIOI-THIEU.md`**

Add a row to the feature table describing the active soundcheck for a soundman, in the voice the file already uses. It must say: a separate **`ĐO`** button; the app plays a 3 s sweep out of **one output channel at a time**; **that channel goes completely silent for 4.5 s** while it is measured, so a 4-output system takes about 18 s and an 8-slot stereo system about **72 s**; the app **proposes** notches and places nothing until **`ÁP DỤNG`**; above 6 kHz this version **draws but never proposes**; and **turn the master down first**.

Amend the preset row to state the asymmetry (spec §4.8): a preset saved after a soundcheck **does** carry the preventive notches, but reloading brings them back as `Origin::Preset`, so they **auto-release after 30 s of quiet** like any other preset notch. To get the full preventive protection back, run `ĐO` again.

- [ ] **Step 2: `docs/KY-THUAT-CHONG-HU.md`**

A new section, `### Soundcheck đo chủ động (1.3.0, lane M)`, covering:

- the four insertion points in the callback, and **why point 3 must sit above the `±1.0f` clamp** (`src/app/AudioEngine.cpp:631-647`);
- the mute rule: **every lane routed to the measured OUTPUT CHANNEL**, not one `(slot, lane)` pair — because several slots sum onto one channel (`:565-577` clears, `:621` accumulates), so muting a pair leaves that channel's loop closed;
- the seven atomics and why they are read **once** beside `bypass`;
- why tap suspension is keyed on `scSuspendTaps_` and not on `scOutChannel_`, **with the 11.5 s number**;
- the loop-gain maths, `H_dB >= 0` ⇒ howl, `margin = -H_dB`, and the truncation caveat the test measures;
- the trusted band, and why 6–10 kHz is drawn but never proposed (the 20 dB per-bin energy difference);
- the depth rule and the six worked examples, with the note that the **ceiling is the last rung** and may be off-grid (`presets/Music.json` = −10);
- the abort set and the worst-case stop latency table from spec §3;
- the preset asymmetry from §4.8 — **this file and `GIOI-THIEU.md` must both carry it, in the same change** (Definition of done item 5).

Add to the safety-limits table:

```markdown
| Sweep vượt mức | `SoundcheckSignal::clampPeak` kẹp ở `kSoundcheckMaxPeak` = 0.1f (−20 dBFS) tại CẢ chỗ đặt lẫn chỗ dùng; kẹp cuối đường ±1.0f vẫn nằm SAU điểm tiêm |
| Thread lane M chết giữa lúc phát | Ramp-out do CHÍNH callback sinh; nó tự đặt `scOutChannel_ = -1`. Không cần thread nào khác còn sống |
| Restart thiết bị giữa lúc đo | `onBeforeRestart` abort + join lane M TRƯỚC khi stop detector; `micCapture_.clear()` trong cùng khối drain |
| Chỉ số kênh ngoài phạm vi sau restart | Kiểm lại MỖI callback với số kênh của chính callback đó |
```

And to the session-log table:

```markdown
| `soundcheck_start` / `_output` / `_result` / `_apply` / `_abort` | lane M | xem `docs/superpowers/specs/2026-09-15-active-soundcheck-design.md` §4.7. `ring_risk` là **null** khi chưa chấm được khung nào, không phải 0.0 |
```

- [ ] **Step 3: the roadmap**

`docs/superpowers/specs/2026-09-04-anti-feedback-v2-roadmap.md` **line 24** — the lane table's M row still points at a spec filename that does not exist (`2026-09-06-active-soundcheck-design.md`) and says "phiên B theo prompt 2026-09-06 chưa chạy". Replace the last column with a link to the real file:

```markdown
[`2026-09-15-active-soundcheck-design.md`](2026-09-15-active-soundcheck-design.md) — **đã hạ cánh 1.3.0**
```

**Line 71** — the status table's M row currently reads `**mở** — S đã hạ cánh; chưa có spec, chưa có nhánh`. Replace it with the branch, the commit range, the suite count the final `ctest` printed, and the nine owner confirmations with the date each was answered.

And lane A's row: M now exists, so amend "chờ M" to name what lane M does and does **not** supply — it gives loop gain per bin, and it explicitly does **not** give round-trip delay (spec §4.4, §6).

- [ ] **Step 4: the spec's own status line**

`docs/superpowers/specs/2026-09-15-active-soundcheck-design.md` line 4 currently says **"CHƯA owner duyệt"**. Update it to name the plan, the branch and the owner's decision date — and **only after Task 12 Step 0 has recorded real answers**. Until then it stays as it is: a spec that claims owner approval it does not have is worse than one that admits it.

- [ ] **Step 5: `docs/release-notes/1.3.0-alpha.md`**

Create it in the shape of `docs/release-notes/1.2.0-alpha.md`. In this order: version and date; the suite count from the final `ctest` run; what changed; and an explicit **"nghe ở âm lượng thấp trước"** block quoting spec §3's table row by row:

- on the measured output channel: 4.5 s silent, then a −20 dBFS peak / ≈ −23 dBFS RMS sweep for 3.0 s;
- on every other output channel: 0 dB;
- worst case ~72 s for 8 stereo slots;
- after `ÁP DỤNG`: −6 / −12 / −18 / −24 dB (or the preset ceiling) on up to 6 bins per lane;
- after `BỎ` or the 20 s timeout: nothing at all;
- **there is no limiter in this app** — the only thing between the sweep and the driver is a ±1.0f clamp, and the real loudness is set by the operator's master.

It must also state the two divergences from the owner's original prompt that Q6 and Q16 record: v1 **proposes** rather than places, and `ĐO` is its **own** button.

- [ ] **Step 6: `installer/TESTER-NOTES.md`**

Update the header block (version, build date, suite count, SHA-256 and size — the SHA comes from the installer the release script actually produces, so fill it in **after** Task 12). Add `## Mới trong 1.3.0` **before** `## Mới trong 1.2.0`, written for a soundman:

- **HẠ MASTER TRƯỚC.** The sweep plays at 20 dB below the system's full scale **at your current master**. If the system is running a show at normal level, the sweep is a little quieter than the programme. If the master is wide open, 20 dB below full scale is still very loud. The level can only be turned **down**, never up.
- Press `ĐO`; confirm the dialog; **the channel being measured goes completely silent for 4.5 seconds**, one channel after another. A 4-output system takes about 18 s; an 8-slot stereo system about **72 s**. Do not press it while the MC is talking.
- **`DỪNG` is the stop button.** `Esc` usually works too but is not guaranteed — it only reaches the overlay while the overlay has focus. From pressing `DỪNG` to silence is about 31 ms at a 64-sample buffer and 51 ms at 1024, plus your driver's and amp's own latency.
- **The app only proposes.** Nothing changes until you press `ÁP DỤNG`. `BỎ`, or waiting 20 seconds, throws the proposal away.
- **Above 6 kHz this version draws but does not propose.** The sweep covers 100 Hz – 10 kHz, but a constant-amplitude log sweep puts 20 dB less energy into each bin at 10 kHz than at 100 Hz, so the top of the range is not trustworthy yet. Tell us whether it is worth widening.
- **The loop through OTHER output channels is still closed while measuring.** Their loudspeakers are still carrying the mic. If the room is near the edge on one of them, the sweep can set it off — which is why the app refuses to start when `RING RISK` already reads `RISING` or worse.
- **A preset saved after a soundcheck brings the notches back as PRESET notches**, so they auto-release after 30 s of quiet. Run `ĐO` again to get the preventive protection back in full.
- **What to report**, in this order of value:
  1. Turn the master up a dB at a time until it howls, **before and after** `ÁP DỤNG`. **The difference is the only number that proves this feature works.**
  2. Compare the margin curve against a reference measurement (REW / Smaart and a measurement mic): **how many dB out, how many bins off?**
  3. Press `DỪNG` mid-sweep: how long until silence, and is there a click?
  4. Does 4.5 s of silence per channel fit your soundcheck routine? Is ~72 s too long?
  5. In a noisy room, how many channels come back "không đo được"?
  6. Do lane M's candidates match the bins the detector actually notches in the show afterwards?

- [ ] **Step 7: `memory/`**

Create `memory/active-soundcheck-lane-m-2026-09-15.md` in the shape of the other notes, recording at minimum:

- **The unit trap, twice in one project.** `peakinessAt` is an **unbounded ratio** (noise 7.35, tone 131.70) and `kConfirmScore` is the threshold of a **0..1 product**. Spec rev 2 compared them and would have aborted every run in every room; lane R had made the mirror-image mistake three weeks earlier. **Name the unit of both sides of every comparison, in a comment, at the point of comparison.**
- **A flag that returns to a sentinel mid-run cannot be a run-scoped lock.** `scOutChannel_` goes to −1 at every Gap, so keying tap suspension on it un-suspends for 300 ms per channel — ≈ 11.5 s of release clock over 16 channels, past `kReleaseStepMs`. The fix is a second flag with the right scope, not a cleverer read of the first.
- **A safety property must not be inferred from the sign of an index.** `scCaptureActive_` exists separately from `scOutChannel_` for exactly that reason.
- **Where a publish sits decides what a reader can see.** `copySnapshot()` is refreshed inside the drain loop (`NotchController.cpp:568-582`, `:617-650`), so suspending the taps freezes it — which is why the overlay draws lane M's own data and the `index` read happens only after the taps resume.
- **The sign of a derived quantity is worth a test of its own.** Rev 1's `needed = -(H_dB + 6)` produced a POSITIVE depth, which `setNotchImpl` refuses at `NotchController.cpp:214` — so the whole feature would have placed nothing, silently.
- **State a quantiser as a rule, then check every example against the rule.** Rev 2's wording ("the deepest rung not deeper than depth_raw") contradicted its own worked examples and its own tests. Six examples re-derived by hand caught it.
- **`SnapshotBuffer::linked` is the operator's switch, not the behaviour.** `effectiveLinked()` is `isLinked() || width_ < 2 || taps_[1] == nullptr`; from outside, the equivalent is `snapshot.linked || snapshot.laneCount < 2`.
- **Three review rounds found 8 / 2 / 0 blockers, and every blocker in round 2 was created by the round-1 rewrite.** Absorbing eight blockers in one pass generates new defects at a meaningful rate. A second review of the fixed document is not ceremony.
- **The invariant no test can hold.** Invariant 16 ("no lock, no allocation, no logging in the callback") is a property of the source text. It is in the reviewer checklist, and the plan says outright that nothing in the suite enforces it — better than pretending a test does.

Add one line to `memory/MEMORY.md`'s `## Notes` list, in the same style as its neighbours, linking the new file.

- [ ] **Step 8: Verify and commit**

```bash
cd build && ctest -C Release
```
Expected: `100% tests passed` — unchanged from Task 10; record the number for the release note and the tester notes.

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add docs/GIOI-THIEU.md docs/KY-THUAT-CHONG-HU.md docs/release-notes/1.3.0-alpha.md installer/TESTER-NOTES.md memory/MEMORY.md memory/active-soundcheck-lane-m-2026-09-15.md
```
```bash
git add docs/superpowers/specs/2026-09-04-anti-feedback-v2-roadmap.md docs/superpowers/specs/2026-09-15-active-soundcheck-design.md
```
```bash
git commit -m "docs(lane-m): active soundcheck -- level table, trusted band, depth rule, preset asymmetry, tester notes"
```

---

### Task 12: release 1.3.0 to the alpha testers — **BLOCKED on the nine owner confirmations**

**Mức level dự kiến:** everything in spec §3 reaches a real PA. This is the point of the gate.

- [ ] **Step 0: The gate. Do not skip it, and do not answer it yourself.**

Open `docs/superpowers/decisions/2026-09-15-lane-m-active-soundcheck.md` and check that **each of the nine items** listed at the top of this plan has an owner answer recorded as a **new** `Q<n> (lật lại <ngày>)` entry. Old entries are never edited — that is the `recording-design-decisions` rule and the decision file's own closing note says so.

If any answer is missing, **stop here and report which ones**. An agent's own judgement is not the owner's approval, and neither is a coordinator's. This is a signal going into a live PA.

If an answer comes back NO, this is the task it lands on:

| Item | If the answer is NO |
|---|---|
| Q2 (level) | Task 1: `kSoundcheckMaxPeak`, plus every level figure in Tasks 5, 9, 11 |
| Q15 (mute the whole channel) | Tasks 5 and 6 — the mute rule and the duration; and spec §7 risk 3 becomes live, since Q15 PA 3 (mute EVERY output) removes both the cross-lane and the other-channel loops |
| Q6 (propose vs place) | Tasks 7 and 9: an automatic apply is a different flow and a different set of GUI states |
| Q3 (the abort set) | Task 6 |
| Q16 (a separate `ĐO`) | Task 9 |
| Q7 + Q9 (preset asymmetry) | Task 11 at minimum; making preventive notches survive a reload would change `Origin` handling in `NotchController`, which is outside this plan's two additive changes |
| `kResultsTimeoutMs` | Task 6 |
| `ClearReason::SoundcheckReplace` | Task 4, and Task 7's replace pass loses its label |
| `Origin origin` on `SnapshotNotch` | Task 4, and Task 7's replace pass becomes **impossible** — §4.6 has no fallback |

- [ ] **Step 1: Release**

CLAUDE.md's standing instruction: a finished change ships. This one is a minor version.

```bash
pwsh -File installer\release-alpha.ps1 -Part minor
```

The script bumps the PATCH/MINOR in `CMakeLists.txt`, reconfigures, builds Release, runs `ctest` **as a gate**, packages with NSIS and copies the installer to `Z:\My Drive\RELEASE\ALPHA TEST`, then prunes the drop folder to the newest three builds. **A red suite publishes nothing and rolls the version back.** Never pass `/DPRODUCT_VERSION`: `handsfree.nsi` reads the version out of the `project(HandsFree VERSION x.y.z)` line with `!searchparse`, and a build stamped with a number the source does not carry is untraceable.

- [ ] **Step 2: Fill in the installer's SHA-256 and size, then commit the bump**

```bash
git add CMakeLists.txt installer/TESTER-NOTES.md docs/release-notes/1.3.0-alpha.md
```
```bash
git commit -m "chore: release 1.3.0 to alpha (lane M active soundcheck)"
```

The version bump **is a source change** — commit `CMakeLists.txt` or the next run bumps from the same number again.

- [ ] **Step 3: Close the phase in public (global rule 12)**

Three things, before moving on:

1. **The roadmap's lane M rows say so** (Task 11 Step 3), and every statement this lane made false is fixed. A phase still marked "mở" that shipped last week is not a stale note, it is a lie the next session will act on.
2. **Name what the owner can run, and what they should SEE**: the installer in `Z:\My Drive\RELEASE\ALPHA TEST`, the `ĐO` button, `shots/console-soundcheck-results.png`, and `python tools/logstats.py <session>.jsonl` on a log from a real run.
3. **Write the handoff for lane A now**, while the context is still here: `D:\DEV CAVE EP3\shared\handoff\handoff-<YYYYMMDD>-lane-a-afc.md`, per `D:\DEV CAVE EP3\shared\README.md`. It must state the baseline acceptance numbers, what is done vs in progress vs waiting on a human, and the known pitfalls — including, explicitly, that **lane M does NOT supply round-trip delay** (spec §4.4, §6): lane A has to measure it itself or open the deconvolution branch Q5 PA 3 describes.

---

## Người đọc thứ hai: soundcheck đo chủ động làm gì trên sân khấu (1.3.0)

Cho tới 1.2.0, app **hoàn toàn thụ động**: nó chỉ ngồi nghe tín hiệu người khác
đưa vào và trừ gain đi khi phát hiện hú. Nghĩa là **notch đầu tiên luôn tới sau
tiếng hú đầu tiên** — khán phòng nghe khoảng 0,3–0,9 giây tiếng hú trước khi
filter đủ sâu. Lane G làm phần đó *nông hơn*. Lane M là lane duy nhất có thể làm
cho nó *không xảy ra*.

**Cách nó làm.** Bấm nút `ĐO`. App chọn **từng kênh ngõ ra một**, và với mỗi
kênh:

1. **Tắt tiếng kênh đó hoàn toàn 0,5 giây** và nghe xem phòng đang ồn cỡ nào —
   đó là "nền nhiễu". Tắt **cả kênh**, không phải một đường: nhiều slot cộng dồn
   lên cùng một kênh ngõ ra, nên tắt một đường thôi thì vòng hú của kênh đó vẫn
   đóng.
2. **Phát một tiếng "vút" 3 giây** đi từ 100 Hz lên 10 kHz, ở mức 20 dB dưới
   toàn thang, vào đúng kênh ấy và không kênh nào khác.
3. **Nghe tiếp 0,7 giây** để bắt phần ngân của phòng.
4. So tín hiệu thu được với tín hiệu đã phát, **theo từng tần số**. Tỉ số đó
   chính là **độ lợi vòng**: nó nói ở tần số nào tiếng từ loa quay lại mic mạnh
   tới mức nào.

Một tần số có độ lợi vòng **bằng hoặc hơn 0 dB là một tần số sẽ hú**. Còn thiếu
bao nhiêu dB nữa mới hú thì gọi là **margin**. App vẽ đường margin lên analyser,
đánh dấu những chỗ sát nhất, và **đề xuất** notch ở đó — sâu −6, −12, −18 hoặc
−24 dB, đúng thang lane G đang dùng, và không bao giờ sâu hơn trần của preset.

**App không tự đặt gì.** Đề xuất nằm đó chờ; bấm `ÁP DỤNG` thì mới có filter
thật, bấm `BỎ` hoặc để yên 20 giây thì bỏ hết. Đây là chỗ lệch với chỉ thị ban
đầu của chủ dự án (prompt viết "đặt"), và là một trong chín mục chủ dự án phải
chốt.

**Phải biết trước ba điều, vì chúng đổi cách làm việc chứ không chỉ thêm tính
năng:**

- **Kênh đang đo im 4,5 giây.** Bấm `ĐO` giữa lúc MC đang nói thì kênh đó mất
  tiếng 4,5 giây. Hệ 8 slot stereo = 16 kênh ⇒ **khoảng 72 giây**. Hộp thoại xác
  nhận in đúng con số ấy trước khi bạn bấm.
- **Vẫn có thể hú trong lúc đo — chỉ là không qua kênh đang đo.** Vòng hú đi qua
  kênh *đang đo* bị **mở** (kênh đó không mang tiếng mic nào). Vòng đi qua các
  kênh *khác* vẫn **đóng**: loa của chúng vẫn phát tiếng mic. Nếu phòng đang sát
  ngưỡng ở một kênh khác, lần quét **có thể** kích nó. Vì thế có nút `DỪNG`, có
  bộ tự hủy, và app **từ chối chạy** khi chip `RING RISK` đã ở `RISING` trở lên.
- **Hạ master trước.** App không biết SPL tuyệt đối và không có phép quy đổi nào
  đúng nếu không biết gain của bàn và amp. Câu trung thực duy nhất là: sweep phát
  ở **20 dB dưới toàn thang của hệ, tại vị trí master hiện tại của bạn**. Master
  mở hết thì 20 dB dưới toàn thang **vẫn rất to**. Mức phát chỉ chỉnh được
  **xuống**.

**Trên 6 kHz bản này chỉ vẽ, không đề xuất.** Tiếng "vút" chạy đều theo octave,
nên năng lượng nó gửi vào mỗi vạch tần số ở 10 kHz thấp hơn ở 100 Hz đúng 20 dB —
chưa đủ tin để đặt notch. Dải 6–10 kHz vẫn được vẽ, có nhãn "độ tin cậy thấp".
Người đo trên dàn thật cho biết có đáng nới không.

**Preset lưu sau khi đo thì hơi khác một chút.** File **có** mang notch phòng
ngừa, nhưng khi nạp lại chúng vào model như **notch preset bình thường**, nên
**sẽ tự nhả sau 30 giây yên tĩnh**. Muốn bảo vệ phòng ngừa quay lại đầy đủ thì
chạy lại `ĐO`.

---

## Self-review

Run against spec rev 4 with fresh eyes after the plan was written, in the four
subsections lane G's plan uses. Spec §8's round-4 note asks for exactly this and
one thing more — *"the next round should not be another read of the spec: it
should be someone checking the PLAN against the real code before dispatch"*
(lane G lesson 16: a plan is not verified until someone OPENS the files it cites;
three helpers lane G's plan relied on existed nowhere in the repo). Section 3
below lists every name this plan uses, with where it was read.

### 1. Spec coverage

| Spec § | Requirement | Task |
|---|---|---|
| §3 | the level table, stated per task | every audio-path task's `Mức level dự kiến` line; §3's rows are quoted into 5, 6, 7, 11, 12 |
| §3 | the safety-mechanism table (clamp at source and at use, tail clamp, ramps, `DỪNG`, self-abort, refusals, detection off, bounds check, safe-dead defaults) | 1 (clamps, ramps), 5 (tail clamp, bounds check, safe defaults), 6 (aborts, refusals, detection off), 9 (`DỪNG`) |
| §3 | the worst-case stop-latency table | 5 (`AbortRampsDownInTheCallbackAlone`), 11 (tester notes), 12 (owner gate item 4) |
| §4.1 | the seven atomics, `micCapture_`, `micCaptureDrops_`, `kCaptureCapacity` | 5 |
| §4.1 | snapshot-once beside `bypass` (inv 7) | 5 (`AtomicsAreSnapshottedOnce`) |
| §4.1 | per-callback bounds check (inv 3) | 5 (`OutOfRangeChannelIsIgnored`) |
| §4.1 | the four insertion points in order | 5 |
| §4.1 | tap suspension keyed on `scSuspendTaps_`, held across Gaps, ≤ ~420 ms drift (inv 10, 10b) | 5 (`TapsStaySuspendedAcrossTheGap`) |
| §4.1 | the snapshot is frozen during a run ⇒ the overlay uses lane M's own data; `index` is read only after the taps resume | 7 (re-read before each `setNotch`), 9 (`SoundcheckOverlayCarriesLaneMData`) |
| §4.2 | `SoundcheckSignal`: closed-form log sweep, `n < 0` ⇒ 0, raised-cosine window, separate `rampOut` | 1 |
| §4.3 | the state machine, phase by phase | 6 |
| §4.3 | `RunParams`, read once at `Arm`, immutable | 6 (`NoiseFloorGateIsReadAtArm`, `NoiseFloorGateIsStableWithinARun`) |
| §4.3 | `Results` runs with detection restored; `kResultsTimeoutMs = 20 s` | 6 (`DetectionIsRestoredBeforeResults`, `ResultsTimeoutIsTwentySeconds`) |
| §4.3 | ring risk read at `Preflight` and `Arm` only; the RISING identity; `ringRiskValid == false` does not refuse | 6 (`RefusesWhenRingRiskIsRising`, three branches) |
| §4.3 | the noise-floor gate against the LIVE peakiness threshold | 6 (three tests: quiet noise does not abort, a tone does, the gate follows the threshold) |
| §4.3 | mic RMS self-abort with a hold | 6 (`HotMicAbortsOnlyAfterTheHold`) |
| §4.3 | device restart: abort + join before the `NotchController`s; `micCapture_.clear()`; record and re-check rate + channel counts | 5 (`DeviceRestartDrainsTheCaptureRing`), 6 (`SampleRateChangeAborts`, `ChannelCountChangeAborts`), 10 (`DeviceRestartAbortsAndJoinsTheSoundcheckFirst`) |
| §4.3 | `Preflight` validates the specific channel pair (F26) | 6 (`RefusesOnInvalidChannelPair`) |
| §4.4 | the four formulas, the no-delay-compensation argument and its boundary | 2 (`FlatRoomMeasuresFlatResponse`, `DelayDoesNotChangeTheAnswer` at 5 / 900 ms, `DecayLongerThanTheTailIsUnderRead`) |
| §4.4 | trusted band narrower than swept band, 6–10 kHz drawn not proposed | 2 (`AboveTrustedHighHzIsDrawnButNeverTrusted`), 3 (`UntrustedBinIsNeverACandidate`), 9 (the dimmed band), 11 (tester notes) |
| §4.4 | the seven pick steps | 3 |
| §4.4 | the depth rule and all six worked examples | 3 (seven depth tests, each an exact number) |
| §4.4 | saturation from BOTH causes, with `residualDb` | 3 (`SaturatesAtMinusTwentyFourAndReportsResidual`, `SaturationByTheCeilingIsAlsoReported`) |
| §4.4 | the mark/propose split | 3 (`MarkedButNotProposedBelowMinUsefulCut`) |
| §4.5 | `OutputResult`, including `measured` vs `routingInvalid` | 6 (`UnmeasurableChannelIsAValidResultNotAFlatLine`), 9 (two separate sentences) |
| §4.6 | `Origin origin` on `SnapshotNotch` | 4 (`SnapshotCarriesOrigin`) |
| §4.6a | re-read before each `setNotch`, top-down allocation, the residual race acknowledged | 7 (`ApplyReReadsTheSnapshotBeforeEachSetNotch`) |
| §4.6b | clear the previous run with `SoundcheckReplace` | 4 (the enumerator), 7 (`ARerunReplacesItsOwnPreviousProposals`, `ReplacingLeavesEveryOtherOriginAlone`) |
| §4.6c | LINKED: derived from `laneCount`, one index both lanes, all-or-nothing; INDEP stops without rollback | 7 (four tests) |
| §4.6d | a preventive notch is its own ceiling | 7 (`APreventiveNotchIsItsOwnCeiling`) |
| §4.6e | apply on the message thread; the gate handed in via `RunParams` | 7 (`ApplyRunsOnTheMessageThread`), 6 (`RunParams`), 10 (`RestoreDetectionCallbackOnlyTouchesAtomics`) |
| §4.6f | room memory untouched | 6 (`RoomMemoryIsUntouched`) |
| §4.7 | five events, `ev` as the dispatch key, 3 s.f. rounding, `ring_risk: null` | 8 |
| §4.7 | logstats needs NO branch for the five names; the real work is the clear reason | 4 (the reason branch + fixture), 8 (Step 3 proves the totals do not move) |
| §4.8 | the preset asymmetry, in BOTH docs, in the same change | 11 (Steps 1 and 2) |
| §4.9 | `ĐO` as its own button (Q16) | 9 |
| §4.9 | lane M's own countdown, not `getSoundcheckRemainingMs` | 9 |
| §4.9 | overlay, `DỪNG`, focus, `Esc` best-effort | 9 |
| §4.9 | controls locked, `PRESET LOAD` locked in `Results` too | 10 |
| §4.9 | margin curve + markers + dimmed band; two sentences for unmeasured vs mis-routed | 9 |
| §4.9 | a rendered screenshot, read back | 9 Step 6, 10 Step 4 |
| §4.10 | all 22 constants, and `noiseFloorGate` kept OUT of the constant table | Global Constraints table; 1, 2, 3, 6 define them; 6 aliases them |
| §4.11 inv 1–20 | see the invariant list in Global Constraints | 1 (1, 2, 9), 5 (3, 4, 5, 6, 7, 8, 10, 10b), 6 (11, 12, 18, 19, 20), 7 (13, 14, 15, 17), 3 (14, 15) |
| §4.11 inv 16 | **review-enforced, not test-enforced** | 5 Step 10, and the reviewer brief of every `AudioEngine` task |
| §5.1 | every named test | 1–10, listed in the task list below |
| §5.2 | the reviewer's one-line checklist | 5 Step 10 |
| §5.3 | the rig-only list | 11 Step 6 (tester notes), in the spec's own priority order |
| §6 | the two "CHƯA" items from other lanes | 4 |
| §7 | the eleven risks | risks 1, 2, 3, 7, 8 are stated in the tasks that carry them; 4, 5, 6, 9, 10, 11 are in Task 11's docs and tester notes |
| §8 | the deferred item (pre-emphasis) | 11 Step 6, as a question for the first rig measurement — not implemented |

### 2. Spec requirements I could NOT map to a task

Stated explicitly, as the skill requires.

1. **Invariant 16 has no test and cannot have one.** "No lock, no allocation, no logging in the callback" is a property of the source text, not observable behaviour. Spec §4.11 says so outright and §5.2 makes it a reviewer line. Task 5 Step 10 is that read, and it is required in the reviewer brief of every task that touches `AudioEngine`. **Nothing in the suite fails if it is skipped** — say so rather than adding a test that pretends to cover it.

2. **The residual `index` race is narrowed, not closed** (§4.6a, §7 risk 8). Between the snapshot read and the `setNotch` there is a microsecond window in which the detector can take the slot and be silently overwritten (`src/app/NotchController.cpp:226-245` never checks `n.active`). The detector's cadence is ≥ 300 ms so the probability is tiny, but it is not zero, and **no test in this plan can produce it deterministically**. Closing it needs a second write API, which Q7 forbids. Documented, not faked.

3. **"Is the routing physically right?" is not checkable** (§7 risk 1). `Preflight` catches an **invalid** pair; it cannot catch a **valid pair wired to the wrong loudspeaker**. The SNR gate hides part of it and does not hide "the other loudspeaker is also loud enough". Rig only.

4. **Cross-lane loops are not measured** (§2, §7 risk 2). Out → L, mic → R is a real loop and v1 is blind to it. No test; stated in the docs.

5. **Whether preventive notches raise gain-before-feedback** (§5.3 item 3) is the headline claim of the lane and **only a rig can produce the number**. Task 11's tester notes put it first in the report list. No synthetic test can stand in: the synthetic room in Task 2 would be testing the model, not the loop.

6. **The `~420 ms` and `~0.72 s / 11.5 s` figures are derived, not measured.** They come from `kTapSilenceTimeoutMs = 250 ms` (`src/app/NotchController.h:82`) plus `kTapCapacity = 8192` ≈ 170 ms at 48 kHz (`src/app/AudioEngine.h:275`). `TapsStaySuspendedAcrossTheGap` pins the **behaviour** (no tap during a Gap); the **milliseconds** are arithmetic in a comment. If a reviewer wants the number itself pinned, that is a `liveMsForTest()` delta test across a full simulated run — not in this plan, flagged rather than faked.

7. **`kSoundcheckMinPeak = 0.01f` is declared and never read.** Spec §4.10 lists it (Q2: "the level can only be adjusted DOWN"), but v1 exposes no level control at all, so nothing clamps against it. It is documentation in constant form. A reviewer may reasonably ask for it to be deleted; the plan keeps it because §4.10's table is the reference the release note quotes. This is precisely lane G's `kDepthStepDb` situation.

8. **`NotchController::setSampleRate` still has no production caller.** Carried over from lane R and lane G unresolved; lane M does not touch it and does not fix it.

9. **§4.10's literal wording, "one place, `SoundcheckController.h`", is not followed literally.** The constants are defined on the class that computes with them and **aliased** in `SoundcheckController.h`, because `src/dsp/` may not include `src/app/` and `AudioEngine.cpp` cannot include `app/SoundcheckController.h`. One definition, no second literal, one file to look them up in — but a reviewer comparing the plan against §4.10 word for word will see a difference, so it is declared in Global Constraints and Task 4 Step 6 greps for violations.

10. **The restore-detection callback is the one place this plan INTERPRETS the spec.** Invariant 17 says `SoundcheckController` never calls `NotchController`; invariant 12 says detection is restored *before* `Results`. A message-thread hop satisfies 17 and breaks 12's ordering. The plan's answer — an injected `std::function` whose body in `MainComponent` is one relaxed atomic store per slot (`src/app/NotchController.cpp:936-939`) — keeps inv 17's letter (no pointer is held) and inv 12's ordering. **This is the first thing a reviewer should challenge.** Task 10's `RestoreDetectionCallbackOnlyTouchesAtomics` pins the lambda's whole observable effect.

11. **"The logstats branch"** (§4.6b, F21) is real but thin. `reasonName` must gain a case — that is mandatory and tested. On the Python side, a reason is already echoed verbatim into the `cleared by` column (`tools/logstats.py:136`), so nothing *breaks* without a change; the plan adds a `soundcheck_replaced` tally and `--expect-soundcheck-replaced` so a **fixture can pin that the name survives the whole C++ → JSON → reader path**. That is a choice, not a spec requirement, and it is declared here.

### 3. API names used

**VERIFIED to exist** — read in this worktree at `2c84075` on 2026-09-15, at these lines:

| Name | Where |
|---|---|
| `AudioEngine::audioDeviceIOCallbackWithContext` | `src/app/AudioEngine.h:240-245`, `.cpp:458` |
| `AudioEngine::audioDeviceAboutToStart`, the tap/command drain block | `src/app/AudioEngine.h:247`, `.cpp:686`, drain at `.cpp:729-738` |
| `kMaxOutputLevel = 1.0f`, applied | `src/app/AudioEngine.cpp:15`, `:631-647` (the `jlimit` at `:643`) |
| the `bypass` snapshot line and its "snapshotted ONCE" comment | `src/app/AudioEngine.cpp:509-514` |
| `struct LaneRef`, `lanes[]`, `tapSource[][]` | `src/app/AudioEngine.cpp:516`, `:520`, `:526`, assigned `:561` |
| the lane-loop bounds check | `src/app/AudioEngine.cpp:546-548`, `:555-556` |
| output clearing before accumulation; `out[n] +=` | `src/app/AudioEngine.cpp:565-577` (clear at `:575`), `:621` |
| the tap-write loop and `tapDropCounts_` | `src/app/AudioEngine.cpp:657-683` |
| `AudioEngine::kTapCapacity = 8192`, `kMaxSlots`, `kMaxSlotLanes` | `src/app/AudioEngine.h:275`; `src/app/SlotConfig.h` |
| `AudioEngine::getTapBuffer`, `getTapDropCount`, `getCommandQueue`, `getSlotConfig`, `setSlotConfig`, `isRunning`, `getCurrentSampleRateHz`, `getNumInputChannels`, `getNumOutputChannels`, `getLastDeviceError`, `setMode` | `src/app/AudioEngine.h:181-183`, `:194-196`, `:211-212`, `:220-221`, `:75`, `:142`, `:148-149`, `:155`, `:158` |
| `LockFreeRingBuffer::write / read / clear / getCapacity / getAvailableRead`, and `clear()`'s "no producer, no consumer" precondition | `src/dsp/LockFreeRingBuffer.h:54`, `:85`, `:145`, `:151`, `:117`, precondition at `:131-141` |
| `Detector::kFftSize / kHopSize / kNumBins / kFftOrder` | `src/dsp/Detector.h:66-69` |
| `PeakinessAnalyzer::peakinessAt`, `kDefaultThreshold = 10.0f`, the rig figures 7.35 / 131.70 | `src/dsp/PeakinessAnalyzer.h:166`, `:124`, `:60-65` |
| `CandidateScorer::kConfirmScore = 0.7f` | `src/dsp/CandidateScorer.h:49` |
| `ClockSource`, `JuceMonotonicClock` | `src/dsp/ClockSource.h:8-13`, `:17-21` |
| `NotchController::Origin { Detector, Preset, Manual, Soundcheck }` | `src/app/NotchController.h:62` |
| `NotchController::ClearReason` (six values today) | `src/app/NotchController.h:67-70` |
| `kSoundcheckDurationMs`, `kDepthLadderDb`, `kMaxDepthDb`, `kRiskFreezeFraction` | `src/app/NotchController.h:91`, `:109`, `:114`, `:140` |
| `effectiveLinked()`, `analysedLanes()` | `src/app/NotchController.h:217`, `:655` |
| `setNotch`, `clearNotch`, and their "message thread" declaration | `src/app/NotchController.h:221`, `:224`, comment at `:219-220` |
| `failNextSetNotchOnLaneForTest` | `src/app/NotchController.h:298` |
| `depthDbForTest` `:303`, `deepestDbForTest` `:304`, `activeForTest` `:310`, `ceilingDbForTest` `:324`, `rawCeilingDbForTest` `:328`, `detectionActiveForTest` `:388` | `src/app/NotchController.h` |
| `NotchController::runOnce`, `kSlots = 16` | `src/app/NotchController.h:350`, `:73` |
| `setDetectionActive` (one relaxed store) | `src/app/NotchController.h:340`, `.cpp:936-939` |
| `getSoundcheckRemainingMs`, `startSoundcheck` | `src/app/NotchController.h:346`, `.cpp:1005-1011`, `:1016` |
| `getNotchQ`, `getNotchDepthDb`, `getPeakinessThreshold` | `src/app/NotchController.h:362-365` |
| `SnapshotNotch` (6 fields today) and `SnapshotBuffer` (`laneCount`, `linked`, `ringRiskScore/Valid/Threshold`, `releaseFrozen`) | `src/app/NotchController.h:394-406`, `:408-445` (`laneCount` `:412`, `linked` `:413`, `ringRiskThreshold` `:436`) |
| `copySnapshot` | `src/app/NotchController.h:447`, `.cpp:1547` |
| `setNotchImpl` and its five predicates, incl. `if (! (depth <= 0.0)) return false;` | `src/app/NotchController.cpp:195-256`, the depth refusal at `:214`, the ceiling ternary at `:243-245` |
| `pushClearLocked` (lowers only `active`) | `src/app/NotchController.cpp:270` |
| `adoptPreset`'s `PartialApplyUnwind` | `src/app/NotchController.cpp:528-530` |
| the snapshot fill site (the aggregate initialiser) | `src/app/NotchController.cpp:568-582`, the init at `:578-580`, `lastDataMs_` at `:572` |
| `latest_.laneCount` / `latest_.linked` and the "operator's own switch" comment | `src/app/NotchController.cpp:636`, `:637-640` |
| `latest_.ringRiskThreshold = CandidateScorer::kConfirmScore` | `src/app/NotchController.cpp:648` |
| `tapAlive = (nowPolled - lastDataMs_) < kTapSilenceTimeoutMs` | `src/app/NotchController.cpp:662`; the constant at `NotchController.h:82` |
| the auto-release Soundcheck exemption | `src/app/NotchController.cpp:729`, `:1322` |
| `firstFreeIndexLocked` (private, bottom-up) and `firstFreeIndexAllLanesLocked` | `src/app/NotchController.cpp:1064`, `:1066-1068`, `:1072-1083`, used `:1105` |
| the room-memory gate excluding Soundcheck | `src/app/NotchController.cpp:1116` |
| `placeConfirmed`'s `PartialApplyUnwind` | `src/app/NotchController.cpp:1264` |
| `SessionLogger::makeEvent` | `src/app/SessionLogger.h:50`, `.cpp:25` |
| `MainComponent::notchEventToVar` and the `ev` ternary | `src/app/MainComponent.cpp:578-588` |
| `originName` / `reasonName` / `retuneReasonName` (free functions, anonymous namespace) | `src/app/MainComponent.cpp:54`, `:66`, `:84` |
| `MainComponent::applyModeGating`, `startSoundcheck` call, `loadPreset`, `savePreset` | `src/app/MainComponent.cpp:783`, `:802`, `:830`, `:1038` |
| `devicePanel_.onBeforeRestart` / `onAfterRestart` | `src/app/MainComponent.cpp:351`, `:358` |
| `modeRail_.onClearAllConfirmed` and `modeRail_.getSoundcheckRemainingMs` lambdas | `src/app/MainComponent.cpp:241`, `:247` |
| the two preset chooser lambdas | `src/app/MainComponent.cpp:258`, `:275` |
| `engine_.setSlotConfig` inside `changeSlotConfig` | `src/app/MainComponent.cpp:814` |
| `modeBar_` is never made visible | `src/app/MainComponent.cpp:211` (comment: "statusBar_ / modeBar_ stay alive but hidden") |
| `MainComponent::getNotchControllerForTest` | `src/app/MainComponent.h:188` |
| `gui::ModeRail::soundcheckButton { "SOUNDCHECK" }` `:81`, `autoButton` `:82`, `bypassButton` `:83`, `clearAllButton` `:84`, `countdownLabel` `:85`, `onSoundcheck` `:45`, `setDisplayedMode` `:64`, `updateCountdown` `:74` | `src/gui/ModeRail.h` |
| `gui::ModeBar::soundcheckButton { "Run Soundcheck (15s)" }` | `src/gui/ModeBar.h:43` |
| `gui::SpectrumView::setDisplayLane` `:202`, `setController` `:113`, `refreshFromSnapshot` `:88`, `kMinHz/kMaxHz` `:64-65`, `kRingRiskRisingFraction = NotchController::kRiskFreezeFraction` `:246`, `riskForScore` `:265`, `tickForTest` `:339`, `paint` `:341` | `src/gui/SpectrumView.h` |
| `spectrumPointSizeForTest` `:122`, `spectrumPointCapacityForTest` `:123`, `dashedStemPathElementCountForTest` `:144`, `kDashedStemReserveFloats` `:189`, `snapshotNotchCountForTest` `:191`; `markerPath_` `:439` | `src/gui/SpectrumView.h` — the shape the new overlay accessor copies |
| `presets/Music.json` ships `"depth": -10.0` | `presets/Music.json:8` |
| `tools/logstats.py`: `load` with `encoding="utf-8"` `:20`, the `if/elif` chain with no `else` `:45-83` (the "an unknown ev falls through" comment at `:66-71`), the reason column `:132`, the `--expect-*` flags `:153-157` | `tools/logstats.py` |
| `logstats_fixture` and its argument list | `tests/CMakeLists.txt:113-117`, inside `if(Python3_Interpreter_FOUND)` at `:112` |
| `HANDSFREE_CORE_SOURCES` (absolute paths, shared by app / tests / snapshot) | `CMakeLists.txt:84` |
| `gtest_discover_tests(HandsFreeTests)` and the 23-file source list | `tests/CMakeLists.txt:107`, `:20-42` |
| `project(HandsFree VERSION 1.2.0)` | `CMakeLists.txt:3` |
| test fixtures: `FakeClock`, `Harness`, `SlotHarness`, `StereoHarness`, `SineSource`, `NoiseSource`, `pump`, `pumpQuietFor`, `firstActiveIndex`, `RampSineSource`, `pumpStereo`, `Recorder`, `kTestSr`, `kBlockMs`, `kWarmupBlocks` | `tests/test_notchcontroller.cpp:12`, `:20`, `:28`, `:39`, `:316`, `:335`, `:350`, `:359`, `:370`, `:468`, `:517`, `:1471`; constants `:311-314` |
| test fixture: `CallbackDriver` | `tests/test_audioengine.cpp:36-61`, anonymous namespace closing at `:62` |
| `HandsFreeSnapshot` target | `tools/CMakeLists.txt:6-11` |

**INTRODUCED by this plan** (nothing above defines them today):

`SoundcheckSignal` and every member (`kSweepLowHz`, `kSweepHighHz`, `kSweepSeconds`, `kRampMs`, `kRampOutMs`, `kSoundcheckMaxPeak`, `kSoundcheckMinPeak`, `clampPeak`, `Params`, `sampleAt`, `instantaneousHz`, `totalSamples`, `rampSamples`, `clampedPeak`, `rampOut`, `rampOutSamples`); `LoopGainEstimator` and every member (`kNumBins`, `kTrustedHighHz`, `kMinBandSnrDb`, `kMinBinSnrDb`, `reset`, `pushNoiseFloor`, `pushReference`, `pushCapture`, `Result`, `finish`, `binToHz`, `hzToBin`, `Stream`, `pushInto`, `analyseFrame`); `SoundcheckCandidates` and every member (`kCandidateMarginDb`, `kMinUsefulCutDb`, `kMinProminenceDb`, `kTargetMarginDb`, `kMaxPreventivePerLane`, `Ladder`, `Depth`, `depthFor`, `Input`, `Candidate`, `Output`, `pick`, `smooth`); `SoundcheckController` and every member (`kTailSeconds`, `kGapMs`, `kNoiseFloorMs`, `kMicAbortDbfs`, `kMicAbortHoldMs`, `kResultsTimeoutMs`, `kPollMs`, the fifteen aliases, `State`, `Refusal`, `AbortReason`, `Target`, `RunParams`, `OutputResult`, `preflight`, `arm`, `applyRequested`, `dismissRequested`, `requestStop`, `abortAndJoin`, `getState`, `getCurrentTargetIndex`, `getTargetCount`, `getElapsedMsInRun`, `getRemainingMsInRun`, `copyResults`, `copyResultsForSlot`, `setDetectionActiveOnAllSlots`, `logEvent`, `onStateChanged`, `runOnce`, `start`, `stop`, `enterTarget`, `enterGap`, `finishRun`, `beginAbort`, `checkDeviceUnchanged`, `checkMicNotHot`, `noiseWindowIsRinging`, `drainCapture`, `analyseCurrentTarget`, `noiseMagnitudes_`, `roundToThreeSignificantFiguresForTest`); the free `applySoundcheckResults`, `SoundcheckApplyStats`, and the file-local `firstFreeIndexTopDown`, `round3sf`, `abortReasonName`; on `AudioEngine`: `scOutChannel_`, `scSuspendTaps_`, `scCaptureInChannel_`, `scCaptureActive_`, `scSampleIndex_`, `scPeak_`, `scRampOutAtSample_`, `kCaptureCapacity`, `micCapture_`, `micCaptureDrops_`, `setSoundcheckOutputChannel`, `setSoundcheckCaptureChannel`, `setSoundcheckCaptureActive`, `setSoundcheckTapsSuspended`, `setSoundcheckSampleIndex`, `setSoundcheckPeak`, `requestSoundcheckRampOut`, `getSoundcheckOutputChannel`, `getSoundcheckSampleIndex`, `soundcheckIsEmitting`, `getMicCaptureBuffer`, `getMicCaptureDropCount`, `setSoundcheckPeakUnclampedForTest`; on `NotchController`: `ClearReason::SoundcheckReplace` and `SnapshotNotch::origin`; `gui::ModeRail::measureButton`, `onMeasure`, `setMeasureEnabled`; `gui::SoundcheckPanel` entirely; `gui::SpectrumView::setSoundcheckOverlay`, `clearSoundcheckOverlay`, `hasSoundcheckOverlay`, `kLowConfidenceAboveHz`, `soundcheckOverlayPathElementCountForTest`; on `MainComponent`: `soundcheck_`, `soundcheckPanel_`, `setSoundcheckControlsLocked`, `buildSoundcheckTargets`, `soundcheckRefusalMessage`, `confirmSoundcheck`, `logSoundcheckApply`, `getSoundcheckControllerForTest`; test helpers `MultiDriver`, `routeMono`, `Rig`, `NotchRig`, `oneCandidate`, `whiteNoise`, `noisePlusTone`, `evOf`, `sawEvent`, `renderSweep`, `flatRoom`, `resonantRoom`, `runRoom`, `meanMidBandDb`, `Field`, `shippedLadder`, `measuredHzAround`, `defaultParams`; `logstats.py`'s `soundcheck_replaced` and `--expect-soundcheck-replaced`.

**NAMES THIS PLAN USES BUT COULD NOT CONFIRM, and which every task using them must grep for first.** Lane G's M-1 is the precedent: three helpers its plan relied on (`TempDir`, `pumpOneBlockThroughSlotZero`, `notchControllerForTest`) existed nowhere in the repo, and the v1 self-review flagged them without anyone acting on it. **Acting on it here means opening the file before writing the test:**

- `MainComponent::notchEventToVarForTest` and a `makeClearEvent` helper in `tests/test_sessionlogger.cpp` — introduced by lane G Task 9, cited by that plan's self-review, but **not re-read in this worktree**. Task 4 Step 1 says to grep and use whatever is actually there.
- `gui::SpectrumView::soundcheckOverlayPathElementCountForTest` — **new**, modelled on `dashedStemPathElementCountForTest` (`src/gui/SpectrumView.h:144`). The existing `spectrumPointCapacityForTest` (`:123`) was read and is used as-is. Note the caveat that accessor's own comment makes: `juce::Path` has no public capacity getter, so an element count is a proxy for "did not grow", not a proof.
- `MainComponent::getDevicePanelForTest`, `getModeRailForTest`, `lastMessageForTest`, `setSoundcheckControlsLockedForTest` — **these probably do not all exist.** Task 10 Step 1 says to grep `src/app/MainComponent.h` and add only the genuinely missing ones, each marked `// TEST ACCESSOR ONLY`.
- `SlotConfig`'s field names (`enabled`, `width`, `inputChannels[]`, `outputChannels[]`) are used as they appear in `src/app/AudioEngine.cpp:530-544` and `src/app/MainComponent.cpp:817-827`, but `src/app/SlotConfig.h` **was not opened**. Task 5 Step 1's `routeMono` helper is the first user; check the header there.

### 4. Type consistency

- **`Origin` and `ClearReason` spellings.** `Origin::Soundcheck` (`src/app/NotchController.h:62`) and `ClearReason::SoundcheckReplace` / `::PartialApplyUnwind` are used with those exact spellings in Tasks 4, 7 and 8. The JSON strings are `"soundcheck"`, `"soundcheck_replace"`, `"partial_apply_unwind"` — lower snake case, mapped in exactly one place each (`originName`, `reasonName`).
- **`depthDb` vs `depthDB`.** The repo already mixes them, by history: `NotchInfo::depthDB`, `ModelNotch::depthDB`, `NotchCommand::depthDB`, `SnapshotNotch::depthDB`, `PresetNotch::depthDB` keep the capital-B form; the accessor style is `depthDbForTest` / `getNotchDepthDb`. This plan follows the **accessor** style for everything it introduces: `SoundcheckCandidates::Depth::depthDb`, `Candidate::depthDb`, `OutputResult::Candidate::depthDb`, `kMaxDepthDb`, `kMicAbortDbfs`. It never introduces a `depthDB`.
- **`marginDb` is `-hDb`, everywhere.** `LoopGainEstimator::Result` carries `hDb` (loop gain); `SoundcheckCandidates::Candidate` and `OutputResult` carry `marginDb`. The sign flip happens in exactly one place, `SoundcheckCandidates::pick`, and `MarginIsTheNegativeOfLoopGain` pins it. No function takes "margin" and computes as if it were "gain".
- **`std::max` for every ceiling clamp, in both directions**, with one meaning: pick the SHALLOWER, because deeper is more negative. Same convention lane G uses at every clamp site.
- **`kNumBins` is one number.** `Detector::kNumBins` (1025) is aliased by `LoopGainEstimator::kNumBins` and again by `SoundcheckCandidates::kNumBins`. No task declares a bin count of its own.
- **`std::int64_t` for every sample index** — `scSampleIndex_`, `scRampOutAtSample_`, `sampleAt`, `rampOut`, `totalSamples`, `rampSamples`, `rampOutSamples`. Never `int` (a 3 s sweep at 192 kHz is 576 000 samples, which fits, but the ramp anchor accumulates for the whole run and does not).
- **`float` on the wire, `double` in the maths.** The engine's atomics and `OutputResult` are `float` (they cross threads and go into `juce::var`); `LoopGainEstimator`'s accumulators and `SoundcheckCandidates::depthFor` are `double`.
- **The constants are defined once and aliased**, never re-declared: `SoundcheckSignal::k*`, `LoopGainEstimator::k*`, `SoundcheckCandidates::k*` are the definitions; `SoundcheckController::k*` are aliases. Task 4 Step 6 greps for a second literal.
- **`runOnce()`** means the same thing on both controllers: one synchronous step of the loop the thread would otherwise run, callable from a test with no thread started. `NotchController::runOnce` (`src/app/NotchController.h:349`) is the precedent.
- **Test naming.** New suites are `SoundcheckSignal`, `LoopGainEstimator`, `SoundcheckCandidates`, `SoundcheckController`, `SoundcheckApply`, `SoundcheckPanel`; additions to existing files use `AudioEngineSoundcheck`, `NotchControllerSoundcheck`, `SessionLoggerSoundcheck`, `MainComponentSoundcheck` — so `ctest -R Soundcheck` selects the whole lane and nothing else.
