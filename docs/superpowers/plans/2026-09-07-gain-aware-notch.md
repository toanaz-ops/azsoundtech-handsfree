# Gain-aware Notch (Lane G) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A notch is placed at −6 dB and only earns more depth while its bin is still ringing, releases one 6 dB rung at a time instead of clearing off a cliff, and every depth change on a running filter is a state-preserving 10 ms coefficient ramp.

**Architecture:** `Biquad` gains a `rampNotchDepth()` that interpolates its five normalised coefficients toward a new design over N samples **without** calling `reset()`; `NotchChain::setNotch` routes to it whenever the slot is Active at the same freq and Q. `NotchController` keeps a depth ladder per model notch (`deepestDb`, `stageChangedAtMs`, `quietMs`, `releasedSteps`, `ceilingDb`) and drives it from two places it already owns: the reinforce loop inside `processSpectrumForDetection` (deepen / reclamp) and step 3 of `runOnce` (ceiling clamp / release / clear). Both already hold `modelMutex_`, so retunes go through a new `pushRetuneLocked` sibling of `pushClearLocked` rather than through `setNotchImpl`, which takes the same non-recursive mutex itself.

**Tech Stack:** C++17, JUCE 8 (juce_events, juce_dsp, juce_gui_basics), GoogleTest via ctest, CMake + Visual Studio 18 2026 generator, MSVC, Python 3 (logstats fixture test).

**Spec:** `docs/superpowers/specs/2026-09-06-gain-aware-notch-design.md` (v2 + Q13, folded into §4.1 and §7). **Decisions:** `docs/superpowers/decisions/2026-09-06-lane-g-gain-aware-notch.md` — Q1–Q13 are FIXED; do not re-open them.

**Release:** 1.2.0 (`pwsh -File installer\release-alpha.ps1 -Part minor`), after the docs task.

## Revision 2026-09-07 — what a read-only cross-check changed

A second session read the real files this plan cites and found the plan asserting
things the code does not do. Everything below is already applied; the list exists so
a reviewer can check the fix rather than re-derive the defect.

**Owner ruling Q13 (new, changes Tasks 4–8 and 10).** The **effective ladder** is the
rungs of `{−6, −12, −18, −24}` that are SHALLOWER than the ceiling, **plus the ceiling
itself as the last rung**. Ceiling −10 (the shipped `presets/Music.json`) ⇒ −6 → −10.
Ceiling −13.7 ⇒ −6 → −12 → −13.7. Ceiling −18 ⇒ −6 → −12 → −18. Ceiling −6 ⇒ never
deepens. `ceilingRung` **is** the ceiling value and may be a non-multiple of 6 — the
only place a Detector notch stands off-rung. The v2 helper `ceilingRungDb` ("quantise
to the shallowest rung not deeper than the ceiling") and its test are **deleted**;
`nextDeeperRungDb` gains a `ceilingDb` parameter and IS the effective ladder.
`nextShallowerRungDb` is unchanged and already correct (−10 → −6, −13.7 → −12).

| ID | Defect | Fix |
|---|---|---|
| B-1 | Task 7's ceiling branch fired for Preset/Manual notches, violating Q8 / spec §4.1 "slider không chạm" — `tests/test_gui_wiring.cpp:1030` adopts a preset notch at −9.0, which the v2 quantisation would have pulled to −6 on the first `runOnce()` | the branch is guarded `if (n.origin == Origin::Detector)`; Q13 makes it inert for the others as well, and a new test adopts a preset at −9 and asserts `runOnce()` leaves it there |
| B-2 | Task 8 used `remembered` above the block that defines it (Task 5 put the depth choice at `NotchController.cpp:585-587`, the index search at `:589-596`), and its replacement block silently dropped Task 5's `if (origin == Origin::Soundcheck) depthDb = ceiling;` (KD-7) | the whole depth choice moved BELOW the index lookup — index known ⇒ bin known ⇒ memory lookup possible — with the Soundcheck line LAST. Tasks 5 and 8 now both show the FINAL shape of that block, Task 5 with the memory hook stubbed |
| B-3 | `pushClearLocked` (`NotchController.cpp:195-212`) only lowers `active`; `depthDB` is retained because the Clear event reads it at `:208`. Every probe of the form `depthDbForTest(c,i) < 0.0` therefore matched stale CLEARED slots | Task 4 adds `activeForTest(int, int)`; every Tasks 5–8 test that inferred "active" from depth now asks `activeForTest`. `depthDB` is never zeroed on Clear |
| B-4 | Release-to-Clear from −24 is 30 + 10 + 10 + 10 = **60 s**, not 55 s; seven `pumpQuietFor(..., 55000.0)` / `52000.0` calls stood on −24 notches and would have asserted against a notch still alive | every pump is recomputed from the rung the notch actually stands on, with the arithmetic in a comment. Task 7's tests that start at −18 (51.5 s / 52 s) were already right and are untouched |
| B-5 | `RampSineSource` at +3 dB/250 ms can never confirm: `rNorm = clamp((rise−1)/0.5, 0, 1)` against a reference frame ≥ 0.45 × 250 = 112.5 ms old (`CandidateScorer.cpp:83-106`), so 0.012 dB/ms gives rise ≈ 1.18 ⇒ rNorm 0.35, below `kConfirmScore` 0.7, forever | the slope is derived in Task 5 from the two real bounds and set to **+9.5 dB per 250 ms**; the test also asserts the Set event's `riseRatio` landed in [1.5, 2.0), and `riseRatio` is added to `NotchEvent` in Task 4 so it can |
| B-6 | `RampSineSource` / `primeAndPlaceSlowly` were inserted at `tests/test_notchcontroller.cpp:327`, above `NoiseSource` (`:329`), `pump` (`:344`) and `kWarmupBlocks` (`:308`) — all three of which they call | insertion moved to after `pump` ends (line 349), the same place Task 7 adds `pumpQuietFor` |
| M-1 | Task 9's GUI tests called `TempDir`, `pumpOneBlockThroughSlotZero`, `notchControllerForTest` — none exist in `tests/test_gui_wiring.cpp` | rewritten against the real shape at `:1016-1041`: `juce::ScopedJuceInitialiser_GUI` first, `app.getNotchControllerForTest(0)` (`MainComponent.h:188`), an explicit temp `juce::File` + `deleteFile()`, and `app.getAudioEngine().getTapBuffer(0).write(...)` then `controller0->runOnce()` |
| M-2 | `ACeilingOfMinusSixNeverDeepens` asserted no `Set` at all over 300 tone blocks, but the armed detector keeps placing FRESH notches on a ringing lane — documented at `tests/test_notchcontroller.cpp:1101-1102` | it asserts on `depthDbForTest(0, slot)` and only on commands carrying that `(lane, index)` |
| M-3 | The harness `pump` writes the raw tone into `h.tap` (`:344-349`), so the detector never sees a NOTCHED spectrum: Q7's "stops at the first rung that quiets the bin" cannot be observed headless, and the ladder always climbs to the ceiling in tests | stated in Task 6's level note and in Task 10's tester notes as a rig-only claim. No test fakes it |
| M-5 | Task 4's `adoptPreset` clamp counter incremented before the `firstLane >= width_` skip (`NotchController.cpp:242-246`) and before validation, counting notches that were never adopted | counted once per `PresetNotch`, only when `setNotch` returned true AND the clamp actually changed the value |
| M-6 | Task 7's Files list cited `:369-371 (publish releaseFrozen)`, but the publish happens in its own scope inside step 3; and `latest_.releaseFrozen` was written only under `tapAlive`, so it went stale the moment the tap died | Files list corrected; `releaseFrozen` is published on EVERY `runOnce`, `false` when `!tapAlive` |
| M-7 | `tests/fixtures/session-sample.jsonl` gained a `notch_retune` reading `from_db:-12, depth_db:-18, reason:deepen` for a notch whose `notch_set` (line 4) already said `depth_db:-18` | the fixture is coherent: set at −6, deepen −6 → −12, release −12 → −6, then the existing `auto_release` exactly 10 s later. `logstats` assertions updated to match |
| m-1..m-8 | line-number and wording corrections | applied inline; the three the cross-check itself got wrong are listed at the end of the Self-review |

Three requirements of spec §5.3 that had no step now have one: the **raise**-the-ceiling
half of the live-ceiling rule, the **preset** release-ladder and reclamp pair, and the
**INDEP** half of the LINKED/INDEP rung rule. One existing test the plan had missed —
`NotchControllerPreset.AdoptedPresetsAutoReleaseLikeDetectorNotches`
(`tests/test_notchcontroller.cpp:267`) — is now in Task 7's edit list.

## Global Constraints

- **Depth is always inside [−24, 0] dB.** `setNotchImpl` clamps `depthDB = max(depthDB, -24.0)` for EVERY `Origin` (Q12); `pushRetuneLocked` refuses anything outside the range. No `NotchCommand::Set` may ever leave this controller with `depthDB > 0` or `< -24`.
- **A deepening step is at most 6 dB.** `kDepthStepDb = 6.0`, fixed rungs `{-6, -12, -18, -24}`. The **effective ladder** (Q13) is the fixed rungs shallower than the ceiling plus the ceiling itself as the last rung, so the final step can be SMALLER than 6 dB (−6 → −10 under `presets/Music.json`) but never larger. A step in the SHALLOW direction may be larger than 6 dB (a ceiling dropped several rungs at once) — invariant 3, allowed because shallower is never dangerous.
- **Every depth change on a running notch is ramped over `kRampMs = 10.0` ms** (`NotchChain::kRampMs`), i.e. ≤ 0.6 dB/ms in the deep direction.
- **Never remove a clamp, a limiter, a NaN/denormal guard, or a bounds check.** `ScopedNoDenormals` (`AudioEngine.cpp:487`) and the `isfinite` + output clamp (`AudioEngine.cpp:605-642`) are untouched by this lane.
- **No allocation, no logging, no locks on the audio thread.** `Biquad::processSample` gains exactly one branch (`rampRemaining_ > 0`) plus five additions.
- **No new lock order.** `modelMutex_` and `snapshotMutex_` are never nested today and must not become nested. The release freeze reads the detector-thread members `frameMaxScore_` / `frameScoreValid_` (`NotchController.h:433-434`), never the snapshot.
- **`NotchCommand` gains no field and `AudioEngine::drain` (`AudioEngine.cpp:430-450`) is not touched.** A retune is a `Set` onto the same `slot`/`channel`/`index`.
- **Constants are fixed for 1.2.0 and do not reach the GUI** (spec §4.9). No new widget, no new slider.
- **`.superpowers/sdd/.gitignore` must be deleted before every commit.** The `sdd-workspace` script rewrites it with `*` on each run, which silently un-tracks this repo's `.superpowers/` policy (memory/sdd-workspace-gitignore-trap-2026-09-04.md).
- **UTF-8 explicitly on any script that reads and rewrites a repo file** (repo rule 6). `tools/logstats.py` already opens with `encoding="utf-8"`; keep it.
- **Commit with explicit paths.** Never `git add -A` and never `git add .`.
- **Build commands** (from the worktree root):
  - configure (only when a header or a `CMakeLists.txt` changed): `cmake -B build -G "Visual Studio 18 2026" -A x64`
  - build: `cmake --build build --config Release`
  - one suite: `cd build && ctest -C Release -R <regex> --output-on-failure`
  - full gate before every commit that touches `src/`: `cd build && ctest -C Release` must report `100% tests passed`.
- **Every new test carries a comment saying which production change turns it red** (repo convention, lane S/R/D).
- **The `100% tests passed (N)` numbers in each task are ESTIMATES**, carried forward from the 454 the suite reports today plus the tests each task adds. Use the number `ctest` actually prints; if it disagrees with the estimate by more than the tests you just wrote, find out why before moving on rather than editing the estimate.

## File Structure

| File | Responsibility after this plan |
|---|---|
| `src/dsp/Biquad.h/.cpp` | `rampNotchDepth()`, the shared `designPeaking()` coefficient formula, ramp state (`target*`, `delta*`, `rampRemaining_`), `stateForTest()` / `coeffsForTest()` / `rampRemainingForTest()` |
| `src/dsp/NotchChain.h/.cpp` | `kRampMs`, `setNotch` routes Active + same freq + same Q to the ramp; every other case keeps today's reset path |
| `src/dsp/CandidateScorer.h/.cpp` | `ScoreBreakdown::riseRatio` — the raw `mag_now / mag_ref`; `score` unchanged |
| `src/app/NotchController.h/.cpp` | ladder constants, `RetuneReason`, `NotchEvent::Kind::Retune` + `fromDepthDb`, five new `ModelNotch` fields, `pushRetuneLocked`, ladder helpers, deepen/reclamp in the reinforce loop, ceiling/release ladder in `runOnce` step 3, `ReleasedMemory`, `SnapshotNotch::deepestDb`, `SnapshotBuffer::releaseFrozen`, test seams |
| `src/app/MainComponent.cpp` | `notchEventToVar` emits `ev: "notch_retune"`; `savePreset` writes `deepestDb` and round-trips `notchDefaults` |
| `tools/logstats.py` | `notch_retune` UPDATES the open notch record instead of closing it; `--expect-retunes` |
| `tests/test_biquad.cpp`, `tests/test_notchchain.cpp`, `tests/test_candidatescorer.cpp`, `tests/test_notchcontroller.cpp`, `tests/test_presetmanager.cpp`, `tests/test_gui_wiring.cpp` | spec §5 |
| `tests/fixtures/session-sample.jsonl`, `tests/CMakeLists.txt` | fixture carries two `notch_retune` lines; the `logstats_fixture` test asserts them |
| `docs/GIOI-THIEU.md`, `docs/KY-THUAT-CHONG-HU.md`, `docs/spec-ring-risk.md`, `docs/superpowers/specs/2026-09-05-data-loop-design.md`, `docs/superpowers/specs/2026-09-04-anti-feedback-v2-roadmap.md`, `docs/release-notes/1.2.0-alpha.md`, `installer/TESTER-NOTES.md`, `memory/` | the docs task |

---

### Task 1: `Biquad::rampNotchDepth` — change depth without losing filter state

**Mức level dự kiến (spec §3):** none on its own. This task adds an unreachable code path — nothing calls `rampNotchDepth` until Task 2. **0 dB** change to the shipped signal path. The property it must carry into Tasks 2+: every intermediate coefficient set is a convex combination of two valid peaking designs at the same `freq`/`Q`/`sampleRate`, and the spec's §4.7 derivation proves such a combination is itself a peaking filter with numerator gain `A_n ≤ 1 ≤ 1/A_d`, so `|H| ≤ 1` at every ω — **it can never boost, at any point of the ramp**. Measured by the round-1 critic: max gain 1.9e-15 dB over 3 SR × 5 f × 3 Q × 7 depth pairs × 101 points, and 20 000 random convex combinations × 400 frequencies.

**Files:**
- Modify: `src/dsp/Biquad.h:56-113` (doc block, public methods, private members)
- Modify: `src/dsp/Biquad.cpp:21-143` (shared design helper, ramp, `processSample`, `reset`)
- Test: `tests/test_biquad.cpp` (append after `TEST(Biquad, DepthFormAppliesTheSameParameterGuardsAsThePureNotch)` at line 487)

**Interfaces:**
- Produces:
  ```cpp
  bool Biquad::rampNotchDepth (double freq, double Q, double sampleRate,
                               double depthDB, int rampSamples);
  struct Biquad::State  { double z1, z2; };
  struct Biquad::Coeffs { double b0, b1, b2, a1, a2; };
  Biquad::State  Biquad::stateForTest() const;
  Biquad::Coeffs Biquad::coeffsForTest() const;
  int            Biquad::rampRemainingForTest() const;
  ```
- Consumes: nothing new. `setNotchFilter(double,double,double,double)` (`Biquad.h:102`) keeps its exact signature and behaviour.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_biquad.cpp`. `sineWave` (line 25) and `rms` (line 38) already exist in that file's anonymous namespace — reuse them; add only the two helpers below to the same namespace (put them next to `chargeThenFeedSilence`, line 53).

```cpp
    // |H(e^{j w})| straight from the five NORMALISED coefficients the filter is
    // running RIGHT NOW. Evaluating the transfer function analytically is what
    // makes the mid-ramp no-boost test exact: it reads the coefficient set the
    // biquad actually holds at that instant, with no need to freeze the ramp.
    double magnitudeAt(const Biquad::Coeffs& c, double freqHz, double sampleRate)
    {
        const double w   = 2.0 * kPi * freqHz / sampleRate;
        const double cw  = std::cos(w),  sw  = std::sin(w);
        const double c2w = std::cos(2.0 * w), s2w = std::sin(2.0 * w);
        const double nre = c.b0 + c.b1 * cw + c.b2 * c2w;
        const double nim = -(c.b1 * sw + c.b2 * s2w);
        const double dre = 1.0 + c.a1 * cw + c.a2 * c2w;
        const double dim = -(c.a1 * sw + c.a2 * s2w);
        return std::sqrt((nre * nre + nim * nim) / (dre * dre + dim * dim));
    }

    // 40 log-spaced probes over 20 Hz..20 kHz plus 20 packed around f0, which
    // is where a peaking filter's gain actually lives.
    std::vector<double> noBoostProbeFrequencies(double f0, double Q, double sampleRate)
    {
        std::vector<double> probes;
        for (int i = 0; i < 40; ++i)
            probes.push_back(20.0 * std::pow(1000.0, static_cast<double>(i) / 39.0));
        const double bw = f0 / Q;
        for (int i = 0; i < 20; ++i)
            probes.push_back(f0 - 3.0 * bw + 6.0 * bw * static_cast<double>(i) / 19.0);
        std::vector<double> kept;
        for (double f : probes)
            if (f > 0.0 && f < 0.5 * sampleRate)
                kept.push_back(f);
        return kept;
    }
```

```cpp
// RED IF rampNotchDepth calls reset() (or setNotchFilter) internally. This is
// the one assertion the M-7 critique says every other ramp test passes anyway:
// coefficient comparisons, max|y| bounds and steady-state attenuation are all
// blind to a silently cleared state.
TEST(Biquad, RampNotchDepthPreservesFilterStateExactly)
{
    Biquad f;
    ASSERT_TRUE(f.setNotchFilter(1000.0, 30.0, 44100.0, -6.0));

    // Charge the state mid-cycle -- 10 samples is a quarter period at 1 kHz /
    // 44.1 kHz, so z1/z2 are both far from zero.
    const auto tone = sineWave(1000.0, 44100.0, 4410);
    for (int i = 0; i < 3307; ++i)
        f.processSample(0.5 * tone[static_cast<std::size_t>(i)]);

    const Biquad::State before = f.stateForTest();
    ASSERT_TRUE(f.rampNotchDepth(1000.0, 30.0, 44100.0, -12.0, 441));
    const Biquad::State after = f.stateForTest();

    EXPECT_DOUBLE_EQ(after.z1, before.z1);
    EXPECT_DOUBLE_EQ(after.z2, before.z2);
    EXPECT_NE(before.z1, 0.0) << "state was never charged: the test proves nothing";
    EXPECT_EQ(f.rampRemainingForTest(), 441);
}

// RED IF the ramp jumps coefficients in one step (a click) rather than
// interpolating. 0.08 is the hard bound: a 0.5-amplitude 1 kHz sine at
// 44.1 kHz steps by at most 2*pi*1000/44100*0.5 = 0.0712 per sample, so any
// step past 0.08 is the filter's doing, not the signal's.
TEST(Biquad, RampNotchDepthDoesNotClick)
{
    Biquad f;
    ASSERT_TRUE(f.setNotchFilter(1000.0, 30.0, 44100.0, -6.0));

    const auto tone = sineWave(1000.0, 44100.0, 12000);
    std::vector<double> y(12000);
    for (int i = 0; i < 12000; ++i)
    {
        if (i == 10000)
            ASSERT_TRUE(f.rampNotchDepth(1000.0, 30.0, 44100.0, -12.0, 441));
        y[static_cast<std::size_t>(i)] = f.processSample(0.5 * tone[static_cast<std::size_t>(i)]);
    }

    auto maxStep = [&y](int from, int to) {
        double worst = 0.0;
        for (int i = from + 1; i <= to; ++i)
            worst = std::max(worst, std::abs(y[static_cast<std::size_t>(i)]
                                             - y[static_cast<std::size_t>(i - 1)]));
        return worst;
    };
    const double before = maxStep(9559, 10000);   // the 441 samples before the ramp
    const double during = maxStep(10000, 10441);  // the ramp window itself

    EXPECT_LT(during, 0.08) << "absolute per-sample step bound for this signal";
    EXPECT_LE(during, 1.05 * before) << "the ramp must not roughen the waveform";
}

// RED IF an intermediate coefficient set boosts ANY frequency (M-4). Reads the
// live coefficients at five points of the ramp and evaluates |H| analytically,
// so nothing has to freeze the ramp to measure it.
TEST(Biquad, RampMidpointsNeverBoostAnyFrequency)
{
    const double rates[] = { 44100.0, 48000.0, 96000.0 };
    const double freqs[] = { 120.0, 482.0, 1000.0, 4000.0, 9000.0 };
    const double qs[]    = { 8.0, 30.0, 50.0 };
    const double pairs[][2] = { { -6.0, -12.0 }, { -12.0, -6.0 }, { -6.0, -24.0 },
                                { -24.0, -6.0 }, { -12.0, -18.0 }, { -18.0, -12.0 },
                                { 0.0, -24.0 } };

    for (double sr : rates)
        for (double f0 : freqs)
            for (double Q : qs)
                for (const auto& pair : pairs)
                {
                    if (f0 >= 0.5 * sr)
                        continue;
                    Biquad f;
                    ASSERT_TRUE(f.setNotchFilter(f0, Q, sr, pair[0]));
                    ASSERT_TRUE(f.rampNotchDepth(f0, Q, sr, pair[1], 6));

                    const auto probes = noBoostProbeFrequencies(f0, Q, sr);
                    for (int step = 0; step < 5; ++step)
                    {
                        f.processSample(0.0);   // advance one ramp sample
                        const Biquad::Coeffs c = f.coeffsForTest();
                        for (double probe : probes)
                            EXPECT_LE(magnitudeAt(c, probe, sr), 1.0 + 1e-9)
                                << "boost at " << probe << " Hz, step " << step
                                << ", sr " << sr << ", f0 " << f0 << ", Q " << Q;
                    }
                }
}

// RED IF a ramp restarted mid-flight lands outside the convex hull of the two
// designs it sits between (the 3-point case of the §4.7 proof).
TEST(Biquad, RampRestartedMidFlightStillNeverBoosts)
{
    Biquad f;
    ASSERT_TRUE(f.setNotchFilter(1000.0, 30.0, 48000.0, -6.0));
    ASSERT_TRUE(f.rampNotchDepth(1000.0, 30.0, 48000.0, -24.0, 20));
    for (int i = 0; i < 9; ++i)
        f.processSample(0.0);
    ASSERT_TRUE(f.rampNotchDepth(1000.0, 30.0, 48000.0, -12.0, 20));

    const auto probes = noBoostProbeFrequencies(1000.0, 30.0, 48000.0);
    for (int step = 0; step < 20; ++step)
    {
        f.processSample(0.0);
        const Biquad::Coeffs c = f.coeffsForTest();
        for (double probe : probes)
            EXPECT_LE(magnitudeAt(c, probe, 48000.0), 1.0 + 1e-9);
    }
}

// RED IF the ramp accumulates rounding error instead of snapping to target on
// its last sample.
TEST(Biquad, RampReachesTheExactTargetCoefficients)
{
    Biquad ramped, direct;
    ASSERT_TRUE(ramped.setNotchFilter(1000.0, 30.0, 48000.0, -6.0));
    ASSERT_TRUE(direct.setNotchFilter(1000.0, 30.0, 48000.0, -18.0));
    ASSERT_TRUE(ramped.rampNotchDepth(1000.0, 30.0, 48000.0, -18.0, 480));

    for (int i = 0; i < 480; ++i)
        ramped.processSample(0.0);

    const Biquad::Coeffs a = ramped.coeffsForTest();
    const Biquad::Coeffs b = direct.coeffsForTest();
    EXPECT_DOUBLE_EQ(a.b0, b.b0);
    EXPECT_DOUBLE_EQ(a.b1, b.b1);
    EXPECT_DOUBLE_EQ(a.b2, b.b2);
    EXPECT_DOUBLE_EQ(a.a1, b.a1);
    EXPECT_DOUBLE_EQ(a.a2, b.a2);
    EXPECT_EQ(ramped.rampRemainingForTest(), 0);
}

// RED IF an intermediate coefficient set puts a pole outside the unit circle.
TEST(Biquad, RampDoesNotDivergeOverEveryLadderStepAndRate)
{
    const double rates[]  = { 44100.0, 48000.0, 96000.0 };
    const double ladder[] = { -6.0, -12.0, -18.0, -24.0 };

    for (double sr : rates)
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
            {
                if (i == j) continue;
                Biquad f;
                ASSERT_TRUE(f.setNotchFilter(1000.0, 30.0, sr, ladder[i]));
                ASSERT_TRUE(f.rampNotchDepth(1000.0, 30.0, sr, ladder[j],
                                             static_cast<int>(0.010 * sr)));
                const auto tone = sineWave(1000.0, sr, 8192);
                for (int n = 0; n < 8192; ++n)
                {
                    const double out = f.processSample(tone[static_cast<std::size_t>(n)]);
                    ASSERT_LE(std::abs(out), 1.0 + 1e-9)
                        << "sr " << sr << " " << ladder[i] << " -> " << ladder[j]
                        << " at sample " << n;
                }
            }
}

// RED IF a rejected ramp half-applies: coefficients, state or the in-flight
// ramp move even though the call returned false.
TEST(Biquad, RampNotchDepthRejectionLeavesEverythingUntouched)
{
    Biquad f;
    ASSERT_TRUE(f.setNotchFilter(1000.0, 30.0, 48000.0, -6.0));
    const auto tone = sineWave(1000.0, 48000.0, 1000);
    for (double s : tone)
        f.processSample(s);
    ASSERT_TRUE(f.rampNotchDepth(1000.0, 30.0, 48000.0, -12.0, 480));
    for (int i = 0; i < 100; ++i)
        f.processSample(0.0);

    const Biquad::Coeffs c0 = f.coeffsForTest();
    const Biquad::State  s0 = f.stateForTest();
    const int            r0 = f.rampRemainingForTest();

    EXPECT_FALSE(f.rampNotchDepth(24000.0, 30.0, 48000.0, -18.0, 480));  // >= Nyquist
    EXPECT_FALSE(f.rampNotchDepth(1000.0,   0.0, 48000.0, -18.0, 480));  // Q <= 0
    EXPECT_FALSE(f.rampNotchDepth(1000.0,  30.0,     0.0, -18.0, 480));  // sr <= 0
    EXPECT_FALSE(f.rampNotchDepth(-5.0,    30.0, 48000.0, -18.0, 480));  // freq <= 0
    EXPECT_FALSE(f.rampNotchDepth(1000.0,  30.0, 48000.0,  +3.0, 480));  // boost

    const Biquad::Coeffs c1 = f.coeffsForTest();
    EXPECT_DOUBLE_EQ(c1.b0, c0.b0);
    EXPECT_DOUBLE_EQ(c1.a1, c0.a1);
    EXPECT_DOUBLE_EQ(c1.a2, c0.a2);
    EXPECT_DOUBLE_EQ(f.stateForTest().z1, s0.z1);
    EXPECT_DOUBLE_EQ(f.stateForTest().z2, s0.z2);
    EXPECT_EQ(f.rampRemainingForTest(), r0);
}

// RED IF a setNotchFilter landing mid-ramp lets the stale ramp keep walking
// the coefficients afterwards.
TEST(Biquad, SetNotchFilterCancelsAnInFlightRampAndResetsState)
{
    Biquad f;
    ASSERT_TRUE(f.setNotchFilter(1000.0, 30.0, 48000.0, -6.0));
    const auto tone = sineWave(1000.0, 48000.0, 1000);
    for (double s : tone)
        f.processSample(s);
    ASSERT_TRUE(f.rampNotchDepth(1000.0, 30.0, 48000.0, -24.0, 480));
    for (int i = 0; i < 100; ++i)
        f.processSample(0.0);

    ASSERT_TRUE(f.setNotchFilter(2000.0, 30.0, 48000.0, -12.0));
    EXPECT_EQ(f.rampRemainingForTest(), 0);
    EXPECT_DOUBLE_EQ(f.stateForTest().z1, 0.0);
    EXPECT_DOUBLE_EQ(f.stateForTest().z2, 0.0);

    Biquad direct;
    ASSERT_TRUE(direct.setNotchFilter(2000.0, 30.0, 48000.0, -12.0));
    f.processSample(0.0);   // one more sample: a live ramp would move b0_ here
    EXPECT_DOUBLE_EQ(f.coeffsForTest().b0, direct.coeffsForTest().b0);
}

// RED IF rampSamples <= 0 stops meaning "apply immediately, keep the state".
TEST(Biquad, NonPositiveRampSamplesAppliesTheTargetAtOnceAndKeepsState)
{
    Biquad f;
    ASSERT_TRUE(f.setNotchFilter(1000.0, 30.0, 48000.0, -6.0));
    const auto tone = sineWave(1000.0, 48000.0, 1000);
    for (double s : tone)
        f.processSample(s);
    const Biquad::State before = f.stateForTest();

    ASSERT_TRUE(f.rampNotchDepth(1000.0, 30.0, 48000.0, -18.0, 0));
    Biquad direct;
    ASSERT_TRUE(direct.setNotchFilter(1000.0, 30.0, 48000.0, -18.0));

    EXPECT_DOUBLE_EQ(f.coeffsForTest().b0, direct.coeffsForTest().b0);
    EXPECT_DOUBLE_EQ(f.coeffsForTest().a2, direct.coeffsForTest().a2);
    EXPECT_DOUBLE_EQ(f.stateForTest().z1, before.z1);
    EXPECT_EQ(f.rampRemainingForTest(), 0);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --config Release 2>&1 | tail -5`
Expected: compile errors — `'rampNotchDepth': is not a member of 'Biquad'`, same for `stateForTest`, `coeffsForTest`, `rampRemainingForTest`.

- [ ] **Step 3: Extend `src/dsp/Biquad.h`**

Append this paragraph to the header's doc block, immediately before `#pragma once` (line 56):

```cpp
// Depth retune without a click: rampNotchDepth()
// ==============================================
// setNotchFilter() calls reset() because a new DESIGN owns no state from the
// old one. That is right when the frequency or Q moves, and wrong when only
// the depth does: clearing z1/z2 mid-signal is a step discontinuity straight
// into a PA. rampNotchDepth() therefore keeps the state and walks the five
// normalised coefficients linearly to the new design over `rampSamples`.
//
// Why an interpolated coefficient set is safe (spec 4.7, M-4)
// -----------------------------------------------------------
// Fix freq/Q/sampleRate, write u_i = alpha/A_i and d_i = 1 + u_i. Every convex
// combination (weights w_i) of the a0-normalised peaking designs keeps
// b1 == a1 and satisfies P + alpha*K == 1, with P = sum(w_i/d_i) and
// K = sum(w_i/(A_i*d_i)). Substituting, the interpolated set IS a peaking RBJ
// filter with numerator gain A_n = sum(w_i*A_i/d_i)/P and denominator gain
// 1/A_d = sum(w_i/(A_i*d_i))/P:
//
//   |H(w)|^2 = [(cos w - cos w0)^2 + alpha^2 * A_n^2 * sin^2 w]
//            / [(cos w - cos w0)^2 + alpha^2 * A_d^-2 * sin^2 w]
//
// Every A_i <= 1 (depth <= 0), so A_n <= 1 <= 1/A_d and |H| <= 1 at EVERY
// frequency -- the ramp cannot boost anything, including the frequency it is
// pointed at. The pole radius is sqrt((1 - alpha/A_d)/(1 + alpha/A_d)) < 1, so
// it cannot diverge either. A ramp restarted mid-flight is the three-point
// case of the same argument. Measured 2026-09-06: max gain 1.9e-15 dB over
// 3 rates x 5 frequencies x 3 Qs x 7 depth pairs x 101 probes, and over 20 000
// random convex combinations x 400 frequencies. Coefficient deltas are ~1e-6
// per sample -- nowhere near the subnormal range.
//
// A ramp is only ever entered between two designs sharing freq, Q and
// sampleRate (NotchChain::setNotch enforces that). setNotchFilter() and
// reset() CANCEL an in-flight ramp: the newer instruction wins.
//
// NotchChain::clearNotch does NOT cancel a ramp, so an Idle slot can carry
// rampRemaining_ > 0 that nobody ticks. Harmless: setNotch on an Idle slot
// takes the reset path, which cancels it. And a ramp cut short by a device
// stop leaves the filter at an intermediate depth while NotchInfo.depthDB
// already reads the target -- 10 ms of disagreement, accepted (spec 4.7, m-5).
```

Add to the public section, after `bool setNotchFilter(double freq, double Q, double sampleRate, double depthDB);` (line 102):

```cpp
    // Retunes a RUNNING notch to `depthDB` while keeping the filter state,
    // interpolating all five coefficients over `rampSamples` calls to
    // processSample(). Applies the SAME four rejections as the four-argument
    // setNotchFilter (sampleRate > 0, Q > 0, 0 < freq < sampleRate/2,
    // depthDB <= 0); on rejection it returns false and leaves the
    // coefficients, the state AND any in-flight ramp exactly as they were.
    // rampSamples <= 0 installs the target immediately, still without reset().
    // Branch-only and allocation-free: reachable from the audio thread via
    // NotchChain::setNotch.
    bool rampNotchDepth(double freq, double Q, double sampleRate, double depthDB,
                        int rampSamples);

    // TEST ACCESSORS ONLY -- the ramp's whole point is that state SURVIVES it,
    // and no black-box measurement can tell a preserved state from a cleared
    // one (spec 5.1, M-7).
    struct State  { double z1, z2; };
    struct Coeffs { double b0, b1, b2, a1, a2; };
    State  stateForTest()  const { return { z1_, z2_ }; }
    Coeffs coeffsForTest() const { return { b0_, b1_, b2_, a1_, a2_ }; }
    int    rampRemainingForTest() const { return rampRemaining_; }
```

Replace the private section (lines 106-113) with:

```cpp
private:
    // The four-argument peaking design, in ONE place: setNotchFilter and
    // rampNotchDepth must never be able to drift apart on either the formula
    // or the four rejections. Writes the a0-normalised b0,b1,b2,a1,a2 into
    // `out` and returns true; returns false and writes nothing on rejection.
    static bool designPeaking(double freq, double Q, double sampleRate,
                              double depthDB, double out[5]);

    // Coefficients (a0-normalised).
    double b0_, b1_, b2_;
    double a1_, a2_;

    // Direct Form I transposed state.
    double z1_, z2_;

    // Ramp state. rampRemaining_ == 0 means "not ramping" and is the only
    // thing processSample() branches on.
    double target_[5];
    double delta_[5];
    int    rampRemaining_;
```

- [ ] **Step 4: Implement in `src/dsp/Biquad.cpp`**

Extend the constructor initialiser list (lines 10-19) to zero the new members:

```cpp
Biquad::Biquad()
    : b0_(1.0)
    , b1_(0.0)
    , b2_(0.0)
    , a1_(0.0)
    , a2_(0.0)
    , z1_(0.0)
    , z2_(0.0)
    , target_{ 1.0, 0.0, 0.0, 0.0, 0.0 }
    , delta_{ 0.0, 0.0, 0.0, 0.0, 0.0 }
    , rampRemaining_(0)
{
}
```

Add the shared design helper immediately after the constructor:

```cpp
bool Biquad::designPeaking(double freq, double Q, double sampleRate,
                           double depthDB, double out[5])
{
    // The same four guards as the pure notch -- see the derivation in
    // Biquad.h. The pole radius here is sqrt((1 - alpha/A) / (1 + alpha/A)),
    // i.e. the pure-notch expression with alpha replaced by alpha/A, so the
    // same inputs push the poles onto or outside the unit circle.
    if (sampleRate <= 0.0)          return false;
    if (Q <= 0.0)                   return false;
    if (freq <= 0.0)                return false;
    if (freq >= 0.5 * sampleRate)   return false;

    // A positive depth is a BOOST at the centre frequency. This filter sits in
    // a feedback eliminator, so the one frequency it must never amplify is the
    // one it was pointed at. Refuse rather than trust the caller's sign.
    if (depthDB > 0.0)              return false;

    const double A     = std::pow(10.0, depthDB / 40.0);
    const double omega = 2.0 * kPi * freq / sampleRate;
    const double alpha = std::sin(omega) / (2.0 * Q);
    const double cosw  = std::cos(omega);

    const double a0 = 1.0 + alpha / A;

    // Normalise so a0 == 1 -- lets processSample skip one multiply.
    out[0] = (1.0 + alpha * A) / a0;   // b0
    out[1] = (-2.0 * cosw)     / a0;   // b1
    out[2] = (1.0 - alpha * A) / a0;   // b2
    out[3] = (-2.0 * cosw)     / a0;   // a1
    out[4] = (1.0 - alpha / A) / a0;   // a2
    return true;
}
```

Replace the body of the four-argument `setNotchFilter` (lines 72-128) with the helper call, keeping its `reset()` and adding the ramp cancel:

```cpp
bool Biquad::setNotchFilter(double freq, double Q, double sampleRate, double depthDB)
{
    double c[5];
    if (! designPeaking(freq, Q, sampleRate, depthDB, c))
    {
        return false;
    }

    b0_ = c[0]; b1_ = c[1]; b2_ = c[2]; a1_ = c[3]; a2_ = c[4];

    // A new DESIGN wins over an in-flight depth ramp: the ramp interpolates
    // toward a target this call has just superseded.
    rampRemaining_ = 0;

    // Filter coefficients changed -- state belongs to the previous design.
    reset();
    return true;
}
```

Add `rampNotchDepth` after it:

```cpp
bool Biquad::rampNotchDepth(double freq, double Q, double sampleRate, double depthDB,
                            int rampSamples)
{
    double c[5];
    if (! designPeaking(freq, Q, sampleRate, depthDB, c))
    {
        // Rejected: coefficients, state and any in-flight ramp are untouched.
        return false;
    }

    if (rampSamples <= 0)
    {
        b0_ = c[0]; b1_ = c[1]; b2_ = c[2]; a1_ = c[3]; a2_ = c[4];
        rampRemaining_ = 0;
        return true;   // NO reset(): the state is still valid at this freq/Q.
    }

    // Deltas are measured from the coefficients the filter is running RIGHT
    // NOW, not from the previous target, so a second command arriving
    // mid-ramp starts a fresh straight line from wherever the first one got
    // to instead of snapping back.
    const double current[5] = { b0_, b1_, b2_, a1_, a2_ };
    for (int i = 0; i < 5; ++i)
    {
        target_[i] = c[i];
        delta_[i]  = (c[i] - current[i]) / static_cast<double>(rampSamples);
    }
    rampRemaining_ = rampSamples;
    return true;
}
```

Replace `processSample` (lines 130-137):

```cpp
double Biquad::processSample(double input)
{
    // The ONLY new work on the audio thread: one comparison, and while a ramp
    // is live five additions. No allocation, no logging, no second branch.
    if (rampRemaining_ > 0)
    {
        if (--rampRemaining_ == 0)
        {
            // Land ON the target rather than on the accumulated sum of
            // deltas, so a long ramp cannot drift by rounding error.
            b0_ = target_[0]; b1_ = target_[1]; b2_ = target_[2];
            a1_ = target_[3]; a2_ = target_[4];
        }
        else
        {
            b0_ += delta_[0]; b1_ += delta_[1]; b2_ += delta_[2];
            a1_ += delta_[3]; a2_ += delta_[4];
        }
    }

    // Direct Form I transposed.
    const double output = b0_ * input + z1_;
    z1_ = b1_ * input - a1_ * output + z2_;
    z2_ = b2_ * input - a2_ * output;
    return output;
}
```

Replace `reset` (lines 139-143):

```cpp
void Biquad::reset()
{
    z1_ = 0.0;
    z2_ = 0.0;
    // An explicit reset is a statement that the past is gone; a ramp toward a
    // target designed against that past has nothing left to interpolate.
    rampRemaining_ = 0;
}
```

- [ ] **Step 5: Run the Biquad tests**

Run:
```
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
cd build && ctest -C Release -R Biquad --output-on-failure
```
Expected: PASS — all `Biquad.*` tests, the nine new ones included.

- [ ] **Step 6: Run the full suite**

Run: `cd build && ctest -C Release`
Expected: `100% tests passed` (454 + 9 = 463).

- [ ] **Step 7: Commit**

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add src/dsp/Biquad.h src/dsp/Biquad.cpp tests/test_biquad.cpp
```
```bash
git commit -m "feat(dsp): Biquad::rampNotchDepth -- retune depth without clearing filter state"
```

---

### Task 2: `NotchChain::setNotch` routes a depth-only change to the ramp

**Mức level dự kiến (spec §3):** still **0 dB** in the shipped app — `NotchController` does not send a second `Set` onto a live index until Task 6. What this task fixes is the mechanism: from here on, a `Set` with the same freq and Q on an Active slot changes the cut over 10 ms instead of resetting the filter. The measured attenuation at f0 after the ramp completes is the requested rung within **±0.5 dB** at −6 / −12 / −18 / −24 (asserted in the test below).

**Files:**
- Modify: `src/dsp/NotchChain.h:24-56` (include, `kRampMs`, `setNotch` doc)
- Modify: `src/dsp/NotchChain.cpp:1-53` (include, `setNotch`)
- Test: `tests/test_notchchain.cpp` (append after `TEST(NotchChain, SetNotchRejectsAPositiveDepthAndLeavesTheSlotIdle)` at line 440)

**Interfaces:**
- Consumes (Task 1): `bool Biquad::rampNotchDepth(double freq, double Q, double sampleRate, double depthDB, int rampSamples)`.
- Produces:
  ```cpp
  static constexpr double NotchChain::kRampMs = 10.0;
  // void NotchChain::setNotch(int index, double freq, double Q, double depthDB)
  //   -- unchanged signature; NEW behaviour when the slot is Active and both
  //      freq and Q compare equal to the stored NotchInfo.
  ```

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_notchchain.cpp`. `sineWave` (line 20) and `rms` (line 31) already exist there.

```cpp
// RED IF a depth-only setNotch on a running slot goes back through
// setNotchFilter (which resets the state). After a reset, feeding 0.0 into a
// charged chain returns EXACTLY 0.0; after a ramp it does not.
TEST(NotchChain, DepthOnlyRetuneOfARunningNotchKeepsTheFilterState)
{
    NotchChain chain(48000.0);
    chain.setNotch(0, 1000.0, 30.0, -6.0);

    const auto tone = sineWave(1000.0, 48000.0, 4800);
    for (int i = 0; i < 3600; ++i)
        chain.processSample(tone[static_cast<std::size_t>(i)]);

    chain.setNotch(0, 1000.0, 30.0, -12.0);   // same freq, same Q: ramp
    EXPECT_NE(chain.processSample(0.0), 0.0) << "state was cleared: this was a reset, not a ramp";

    // NotchInfo reports the TARGET immediately, not the ramp's position.
    EXPECT_DOUBLE_EQ(chain.getNotchInfo(0).depthDB, -12.0);
    EXPECT_EQ(chain.getNotchInfo(0).state, NotchChain::NotchState::Active);
}

// RED IF a frequency change stops taking the reset path.
TEST(NotchChain, ChangingFrequencyStillResetsTheFilterState)
{
    NotchChain chain(48000.0);
    chain.setNotch(0, 1000.0, 30.0, -6.0);

    const auto tone = sineWave(1000.0, 48000.0, 4800);
    for (int i = 0; i < 3600; ++i)
        chain.processSample(tone[static_cast<std::size_t>(i)]);

    chain.setNotch(0, 1500.0, 30.0, -6.0);            // different freq
    EXPECT_DOUBLE_EQ(chain.processSample(0.0), 0.0);  // state cleared
    EXPECT_DOUBLE_EQ(chain.getNotchInfo(0).frequency, 1500.0);
}

// RED IF a Q change stops taking the reset path. m-2: the controller must
// resend the STORED Q for exactly this reason -- one bit of difference and the
// retune becomes a reset, i.e. a click.
TEST(NotchChain, ChangingQStillResetsTheFilterState)
{
    NotchChain chain(48000.0);
    chain.setNotch(0, 1000.0, 30.0, -6.0);

    const auto tone = sineWave(1000.0, 48000.0, 4800);
    for (int i = 0; i < 3600; ++i)
        chain.processSample(tone[static_cast<std::size_t>(i)]);

    chain.setNotch(0, 1000.0, 30.000000001, -6.0);
    EXPECT_DOUBLE_EQ(chain.processSample(0.0), 0.0);
}

// RED IF an Idle slot stops taking the reset path (it has no state worth
// keeping, and its stored NotchInfo may name a different design entirely).
TEST(NotchChain, SetNotchOnAnIdleSlotStillTakesTheResetPath)
{
    NotchChain chain(48000.0);
    chain.setNotch(0, 1000.0, 30.0, -6.0);
    const auto tone = sineWave(1000.0, 48000.0, 4800);
    for (int i = 0; i < 3600; ++i)
        chain.processSample(tone[static_cast<std::size_t>(i)]);

    chain.clearNotch(0);
    chain.setNotch(0, 1000.0, 30.0, -12.0);   // same params, but the slot was Idle
    EXPECT_DOUBLE_EQ(chain.processSample(0.0), 0.0);
}

// The measured level, rung by rung (CLAUDE.md: state the level change, and
// prove it in a test). RED IF a ramped retune lands anywhere but the rung it
// was asked for. 0.5 dB is the tolerance the spec 5.2 names.
TEST(NotchChain, MeasuredAttenuationMatchesEveryLadderRungWithinHalfADecibel)
{
    const double ladder[] = { -6.0, -12.0, -18.0, -24.0 };
    const int    rampSamples = static_cast<int>(0.010 * 48000.0);   // kRampMs

    NotchChain chain(48000.0);
    chain.setNotch(0, 1000.0, 30.0, -6.0);

    const auto tone = sineWave(1000.0, 48000.0, 96000);
    for (double rung : ladder)
    {
        chain.setNotch(0, 1000.0, 30.0, rung);

        // Let the ramp finish and the filter settle at the new design before
        // measuring: a Q of 30 at 1 kHz rings for ~10 ms on its own.
        std::vector<double> out(48000);
        for (int i = 0; i < 48000; ++i)
            out[static_cast<std::size_t>(i)] = chain.processSample(tone[static_cast<std::size_t>(i)]);
        ASSERT_GT(48000, rampSamples);

        const double measuredDb = 20.0 * std::log10(rms(out, 24000)
                                                    / rms(std::vector<double>(tone.begin(),
                                                                              tone.begin() + 48000), 24000));
        EXPECT_NEAR(measuredDb, rung, 0.5) << "rung " << rung << " dB";
    }
}

// RED IF a ramped retune can be half-applied. A rejected design must leave the
// slot -- coefficients, state and reported depth -- exactly as it was.
TEST(NotchChain, RejectedDepthOnARunningNotchLeavesTheSlotUnchanged)
{
    NotchChain chain(48000.0);
    chain.setNotch(0, 1000.0, 30.0, -12.0);
    chain.setNotch(0, 1000.0, 30.0, +3.0);   // boost: refused by the biquad
    EXPECT_DOUBLE_EQ(chain.getNotchInfo(0).depthDB, -12.0);
    EXPECT_EQ(chain.getNotchInfo(0).state, NotchChain::NotchState::Active);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --config Release && cd build && ctest -C Release -R NotchChain --output-on-failure`
Expected: FAIL — `DepthOnlyRetuneOfARunningNotchKeepsTheFilterState` reports `chain.processSample(0.0)` equal to 0 (today's `setNotch` always resets).

- [ ] **Step 3: Extend `src/dsp/NotchChain.h`**

Add after `static constexpr int MAX_NOTCHES = 16;` (line 33):

```cpp
    // Depth-only retune ramp (spec 4.7, decision Q5). 10 ms is one block at a
    // 2048 buffer and seven at 64, so it is always at least one callback and
    // never long enough to be heard as a slew. 6 dB over 10 ms is 0.6 dB/ms --
    // the invariant the controller's ladder is written against.
    static constexpr double kRampMs = 10.0;
```

Replace the `setNotch` doc comment (lines 53-56) with:

```cpp
    // Installs a notch in `index`. If the biquad rejects the parameters
    // (sampleRate <= 0, Q <= 0, freq <= 0, or freq >= sampleRate/2 -- see
    // Biquad.h) the slot is left untouched and stays whatever it was.
    //
    // DEPTH-ONLY RETUNE (spec 4.7): when the slot is already Active and both
    // `freq` and `Q` compare EQUAL to the stored NotchInfo, only the depth is
    // moving, so the filter state is still meaningful and clearing it would be
    // a step discontinuity into the PA. That case routes to
    // Biquad::rampNotchDepth and interpolates over kRampMs instead. Everything
    // else -- an Idle slot, a new frequency, a new Q -- keeps taking the
    // setNotchFilter + reset path exactly as before.
    //
    // The comparison is a plain `==` on doubles ON PURPOSE: the caller
    // (NotchController::pushRetuneLocked) resends the freq and Q it stored
    // when the notch was placed, so the values are bit-identical by
    // construction. A near-miss falls through to the reset path, which is the
    // SAFE direction to fail in -- an audible click, never a filter running
    // one design while claiming another.
    void   setNotch(int index, double freq, double Q, double depthDB);
```

- [ ] **Step 4: Implement in `src/dsp/NotchChain.cpp`**

Add the include at the top (after line 1):

```cpp
#include "dsp/NotchChain.h"

#include <cmath>
```

Replace `setNotch` (lines 25-53) — keep the whole existing comment block, and insert the ramp branch before the reset path:

```cpp
void NotchChain::setNotch(int index, double freq, double Q, double depthDB)
{
    if (index < 0 || index >= MAX_NOTCHES)
    {
        return;
    }

    // Depth-only retune of a RUNNING notch: keep the state, ramp the
    // coefficients (see the header). rampSamples is rounded from kRampMs
    // against the chain's CURRENT rate, so the ramp is 10 ms of real time at
    // any device rate. A rejected design leaves the slot completely alone,
    // exactly like the path below.
    if (notchInfo_[index].state == NotchState::Active
        && freq == notchInfo_[index].frequency
        && Q    == notchInfo_[index].Q)
    {
        const int rampSamples = (int) std::lround(kRampMs * sampleRate_ / 1000.0);
        if (! filters_[index].rampNotchDepth(freq, Q, sampleRate_, depthDB, rampSamples))
        {
            return;
        }
        // The reported depth is the TARGET, from the instant the command is
        // accepted -- the model, the GUI and the preset all describe intent,
        // not the ramp's momentary position (spec 4.7).
        notchInfo_[index].depthDB = depthDB;
        return;
    }

    // Configure the biquad for this notch, DEPTH INCLUDED. `depthDB` used to
    // be recorded here and dropped on the floor -- the three-argument
    // setNotchFilter is an infinite-depth null and has no depth parameter --
    // so every notch this app placed was a full null no matter what was asked
    // for, and spec 5.1's 6-24 dB range was unreachable.
    //
    // If the biquad rejects the parameters (see Biquad.h) the slot is left
    // COMPLETELY untouched. Activating it anyway would mark the slot Active
    // while filters_[index] still held whatever design was there before, so
    // the chain would report a notch at one frequency and filter another.
    // A positive depthDB is among the rejected cases: it would boost the
    // ringing frequency instead of cutting it.
    if (! filters_[index].setNotchFilter(freq, Q, sampleRate_, depthDB))
    {
        return;
    }

    notchInfo_[index].frequency = freq;
    notchInfo_[index].Q         = Q;
    notchInfo_[index].depthDB   = depthDB;
    notchInfo_[index].state     = NotchState::Active;
}
```

- [ ] **Step 5: Run the NotchChain tests**

Run:
```
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
cd build && ctest -C Release -R NotchChain --output-on-failure
```
Expected: PASS. Record the four measured attenuations printed by a failure-free run of `MeasuredAttenuationMatchesEveryLadderRungWithinHalfADecibel` — they are the numbers the release note quotes.

- [ ] **Step 6: Run the full suite**

Run: `cd build && ctest -C Release`
Expected: `100% tests passed` (469).

- [ ] **Step 7: Commit**

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add src/dsp/NotchChain.h src/dsp/NotchChain.cpp tests/test_notchchain.cpp
```
```bash
git commit -m "feat(dsp): NotchChain ramps a depth-only retune over kRampMs instead of resetting"
```

---

### Task 3: `ScoreBreakdown::riseRatio` — expose the raw rise the placement policy needs

**Mức level dự kiến (spec §3):** **0 dB.** This task adds one field to a struct and assigns an already-computed local. `score`, `pNorm`, `rNorm`, `mNorm` and `penalty` are byte-for-byte what they were, so no placement decision moves.

**Files:**
- Modify: `src/dsp/CandidateScorer.h:82-90` (`ScoreBreakdown`)
- Modify: `src/dsp/CandidateScorer.cpp:76-110` (the rise branch)
- Test: `tests/test_candidatescorer.cpp`

**Interfaces:**
- Consumes: nothing from Tasks 1–2.
- Produces:
  ```cpp
  struct CandidateScorer::ScoreBreakdown
  {
      float rawPeakiness = 0.0f;
      float pNorm = 0.0f, rNorm = 0.0f, mNorm = 0.0f;
      float riseRatio = 1.0f;          // NEW: raw mag_now / mag_ref, unsaturated
      float penalty = 1.0f;
      float score = 0.0f;
      const float* refFrame = nullptr;
      double refAgeMs = 0.0;
  };
  ```
  Task 5 reads it as `pc.breakdown.riseRatio`.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_candidatescorer.cpp`. That file already has `Rig` (line 72, with `tap`, `detector`, `analyzer`, `scorer`, `cycle()`, `feedHops()`), `makeToneInNoise()` (line 36), `kHop`, `kFrameMs` and `kSampleRate` in its anonymous namespace — use those names, do not invent new helpers.

```cpp
// RED IF riseRatio stops being the RAW mag_now/mag_ref. rNorm saturates at
// rise 1.5 (CandidateScorer.cpp: (rise - 1)/0.5, clamped to 1), so it cannot
// tell "just rising" from "up 12 dB in 250 ms" -- and that distinction is
// exactly what lane G's jump to -12 dB needs (spec 4.2, Q6).
TEST (CandidateScorer, RiseRatioIsTheRawRatioWhileRNormStaysSaturated)
{
    Rig rig;

    // ~700 ms of quiet first, so the rise history holds frames older than the
    // 0.45 x 250 ms minimum age; then a loud tone switched on hard.
    const auto quiet = makeToneInNoise (1000.0, 0.001f, 0.01f, 4242u,
                                        static_cast<std::size_t> (kHop) * 70);
    rig.feedHops (quiet, 66);

    const auto loud = makeToneInNoise (1000.0, 1.0f, 0.01f, 4243u,
                                       static_cast<std::size_t> (kHop) * 8);
    CandidateScorer::ScoreBreakdown seen {};
    bool got = false;
    for (std::size_t h = 0; h < 4 && ! got; ++h)
    {
        ASSERT_EQ (rig.tap.write (loud.data() + h * static_cast<std::size_t> (kHop),
                                  static_cast<std::size_t> (kHop)),
                   static_cast<std::size_t> (kHop));
        const auto spectrum = rig.detector.processLatestBlock (rig.tap);
        rig.scorer.beginBlock (kSampleRate);
        if (spectrum.magnitudes == nullptr)
            continue;

        const auto detected = rig.analyzer.analyse (spectrum);
        for (std::size_t i = 0; i < detected.count; ++i)
        {
            const auto b = rig.scorer.scoreCandidateDetailed (detected.candidates[i],
                                                              spectrum.magnitudes, {});
            if (b.refFrame != nullptr && b.rNorm >= 1.0f) { seen = b; got = true; break; }
        }
        rig.scorer.commitBlock (spectrum.magnitudes, kFrameMs);
    }

    ASSERT_TRUE (got) << "no saturated-rise candidate to inspect";
    EXPECT_FLOAT_EQ (seen.rNorm, 1.0f);
    EXPECT_GT (seen.riseRatio, 2.0f) << "a hard tone start must read past the steep-rise line";
}

// RED IF the no-history branch stops reporting a neutral 1.0. 1.0 means "no
// measurable rise", which is the CONSERVATIVE answer for lane G's gate
// (riseRatio >= 2.0 starts a notch at -12): an unknown rise must never buy
// extra depth.
TEST (CandidateScorer, RiseRatioIsNeutralWithoutUsableHistory)
{
    Rig rig;
    const auto tone = makeToneInNoise (1000.0, 1.0f, 0.01f, 77u,
                                       static_cast<std::size_t> (kHop) * 4);
    ASSERT_EQ (rig.tap.write (tone.data(), static_cast<std::size_t> (kHop)),
               static_cast<std::size_t> (kHop));
    const auto spectrum = rig.detector.processLatestBlock (rig.tap);
    rig.scorer.beginBlock (kSampleRate);
    ASSERT_NE (spectrum.magnitudes, nullptr);

    const auto detected = rig.analyzer.analyse (spectrum);
    ASSERT_GT (detected.count, 0u);
    const auto b = rig.scorer.scoreCandidateDetailed (detected.candidates[0],
                                                      spectrum.magnitudes, {});
    EXPECT_FLOAT_EQ (b.riseRatio, 1.0f);
}

// RED IF the new field changes the arithmetic. The product identity is the
// whole contract: lane G reads one more intermediate number and moves nothing.
TEST (CandidateScorer, ScoreStaysTheProductOfTheSameFourFactors)
{
    Rig rig;
    const auto quiet = makeToneInNoise (1000.0, 0.001f, 0.01f, 11u,
                                        static_cast<std::size_t> (kHop) * 70);
    rig.feedHops (quiet, 66);
    const auto loud = makeToneInNoise (1000.0, 1.0f, 0.01f, 12u,
                                       static_cast<std::size_t> (kHop) * 4);
    ASSERT_EQ (rig.tap.write (loud.data(), static_cast<std::size_t> (kHop)),
               static_cast<std::size_t> (kHop));
    const auto spectrum = rig.detector.processLatestBlock (rig.tap);
    rig.scorer.beginBlock (kSampleRate);
    ASSERT_NE (spectrum.magnitudes, nullptr);

    const auto detected = rig.analyzer.analyse (spectrum);
    ASSERT_GT (detected.count, 0u);
    const auto b = rig.scorer.scoreCandidateDetailed (detected.candidates[0],
                                                      spectrum.magnitudes, {});
    EXPECT_FLOAT_EQ (b.score, b.pNorm * b.rNorm * b.mNorm * b.penalty);
    EXPECT_FLOAT_EQ (rig.scorer.scoreCandidate (detected.candidates[0],
                                                spectrum.magnitudes, {}), b.score);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --config Release 2>&1 | tail -5`
Expected: compile error — `'riseRatio': is not a member of 'CandidateScorer::ScoreBreakdown'`.

- [ ] **Step 3: Add the field in `src/dsp/CandidateScorer.h`**

Replace the `ScoreBreakdown` body (lines 84-89) with:

```cpp
        float rawPeakiness = 0.0f;
        float pNorm = 0.0f, rNorm = 0.0f, mNorm = 0.0f;
        // Lane G (spec 4.2): the RAW rise ratio mag_now / mag_ref, before any
        // normalisation. rNorm above saturates at rise 1.5, so it cannot
        // distinguish a howl creeping up from one that jumped 12 dB in a
        // quarter second -- and that distinction is what decides whether a
        // notch starts at -6 or -12 dB. The neutral value is 1.0, meaning "no
        // measurable rise": BOTH the no-history branch and the
        // history-too-young branch report it, so an unknown rise can never buy
        // a deeper starting notch.
        float riseRatio = 1.0f;
        float penalty = 1.0f;
        float score = 0.0f;
        const float* refFrame = nullptr;
        double refAgeMs = 0.0;
```

- [ ] **Step 4: Assign it in `src/dsp/CandidateScorer.cpp`**

In the rise branch, the no-history case (lines 77-80):

```cpp
    float rNorm = 0.0f;
    if (historyCount_ == 0)
    {
        rNorm = 1.0f;
        out.riseRatio = 1.0f;   // lane G: nothing to compare against yet
    }
    else
    {
```

and inside `if (reference != nullptr)` (line 98), right after `rise` is computed:

```cpp
            const float was  = std::max (reference[candidate.bin], 1e-12f);
            const float rise = magnitudes[candidate.bin] / was;
            out.riseRatio = rise;   // lane G: the raw ratio, unsaturated
            rNorm = (rise - 1.0f) / 0.5f;
            rNorm = std::min (std::max (rNorm, 0.0f), 1.0f);
```

Leave the "history too young" path alone — `out.riseRatio` keeps its 1.0 default there, which is the documented neutral, and the comment at line 108 already explains that branch.

- [ ] **Step 5: Run the scorer tests**

Run:
```
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
cd build && ctest -C Release -R CandidateScorer --output-on-failure
```
Expected: PASS.

- [ ] **Step 6: Run the full suite**

Run: `cd build && ctest -C Release`
Expected: `100% tests passed` (472). Every pre-existing scorer AND controller test must be untouched — if a placement test moved, the field was not additive and the change is wrong.

- [ ] **Step 7: Commit**

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add src/dsp/CandidateScorer.h src/dsp/CandidateScorer.cpp tests/test_candidatescorer.cpp
```
```bash
git commit -m "feat(dsp): expose the raw riseRatio in ScoreBreakdown (score unchanged)"
```

---

### Task 4: the depth ladder in `NotchController` — model fields, clamp, `pushRetuneLocked`

**Mức level dự kiến (spec §3):** **0 dB** for detector placements — nothing calls the retune path yet (Tasks 5–7 do). The one behaviour change that ships here is Q12's clamp: a preset asking for a depth deeper than −24 dB (say −40) used to be adopted verbatim and is now cut to **−24 dB**, i.e. such a notch becomes **up to 16 dB shallower** than before. No shipped preset does this (`presets/Speech.json` is −18, `presets/Music.json` is −10), so only a hand-edited file is affected — and it now gets a log line saying so.

**Files:**
- Modify: `src/app/NotchController.h:74-95` (constants), `:180-196` (`NotchEvent`), `:207-215` (test accessors), `:268-306` (`SnapshotNotch`, `SnapshotBuffer`), `:320-336` (`ModelNotch`, private helpers), `:467` (override member)
- Modify: `src/app/NotchController.cpp:1-5` (includes), `:117` (ladder helpers), `:135-181` (`setNotchImpl`), `:212` (`pushRetuneLocked`), `:231-262` (`adoptPreset`), `:296-303` (snapshot notch gather), `:423` (test accessors)
- Test: `tests/test_notchcontroller.cpp`

**Interfaces:**
- Consumes: nothing from Tasks 1–3 (this task is model-side only).
- Produces:
  ```cpp
  // NotchController.h, public
  static constexpr double kDepthLadderDb[4] = { -6.0, -12.0, -18.0, -24.0 };
  static constexpr int    kDepthLadderSize  = 4;
  static constexpr double kDepthStepDb      = 6.0;
  static constexpr double kMaxDepthDb       = -24.0;
  static constexpr double kDeepenAfterMs    = 300.0;
  static constexpr float  kSteepRiseRatio   = 2.0f;
  static constexpr double kReleaseFirstMs   = kAutoReleaseMs;   // 30 000
  static constexpr double kReleaseStepMs    = 10000.0;
  static constexpr double kMemoryTtlMs      = 300000.0;
  static constexpr int    kMemoryEntriesPerLane = 16;
  static constexpr float  kRiskFreezeFraction   = 0.55f;

  enum class RetuneReason : std::uint8_t { Deepen, Release, Reclamp, Ceiling };

  // NotchEvent gains Kind::Retune plus:
  RetuneReason retuneReason = RetuneReason::Deepen;
  float        fromDepthDb  = 0.0f;
  float        riseRatio    = 0.0f;   // Set: the RAW rise the placement used (B-5)

  // Q13: the effective ladder is "rungs shallower than the ceiling, then the
  // ceiling". nextDeeperRungDb IS that ladder -- there is no separate
  // quantisation helper, and no `ceilingRungDb`.
  static double nextDeeperRungDb    (double currentDb, double ceilingDb);
  static double nextShallowerRungDb (double currentDb);

  double depthDbForTest   (int channel, int index) const;
  double deepestDbForTest (int channel, int index) const;
  double quietMsForTest   (int channel, int index) const;
  bool   activeForTest    (int channel, int index) const;   // B-3
  bool   retuneForTest    (int channel, int index, double newDepthDb, RetuneReason reason);
  void   setRingRiskOverrideForTest (std::optional<std::pair<bool, float>> override);

  struct SnapshotNotch { float frequency, Q, depthDB, deepestDb; std::uint8_t channel, index; };
  // SnapshotBuffer gains: bool releaseFrozen = false;

  // private, modelMutex_ HELD by the caller
  bool   pushRetuneLocked (int channel, int index, double newDepthDb, RetuneReason reason);
  double ceilingDbFor (const ModelNotch& n) const;   // == the ceilingRung (Q13)
  ```

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_notchcontroller.cpp`, after the last `NotchControllerRingRisk` test (the file ends at line 1681). `Harness`, `FakeClock`, `Recorder`, the `Ev` alias, `pump`, `SineSource`, `NoiseSource` and `primeAndPlace` already exist in that file — use those names.

```cpp
// ===========================================================================
// Lane G (gain-aware notch), spec docs/superpowers/specs/
// 2026-09-06-gain-aware-notch-design.md. The ladder, its clamps, and the
// retune command path.
// ===========================================================================

// RED IF the effective ladder stops being "the rungs shallower than the
// ceiling, then the ceiling itself" (Q13). The shipped presets/Music.json
// asks for -10, which is not a multiple of 6: v2's quantise-to-a-rung rule
// would have capped Music at -6, i.e. 4 dB SHALLOWER than 1.1.3, silently.
TEST (NotchControllerLadder, TheEffectiveLadderEndsOnTheCeilingItself)
{
    using NC = NotchController;
    // Ceiling -24: the whole fixed ladder.
    EXPECT_DOUBLE_EQ (NC::nextDeeperRungDb ( -6.0, -24.0), -12.0);
    EXPECT_DOUBLE_EQ (NC::nextDeeperRungDb (-12.0, -24.0), -18.0);
    EXPECT_DOUBLE_EQ (NC::nextDeeperRungDb (-18.0, -24.0), -24.0);
    EXPECT_DOUBLE_EQ (NC::nextDeeperRungDb (-24.0, -24.0), -24.0);   // saturates

    // Ceiling -10 (presets/Music.json): -6 -> -10, and -10 is the end.
    EXPECT_DOUBLE_EQ (NC::nextDeeperRungDb ( -6.0, -10.0), -10.0);
    EXPECT_DOUBLE_EQ (NC::nextDeeperRungDb (-10.0, -10.0), -10.0);

    // Ceiling -13.7: -6 -> -12 -> -13.7.
    EXPECT_DOUBLE_EQ (NC::nextDeeperRungDb ( -6.0, -13.7), -12.0);
    EXPECT_DOUBLE_EQ (NC::nextDeeperRungDb (-12.0, -13.7), -13.7);
    EXPECT_DOUBLE_EQ (NC::nextDeeperRungDb (-13.7, -13.7), -13.7);

    // Ceiling -18 (presets/Speech.json): -6 -> -12 -> -18.
    EXPECT_DOUBLE_EQ (NC::nextDeeperRungDb ( -6.0, -18.0), -12.0);
    EXPECT_DOUBLE_EQ (NC::nextDeeperRungDb (-12.0, -18.0), -18.0);
    EXPECT_DOUBLE_EQ (NC::nextDeeperRungDb (-18.0, -18.0), -18.0);

    // Ceiling -6: there is no rung shallower than -6, so the ladder is one
    // rung long and a notch there NEVER deepens.
    EXPECT_DOUBLE_EQ (NC::nextDeeperRungDb ( -6.0,  -6.0),  -6.0);

    // An odd Manual depth resolves in the direction of travel, still capped.
    EXPECT_DOUBLE_EQ (NC::nextDeeperRungDb ( -3.0, -24.0),  -6.0);
    EXPECT_DOUBLE_EQ (NC::nextDeeperRungDb ( -9.0, -24.0), -12.0);
}

// RED IF a release step can jump more than one rung, or past -6. Q13 needs no
// ceiling here: releasing is always toward a FIXED rung, and the two off-rung
// depths the ladder can hold -- an odd ceiling, an odd Preset/Manual depth --
// both resolve to the nearest fixed rung above them.
TEST (NotchControllerLadder, NextShallowerIsExactlyOneRungAndStopsAtMinus6)
{
    EXPECT_DOUBLE_EQ (NotchController::nextShallowerRungDb (-24.0), -18.0);
    EXPECT_DOUBLE_EQ (NotchController::nextShallowerRungDb (-18.0), -12.0);
    EXPECT_DOUBLE_EQ (NotchController::nextShallowerRungDb (-12.0),  -6.0);
    EXPECT_DOUBLE_EQ (NotchController::nextShallowerRungDb (-6.0),   -6.0);
    EXPECT_DOUBLE_EQ (NotchController::nextShallowerRungDb (-13.7), -12.0);
    EXPECT_DOUBLE_EQ (NotchController::nextShallowerRungDb (-10.0),  -6.0);  // Music's ceiling
    EXPECT_DOUBLE_EQ (NotchController::nextShallowerRungDb (-9.0),   -6.0);  // odd preset depth
}

// RED IF setNotchImpl stops re-initialising the ladder fields when a slot is
// REUSED (B-2). pushClearLocked only lowers `active`, so without this a manual
// -6 dB notch inherits the -24 dB deepestDb of the previous tenant and gets
// reclamped to -24 on its first reinforce.
TEST (NotchControllerLadder, ReusingASlotResetsEveryLadderField)
{
    Harness h;
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -24.0,
                                        NotchController::Origin::Detector));
    EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, 0), -24.0);

    h.controller.clearNotch (0, 0, NotchController::ClearReason::Manual);
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -6.0,
                                        NotchController::Origin::Manual));

    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0),   -6.0);
    EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, 0), -6.0);
    EXPECT_DOUBLE_EQ (h.controller.quietMsForTest (0, 0),    0.0);
}

// RED IF `activeForTest` is implemented as "depthDB < 0" (B-3). It must read
// ModelNotch::active, because pushClearLocked (NotchController.cpp:195-212)
// lowers ONLY `active` -- the Clear event reads n.depthDB at :208, so the
// depth is deliberately retained on a cleared slot. Every liveness probe in
// Tasks 5-8 is built on this accessor; a depth-based one would report every
// slot that has ever held a notch as still active, and the room-memory and
// ceiling tests would pass while asserting nothing.
//
// DO NOT "fix" this by zeroing depthDB in pushClearLocked: that would empty
// the Clear event's depth field, which lane D writes into every notch_clear.
TEST (NotchControllerLadder, ActiveForTestReadsTheFlagNotTheRetainedDepth)
{
    Harness h;
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -18.0,
                                        NotchController::Origin::Detector));
    EXPECT_TRUE (h.controller.activeForTest (0, 0));

    h.controller.clearNotch (0, 0, NotchController::ClearReason::Manual);
    EXPECT_FALSE (h.controller.activeForTest (0, 0));
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -18.0)
        << "a Clear must NOT zero depthDB -- the Clear event reads it";

    EXPECT_FALSE (h.controller.activeForTest (0, 1));   // never touched
}

// RED IF the -24 clamp (Q12) is narrowed to Origin::Detector, or dropped.
// Invariant 1 must hold for EVERY origin, or a hand-edited preset puts a
// -40 dB cut on a PA.
TEST (NotchControllerLadder, DepthDeeperThanMinus24IsClampedForEveryOrigin)
{
    const NotchController::Origin origins[] = { NotchController::Origin::Detector,
                                                NotchController::Origin::Preset,
                                                NotchController::Origin::Manual,
                                                NotchController::Origin::Soundcheck };
    for (auto origin : origins)
    {
        Harness h;
        ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -40.0, origin));
        h.controller.runOnce();

        NotchCommand cmd {};
        ASSERT_EQ (h.commands.read (&cmd, 1), 1u);
        EXPECT_FLOAT_EQ (cmd.depthDB, -24.0f);
        EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -24.0);
        EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, 0), -24.0);
    }
}

// RED IF adoptPreset stops clamping. A file is not a trusted source.
TEST (NotchControllerLadder, AdoptPresetClampsADeepPresetNotch)
{
    Harness h;
    PresetNotch p;
    p.index = 0; p.freq = 1000.0; p.Q = 30.0; p.depthDB = -40.0;
    EXPECT_EQ (h.controller.adoptPreset ({ p }), 1);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -24.0);
}

// RED IF pushRetuneLocked stops sending the STORED freq/Q (m-2). Sending the
// live notchQ_ instead would differ by a bit whenever the operator had moved
// the Q slider after placement, and NotchChain would take the reset path --
// which is a click.
TEST (NotchControllerLadder, RetuneResendsTheStoredFrequencyAndQ)
{
    Harness h;
    ASSERT_TRUE (h.controller.setNotch (0, 2, 987.0, 17.5, -6.0,
                                        NotchController::Origin::Detector));
    h.controller.setNotchDefaults (44.0, -24.0);   // move the live defaults away
    ASSERT_TRUE (h.controller.retuneForTest (0, 2, -12.0,
                                             NotchController::RetuneReason::Deepen));
    h.controller.runOnce();

    NotchCommand cmd {};
    ASSERT_EQ (h.commands.read (&cmd, 1), 1u);   // the original Set
    ASSERT_EQ (h.commands.read (&cmd, 1), 1u);   // the retune
    EXPECT_EQ (cmd.type, NotchCommandType::Set);
    EXPECT_EQ (cmd.channel, 0);
    EXPECT_EQ (cmd.index, 2);
    EXPECT_FLOAT_EQ (cmd.frequency, 987.0f);
    EXPECT_FLOAT_EQ (cmd.Q, 17.5f);
    EXPECT_FLOAT_EQ (cmd.depthDB, -12.0f);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 2), -12.0);
}

// RED IF a retune overwrites lockedAtMs (B-1). lockedAtMs is the age label
// lane D writes into every notch_clear; resetting it on each retune would make
// a notch that lived 40 s report a 300 ms life.
TEST (NotchControllerLadder, RetuneDoesNotResetTheNotchesAge)
{
    Recorder r; Harness h; h.controller.setEventSink (r.sink());
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -6.0,
                                        NotchController::Origin::Detector));

    std::vector<float> hop (512, 0.1f);
    for (int i = 0; i < 200; ++i) {          // 200 * 5 ms = 1 s of live time
        h.tap.write (hop.data(), hop.size());
        h.clock.advance (5.0);
        h.controller.runOnce();
    }
    ASSERT_TRUE (h.controller.retuneForTest (0, 0, -12.0,
                                             NotchController::RetuneReason::Deepen));
    ASSERT_TRUE (h.controller.retuneForTest (0, 0, -18.0,
                                             NotchController::RetuneReason::Deepen));
    h.controller.clearNotch (0, 0, NotchController::ClearReason::Manual);
    h.controller.runOnce();

    const auto clears = r.clears();
    ASSERT_EQ (clears.size(), 1u);
    EXPECT_GT (clears[0].ageMs, 900.0) << "age was measured from the last retune, not the placement";
    EXPECT_FLOAT_EQ (clears[0].depthDb, -18.0f);   // the depth it was actually running
}

// RED IF a Retune event stops carrying where it came FROM, or starts
// allocating a SpectralContext (a retune is not a placement decision).
TEST (NotchControllerLadder, RetuneEventCarriesFromDepthReasonAndNoContext)
{
    Recorder r; Harness h; h.controller.setEventSink (r.sink());
    ASSERT_TRUE (h.controller.setNotch (0, 1, 1000.0, 30.0, -6.0,
                                        NotchController::Origin::Detector));
    ASSERT_TRUE (h.controller.retuneForTest (0, 1, -18.0,
                                             NotchController::RetuneReason::Reclamp));
    h.controller.runOnce();

    const Ev* ret = nullptr;
    for (const auto& e : r.events)
        if (e.kind == Ev::Kind::Retune) { ret = &e; break; }
    ASSERT_NE (ret, nullptr);
    EXPECT_EQ (ret->lane, 0);
    EXPECT_EQ (ret->index, 1);
    EXPECT_FLOAT_EQ (ret->fromDepthDb, -6.0f);
    EXPECT_FLOAT_EQ (ret->depthDb, -18.0f);
    EXPECT_EQ (ret->retuneReason, NotchController::RetuneReason::Reclamp);
    EXPECT_EQ (ret->origin, NotchController::Origin::Detector);
    EXPECT_EQ (ret->ctx, nullptr);
    EXPECT_FALSE (ret->hasScore);
}

// RED IF pushRetuneLocked drops any of its five validate-before-send
// predicates, or the -24 floor. A refused retune must change NOTHING.
TEST (NotchControllerLadder, RetuneRefusesOutOfRangeDepthAndChangesNothing)
{
    Harness h;
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -12.0,
                                        NotchController::Origin::Detector));
    h.controller.runOnce();
    NotchCommand drained {};
    while (h.commands.read (&drained, 1) == 1) {}

    EXPECT_FALSE (h.controller.retuneForTest (0, 0,  +3.0,
                                              NotchController::RetuneReason::Deepen));
    EXPECT_FALSE (h.controller.retuneForTest (0, 0, -40.0,
                                              NotchController::RetuneReason::Deepen));
    EXPECT_FALSE (h.controller.retuneForTest (0, 5, -18.0,   // slot not active
                                              NotchController::RetuneReason::Deepen));
    h.controller.runOnce();

    EXPECT_EQ (h.commands.getAvailableRead(), 0u);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -12.0);
}

// RED IF the snapshot stops carrying deepestDb -- savePreset (Q11) reads it,
// and a notch resting at -6 while the room needed -18 would be saved as -6.
TEST (NotchControllerLadder, SnapshotCarriesTheDeepestDepthTheNotchEverHeld)
{
    Harness h;
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -18.0,
                                        NotchController::Origin::Detector));
    ASSERT_TRUE (h.controller.retuneForTest (0, 0, -6.0,
                                             NotchController::RetuneReason::Release));

    std::vector<float> hop (512, 0.25f);
    h.tap.write (hop.data(), hop.size());
    h.controller.runOnce();

    NotchController::SnapshotBuffer snap;
    h.controller.copySnapshot (snap);
    ASSERT_EQ (snap.notchCount, 1u);
    EXPECT_FLOAT_EQ (snap.notches[0].depthDB,   -6.0f);
    EXPECT_FLOAT_EQ (snap.notches[0].deepestDb, -18.0f);
    EXPECT_FALSE (snap.releaseFrozen);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --config Release 2>&1 | tail -8`
Expected: compile errors — `nextDeeperRungDb`, `nextShallowerRungDb`, `deepestDbForTest`, `activeForTest`, `retuneForTest`, `RetuneReason`, `Kind::Retune`, `fromDepthDb`, `riseRatio`, `SnapshotNotch::deepestDb` and `SnapshotBuffer::releaseFrozen` all undeclared.

- [ ] **Step 3: Extend `src/app/NotchController.h`**

Add `#include <optional>` and `#include <utility>` next to the existing includes (lines 42-48).

After `static constexpr double kDefaultNotchDepthDb = -18.0;` (line 95), add the lane-G constant block:

```cpp
    // === Lane G: the depth ladder (spec 4.9). Fixed for 1.2.0 and
    // deliberately NOT exposed on the GUI -- these are the numbers the
    // ladder's behaviour was reasoned about with, and a slider on any of them
    // turns every future bug report into "which value was it on?".
    //
    // A Detector notch only ever STANDS on a rung. Deeper == more negative.
    static constexpr double kDepthLadderDb[4] = { -6.0, -12.0, -18.0, -24.0 };
    static constexpr int    kDepthLadderSize  = 4;
    static constexpr double kDepthStepDb      = 6.0;
    // Invariant 1: no Set this controller emits may be deeper than this, for
    // ANY Origin (Q12). -24 dB already kills any howl this app can hear.
    static constexpr double kMaxDepthDb       = -24.0;
    // How long a rung must hold before the ladder buys the next one down (Q2).
    // Measured against liveMs_, which advances once per runOnce AFTER the
    // drain loop, so the gate cannot fire twice inside one drain.
    static constexpr double kDeepenAfterMs    = 300.0;
    // A candidate whose RAW rise ratio is at least this starts on the second
    // rung (Q6). Magnitudes are amplitudes, so 2.0 == +6 dB over the rise
    // window. rNorm cannot express this: it saturates at 1.5.
    static constexpr float  kSteepRiseRatio   = 2.0f;
    // Release ladder (Q3): the FIRST rung costs 30 s of quiet, every rung
    // after it 10 s. kAutoReleaseMs keeps its name and value as the first step.
    static constexpr double kReleaseFirstMs   = kAutoReleaseMs;
    static constexpr double kReleaseStepMs    = 10000.0;
    // "Room memory" (Q6/Q10): a howl returning to the SAME BIN within this
    // window restarts at the depth it needed last time.
    static constexpr double kMemoryTtlMs      = 300000.0;
    static constexpr int    kMemoryEntriesPerLane = 16;
    // Release-clock freeze (Q4). The SAME fraction the GUI's RISING band uses
    // (gui::SpectrumView::kRingRiskRisingFraction, SpectrumView.h:239) -- one
    // constant, not two that can drift apart. Frozen at
    // score >= 0.55 x CandidateScorer::kConfirmScore = 0.385.
    static constexpr float  kRiskFreezeFraction = 0.55f;

    // Why a notch's depth moved. Lane D writes it into the session log as
    // `reason` on a notch_retune line.
    enum class RetuneReason : std::uint8_t { Deepen, Release, Reclamp, Ceiling };

    // Ladder arithmetic. Pure and static, so it is testable without a rig.
    //
    // Q13: the EFFECTIVE ladder is the fixed rungs SHALLOWER than the ceiling,
    // plus the ceiling itself as the last rung -- so ceiling -10 (the shipped
    // presets/Music.json) gives -6 -> -10, and ceiling -6 gives a one-rung
    // ladder that never deepens. The `ceilingRung` IS the ceiling value and
    // may be an odd number: it is the ONLY place a Detector notch stands off a
    // fixed rung. There is deliberately no quantisation helper -- v2 had one
    // (`ceilingRungDb`), and quantising -10 down to -6 made Music 4 dB
    // shallower than 1.1.3 with nothing saying so.
    //
    // nextDeeperRungDb: the shallowest fixed rung strictly deeper than
    //   `currentDb`, capped at `ceilingDb`. Saturates at the ceiling.
    // nextShallowerRungDb: the deepest fixed rung strictly shallower than
    //   `currentDb`, saturating at -6. Needs no ceiling -- a release always
    //   moves toward a fixed rung.
    static double nextDeeperRungDb (double currentDb, double ceilingDb);
    static double nextShallowerRungDb (double currentDb);
```

In `NotchEvent` (lines 180-188) replace the `Kind` enum and add the two fields:

```cpp
    struct NotchEvent
    {
        // Retune: a depth change on a notch that stays where it is (lane G).
        // Neither a Set nor a Clear -- MainComponent::notchEventToVar MUST
        // give it its own `ev` name, or tools/logstats.py closes the notch's
        // record at the first 300 ms deepening (B-3).
        enum class Kind : std::uint8_t { Set, Clear, Retune };
        Kind  kind = Kind::Set;
        int   slot = 0, lane = 0, index = 0;
        float hz = 0.0f, q = 0.0f, depthDb = 0.0f;
        Origin      origin = Origin::Detector;              // Set, Retune
        ClearReason reason = ClearReason::Manual;           // Clear
        RetuneReason retuneReason = RetuneReason::Deepen;   // Retune
        float        fromDepthDb  = 0.0f;                   // Retune: the depth it left
        // Set: the RAW rise ratio the placement policy read (B-5). `rise`
        // below is rNorm, which SATURATES at rise 1.5 -- so it cannot tell a
        // 1.6 from a 40, and a test cannot use it to prove that a ramped
        // fixture actually landed in the -6 band rather than merely failing to
        // confirm. One float, filled from pc.breakdown.riseRatio, no
        // allocation, and not written for Clear or Retune.
        float        riseRatio    = 0.0f;
        double ageMs = 0.0;                                 // Clear/Retune: liveMs_ - lockedAtMs
```

`placeConfirmed`'s `scored` event (`.cpp:620-627`) gains one line next to the other breakdown fields:

```cpp
    scored.novelty = pc.breakdown.mNorm; scored.penalty = pc.breakdown.penalty;
    scored.riseRatio = pc.breakdown.riseRatio;   // B-5: the RAW ratio, not rNorm
```

In the test-accessor block, after `void failNextSetNotchOnLaneForTest (int lane);` (line 210):

```cpp
    // TEST ACCESSORS ONLY (lane G). The ladder lives entirely under
    // modelMutex_ and is otherwise observable only through emitted commands,
    // which cannot tell "did not move" from "moved and moved back".
    double depthDbForTest   (int channel, int index) const;
    double deepestDbForTest (int channel, int index) const;
    double quietMsForTest   (int channel, int index) const;
    // B-3: ModelNotch::active, NOT "depthDB < 0". pushClearLocked lowers only
    // `active` and leaves depthDB alone on purpose (the Clear event reads it),
    // so a depth-based liveness probe matches every slot that has ever held a
    // notch. Tasks 5-8 use this accessor for every "is a notch there?" check.
    bool   activeForTest    (int channel, int index) const;
    // Drives pushRetuneLocked the way the detector thread does, taking
    // modelMutex_ exactly once. Tasks 5-7 call the locked helper from loops
    // that already hold it; this seam is what lets the command path be tested
    // before those callers exist.
    bool   retuneForTest (int channel, int index, double newDepthDb, RetuneReason reason);
    // Forces the pair the release freeze reads (spec 4.5 seam, M-6):
    // {valid, score}. nullopt restores the real frameScoreValid_/frameMaxScore_.
    // A static tone cannot hold score >= 0.385 for 30 s -- mNorm is a
    // log-ratio against a 3 s EMA and decays to 0 within seconds -- so the
    // freeze is untestable through audio alone.
    void   setRingRiskOverrideForTest (std::optional<std::pair<bool, float>> override);
```

In `SnapshotNotch` (lines 268-275):

```cpp
    struct SnapshotNotch
    {
        float frequency = 0.0f;
        float Q         = 0.0f;
        float depthDB   = 0.0f;   // the depth RUNNING right now
        // Lane G (Q11): the deepest rung this notch has ever stood on.
        // savePreset writes THIS rather than depthDB, so a preset saved while
        // the room is quiet still records what the room NEEDED, not what the
        // release ladder had wound back to.
        float deepestDb = 0.0f;
        std::uint8_t channel = 0;
        std::uint8_t index   = 0;
    };
```

In `SnapshotBuffer`, after `float ringRiskThreshold = 0.0f;` (line 305):

```cpp
        // Lane G (Q9): was the release clock frozen on the most recent tick?
        // Published for the GUI and the log to use LATER -- 1.2.0 draws
        // nothing with it. The RING RISK chip cannot stand in for it: it holds
        // for 750 ms and follows displayedSlot_ only, so it can read RISING
        // while the clock is running again, and a slot that is not displayed
        // can be frozen with nothing on screen saying so (M-8).
        bool  releaseFrozen = false;
```

In `ModelNotch` (lines 320-329) add the five fields:

```cpp
    struct ModelNotch
    {
        double frequency    = 0.0;
        double Q            = 0.0;
        double depthDB      = 0.0;
        double lockedAtMs   = 0.0;
        double lastDetectedMs = 0.0;
        Origin origin       = Origin::Detector;
        bool   active       = false;

        // --- lane G ladder state (spec 4.2) -------------------------------
        // All five are re-initialised by setNotchImpl, the ONE place a slot
        // becomes active, for every Origin and every path (placeConfirmed,
        // adoptPreset, the GUI's setNotch, the partial-apply unwind).
        // pushClearLocked only lowers `active`, so a reused slot would
        // otherwise inherit the previous tenant's ladder (B-2).

        // Deepest rung held since placement -- also where a reclamp jumps to.
        double deepestDb        = 0.0;
        // liveMs_ at the last depth change (deepen, release, reclamp, ceiling).
        double stageChangedAtMs = 0.0;
        // ACCUMULATED quiet time since the last depth change or reinforce, in
        // live ms. A counter rather than a timestamp precisely so the freeze
        // can stop it without losing what it had banked.
        double quietMs          = 0.0;
        // Rungs released from deepestDb. 0 == not releasing. An integer count
        // instead of comparing doubles (m-3).
        int    releasedSteps    = 0;
        // This notch's own ceiling. Preset/Manual/Soundcheck: the depth the
        // caller asked for -- the slider must not drag a notch a human or a
        // file set explicitly (Q8). Detector: NaN, meaning "follow the live
        // slider", resolved by ceilingDbFor().
        double ceilingDb        = 0.0;
    };
```

Declare the private helpers next to `pushClearLocked` (line 336):

```cpp
    void pushClearLocked (int channel, int index, ClearReason reason);
    // modelMutex_ HELD. Re-sends `index` as a Set at a new depth, keeping the
    // notch's stored frequency, Q, lockedAtMs, origin and lastDetectedMs.
    // Applies the same five predicates setNotchImpl does plus the -24 floor,
    // and returns false changing NOTHING when any fails or the slot is not
    // active.
    //
    // It exists because both callers -- the reinforce loop in
    // processSpectrumForDetection and step 3 of runOnce -- already hold
    // modelMutex_, which is NOT recursive: calling setNotch/setNotchImpl from
    // either would deadlock, and setNotchImpl would also stamp a fresh
    // lockedAtMs, destroying lane D's age label (B-1).
    bool pushRetuneLocked (int channel, int index, double newDepthDb, RetuneReason reason);
    // The ceiling this notch obeys: its own, or the LIVE slider for a Detector
    // notch (whose ceilingDb is NaN). Read fresh every tick, so lowering the
    // slider mid-show takes effect (spec 7).
    double ceilingDbFor (const ModelNotch& n) const;
```

And the override member, next to `failSetNotchLaneForTest_` (line 467):

```cpp
    std::optional<std::pair<bool, float>> ringRiskOverrideForTest_;
```

- [ ] **Step 4: Implement in `src/app/NotchController.cpp`**

Add `#include <optional>` and `#include <utility>` to the include block (lines 3-5).

Add the ladder helpers after `setWidth` (line 117):

```cpp
// --- Lane G ladder arithmetic (spec 4.1, Q13). Pure: no state, no locks. ---

double NotchController::nextDeeperRungDb (double currentDb, double ceilingDb)
{
    // The shallowest FIXED rung strictly deeper than `currentDb` ...
    double next = kDepthLadderDb[kDepthLadderSize - 1];
    for (int i = 0; i < kDepthLadderSize; ++i)
        if (kDepthLadderDb[i] < currentDb)
        {
            next = kDepthLadderDb[i];
            break;
        }
    // ... capped at the ceiling, which is the effective ladder's LAST rung
    // (Q13). std::max picks the SHALLOWER of the two, because deeper is more
    // negative: ceiling -10 turns "-6 -> -12" into "-6 -> -10".
    //
    // A caller must already have established `currentDb > ceilingDb` -- at or
    // past the ceiling this returns `currentDb` unchanged, which every caller
    // treats as "nothing to do" rather than as a step.
    return std::max (next, ceilingDb);
}

double NotchController::nextShallowerRungDb (double currentDb)
{
    // The deepest rung strictly shallower than `currentDb`, saturating at the
    // ladder top. An odd Preset/Manual depth resolves upward.
    for (int i = kDepthLadderSize - 1; i >= 0; --i)
        if (kDepthLadderDb[i] > currentDb)
            return kDepthLadderDb[i];
    return kDepthLadderDb[0];
}

// The `ceilingRung` of spec 4.1 -- under Q13 that IS the ceiling value, so
// there is nothing to quantise. A Detector notch (ceilingDb == NaN) follows
// the LIVE slider; everything else carries its own (Q8).
double NotchController::ceilingDbFor (const ModelNotch& n) const
{
    return std::isnan (n.ceilingDb) ? notchDepthDb_.load (std::memory_order_relaxed)
                                    : n.ceilingDb;
}
```

Rewrite `setNotchImpl` (lines 135-181) — the clamp goes in before validation, the five ladder fields inside the lock:

```cpp
bool NotchController::setNotchImpl (int channel, int index, double frequency, double Q, double depthDB,
                                    Origin origin, const NotchEvent* scored)
{
    // width gates the policy surface; internal fan-out loops never exceed it.
    if (channel < 0 || channel >= width_ || index < 0 || index >= kSlots)
        return false;

    // Q12 / invariant 1: nothing deeper than the ladder floor leaves this
    // controller, whatever asked for it -- a preset file is not a trusted
    // source, and -24 dB already kills any howl this app can hear. Applied
    // BEFORE validation, so a positive depth is still refused below.
    const double depth = std::max (depthDB, kMaxDepthDb);

    // Same predicates Biquad::setNotchFilter applies (see header comment).
    const double sampleRate = lanes_[0].detector.getSampleRate();
    if (! (sampleRate > 0.0))                       return false;
    if (! (Q > 0.0))                                return false;
    if (! (frequency > 0.0 && frequency < sampleRate * 0.5))
                                                    return false;
    if (! (depth <= 0.0))                           return false;  // positive depth would BOOST

    if (failSetNotchLaneForTest_.load (std::memory_order_relaxed) == channel)
    {
        failSetNotchLaneForTest_.store (-1, std::memory_order_relaxed);
        return false;   // TEST ONLY: forces the partial-apply unwind path
    }

    const NotchCommand cmd { NotchCommandType::Set,
                             (std::uint8_t) channel, (std::uint8_t) index,
                             (float) frequency, (float) Q, (float) depth,
                             slotId_ };

    {
        const std::lock_guard<std::mutex> lock (modelMutex_);
        auto& n = model_[slotOf (channel, index)];
        n.frequency      = frequency;
        n.Q              = Q;
        n.depthDB        = depth;
        n.origin         = origin;
        n.active         = true;
        n.lockedAtMs     = liveMs_;
        n.lastDetectedMs = liveMs_;
        // Lane G (B-2): the one place a slot becomes active is the one place
        // the ladder can be trusted to start clean.
        n.deepestDb        = depth;
        n.stageChangedAtMs = liveMs_;
        n.quietMs          = 0.0;
        n.releasedSteps    = 0;
        n.ceilingDb        = (origin == Origin::Detector)
                                 ? std::numeric_limits<double>::quiet_NaN()
                                 : depth;
        outbox_.push_back (cmd);

        NotchEvent ev = scored != nullptr ? *scored : NotchEvent {};
        ev.kind = NotchEvent::Kind::Set;
        ev.slot = slotId_; ev.lane = channel; ev.index = index;
        ev.hz = (float) frequency; ev.q = (float) Q; ev.depthDb = (float) depth;
        ev.origin = origin;
        pushEventLocked (std::move (ev));
    }
    return true;
}
```

Add `pushRetuneLocked` immediately after `pushClearLocked` (line 212):

```cpp
bool NotchController::pushRetuneLocked (int channel, int index, double newDepthDb,
                                        RetuneReason reason)
{
    auto& n = model_[slotOf (channel, index)];
    if (! n.active)
        return false;

    // Validate-before-send: the same five predicates as setNotchImpl plus the
    // ladder floor (spec 4.4). Nothing is touched when any of them fails -- a
    // refused retune leaves the model AND the chain on the depth they already
    // agreed on, which is the only state where they cannot disagree.
    const double sampleRate = lanes_[0].detector.getSampleRate();
    if (! (sampleRate > 0.0))                                    return false;
    if (! (n.Q > 0.0))                                           return false;
    if (! (n.frequency > 0.0 && n.frequency < sampleRate * 0.5)) return false;
    if (! (newDepthDb <= 0.0))                                   return false;
    if (! (newDepthDb >= kMaxDepthDb))                           return false;

    // freq and Q come from the STORED notch, never from notchQ_ (m-2): a Q
    // differing by one bit sends NotchChain::setNotch down the reset path, and
    // a reset mid-signal is the click this whole lane exists to avoid.
    outbox_.push_back ({ NotchCommandType::Set,
                         (std::uint8_t) channel, (std::uint8_t) index,
                         (float) n.frequency, (float) n.Q, (float) newDepthDb,
                         slotId_ });

    NotchEvent ev;
    ev.kind = NotchEvent::Kind::Retune;
    ev.slot = slotId_; ev.lane = channel; ev.index = index;
    ev.hz = (float) n.frequency; ev.q = (float) n.Q;
    ev.fromDepthDb = (float) n.depthDB;
    ev.depthDb     = (float) newDepthDb;
    ev.origin = n.origin;
    ev.retuneReason = reason;
    // Age from PLACEMENT, like a Clear's. lockedAtMs, origin and
    // lastDetectedMs are deliberately left alone: this is the same notch, and
    // lane D's labels are keyed to when it was placed (B-1).
    ev.ageMs = liveMs_ - n.lockedAtMs;
    pushEventLocked (std::move (ev));

    n.depthDB = newDepthDb;
    return true;
}
```

In `adoptPreset` (lines 231-262), count and log the clamps. Replace the opening line, and count inside the apply loop:

```cpp
    int adopted = 0, skipped = 0, clamped = 0;
```

```cpp
        int applied = 0;
        for (int lane = firstLane; lane <= lastLane; ++lane)
            if (setNotch (lane, p.index, p.freq, p.Q, p.depthDB, Origin::Preset))
            {
                ++applied;
                // Q12: setNotchImpl does the clamping; count it HERE so the
                // operator is told a hand-edited file asked for more than the
                // app allows, instead of quietly getting a different filter
                // than the file names.
                //
                // M-5: counted only when the notch was ACTUALLY adopted and
                // the clamp actually moved the value. Counting at the top of
                // the loop body -- above the `firstLane >= width_` skip at
                // .cpp:242-246 and above setNotchImpl's own validation --
                // reports clamps on notches that were never applied at all: a
                // lane-1 notch on a mono slot, a freq past Nyquist, a positive
                // depth. `lane == firstLane` makes it one count per
                // PresetNotch, not one per lane of a mirrored stereo pair.
                if (lane == firstLane && p.depthDB < kMaxDepthDb)
                    ++clamped;
            }
```

```cpp
    if (clamped > 0)
        juce::Logger::writeToLog ("preset: clamped " + juce::String (clamped)
                                  + " notch depth(s) to " + juce::String (kMaxDepthDb, 1)
                                  + " dB (slot " + juce::String (slotId_) + ")");
    if (skippedOut != nullptr)
        *skippedOut = skipped;
    return adopted;
```

In `runOnce`'s notch gather (lines 300-303) carry `deepestDb` into the snapshot:

```cpp
                    if (! n.active) continue;
                    notchList[notchCount++] = { (float) n.frequency, (float) n.Q,
                                                (float) n.depthDB, (float) n.deepestDb,
                                                (std::uint8_t) c, (std::uint8_t) i };
```

Add the accessors next to `liveMsForTest` (line 423):

```cpp
double NotchController::depthDbForTest (int channel, int index) const
{
    const std::lock_guard<std::mutex> lock (modelMutex_);
    return model_[slotOf (std::clamp (channel, 0, kChannels - 1),
                          std::clamp (index, 0, kSlots - 1))].depthDB;
}

double NotchController::deepestDbForTest (int channel, int index) const
{
    const std::lock_guard<std::mutex> lock (modelMutex_);
    return model_[slotOf (std::clamp (channel, 0, kChannels - 1),
                          std::clamp (index, 0, kSlots - 1))].deepestDb;
}

double NotchController::quietMsForTest (int channel, int index) const
{
    const std::lock_guard<std::mutex> lock (modelMutex_);
    return model_[slotOf (std::clamp (channel, 0, kChannels - 1),
                          std::clamp (index, 0, kSlots - 1))].quietMs;
}

bool NotchController::activeForTest (int channel, int index) const
{
    // B-3: the FLAG. pushClearLocked (.cpp:195-212) lowers `active` and leaves
    // frequency/Q/depthDB standing -- the Clear event reads n.depthDB at :208 --
    // so `depthDB < 0` is true for every slot that has ever held a notch.
    const std::lock_guard<std::mutex> lock (modelMutex_);
    return model_[slotOf (std::clamp (channel, 0, kChannels - 1),
                          std::clamp (index, 0, kSlots - 1))].active;
}

bool NotchController::retuneForTest (int channel, int index, double newDepthDb,
                                     RetuneReason reason)
{
    if (channel < 0 || channel >= kChannels || index < 0 || index >= kSlots)
        return false;
    const std::lock_guard<std::mutex> lock (modelMutex_);
    return pushRetuneLocked (channel, index, newDepthDb, reason);
}

void NotchController::setRingRiskOverrideForTest (std::optional<std::pair<bool, float>> override)
{
    // Detector-thread state, written from the test thread with the detector
    // STOPPED -- same precondition as setWidth() and setEventSink().
    ringRiskOverrideForTest_ = override;
}
```

- [ ] **Step 5: Run the controller tests**

Run:
```
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
cd build && ctest -C Release -R NotchController --output-on-failure
```
Expected: PASS, including the eleven new `NotchControllerLadder.*` tests (the two ladder-arithmetic tests, `ActiveForTestReadsTheFlagNotTheRetainedDepth`, and the eight model/command tests).

- [ ] **Step 6: Run the full suite**

Run: `cd build && ctest -C Release`
Expected: `100% tests passed` (483). No pre-existing test may move: nothing calls `pushRetuneLocked` outside the seam yet, and the only shipped behaviour change is the −24 clamp, which no shipped preset triggers.

- [ ] **Step 7: Commit**

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add src/app/NotchController.h src/app/NotchController.cpp tests/test_notchcontroller.cpp
```
```bash
git commit -m "feat(controller): depth ladder state, -24 clamp for every origin, pushRetuneLocked"
```

---

### Task 5: `placeConfirmed` places SHALLOW — −6 dB, or −12 on a steep rise

**Mức level dự kiến (spec §3):** this is the first task the testers hear. A detector notch is now placed at **−6 dB**, or **−12 dB** when the candidate's raw rise ratio is ≥ 2.0 (+6 dB over the rise window) — instead of today's fixed **−18 dB**. At the notch's own bin that is **12 dB shallower** (or 6 dB) at the instant of placement; Task 6 walks it back down at 6 dB per 300 ms while the bin keeps ringing, so a fast-building howl reaches −18 about **300–600 ms later than 1.1.3 does**. Outside the bin: **0 dB**. A room that howls explosively will therefore be audibly howling for up to ~0.6 s longer than on 1.1.3 — **this is the row testers must try at low volume first.** Soundcheck notches are exempt (KD-7): they still take the full slider depth on the first block.

**Files:**
- Modify: `src/app/NotchController.cpp:580-632` (`placeConfirmed`: the depth choice moves BELOW the index lookup — see Step 4) and `:693-699` (the `[detect]` log line)
- Modify: `tests/test_notchcontroller.cpp:349` (fixture: `RampSineSource` + `primeAndPlaceSlowly` go AFTER `pump` closes — B-6), `:436-438` (the stale `-18` assert), `:875-889` (`SetNotchDefaultsFlowIntoPlacedNotch`)
- Test: `tests/test_notchcontroller.cpp`

**Interfaces:**
- Consumes (Task 3): `pc.breakdown.riseRatio`. Consumes (Task 4): `kDepthLadderDb`, `kSteepRiseRatio`, `NotchEvent::riseRatio`, `activeForTest`, and `setNotchImpl`'s initialisation of `deepestDb` / `ceilingDb`.
- Produces: the placement depths Task 6 deepens from and Task 7 releases from. New test fixture symbol:
  ```cpp
  struct RampSineSource { double freq; float amp; double gainDbPerMs; std::vector<float> hop(); };
  ```

- [ ] **Step 1: Fix the two existing tests the new policy makes wrong (M-12)**

These are pre-existing tests that assert 1.1.3's fixed depth. They are not "broken by the change" — they encoded the old policy, and the spec says what replaces it. Edit them BEFORE writing the new tests so the run in Step 3 is unambiguous.

`tests/test_notchcontroller.cpp:436-438`, inside `PersistentHowlSetsNotchOnBothChannels`:

```cpp
    // Runtime defaults (brief 2026-08-24): Q 30. Depth is now the LADDER's
    // (lane G, spec 4.3), not notchDepthDb_: SineSource switches its tone on
    // hard, so riseRatio is far past kSteepRiseRatio and the notch starts on
    // the second rung. -18 arrives later, via the deepen path (Task 6).
    EXPECT_FLOAT_EQ (cmd.Q, 30.0f);
    EXPECT_FLOAT_EQ (cmd.depthDB, -12.0f);
```

`tests/test_notchcontroller.cpp:875-889`, `SetNotchDefaultsFlowIntoPlacedNotch` — the Q still flows through; the depth is now a CEILING, not a starting value. Replace the body's last two assertions:

```cpp
TEST (NotchControllerDetection, SetNotchDefaultsFlowIntoPlacedNotch)
{
    Harness h;
    h.controller.setDetectionActive (true);
    h.controller.setNotchDefaults (20.0, -24.0);

    int slot = -1;
    ASSERT_NO_FATAL_FAILURE (primeAndPlace (h, slot));
    ASSERT_GE (slot, 0);

    NotchCommand cmd {};
    ASSERT_EQ (h.commands.read (&cmd, 1), 1u);
    EXPECT_FLOAT_EQ (cmd.Q, 20.0f);
    // Lane G: the depth default is now the CEILING (Q1), not the starting
    // depth. A ceiling of -24 permits the whole ladder, and SineSource's hard
    // start puts the first rung at -12.
    EXPECT_FLOAT_EQ (cmd.depthDB, -12.0f);
    EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, slot), -12.0);
}
```

- [ ] **Step 2: Add `RampSineSource` to the fixture and write the failing tests**

Add to the anonymous namespace in `tests/test_notchcontroller.cpp` **after `pump` closes at line 349** — not after `SineSource` at 327 (B-6). Both new helpers call `NoiseSource` (declared `:329`), `pump` (`:344-349`) and `kWarmupBlocks` (`:308`), so an insertion above `pump` does not compile. Line 349 is also where Task 7 adds `pumpQuietFor`.

**Deriving the ramp slope (B-5).** The +3 dB/250 ms of the spec's example can never
confirm, and the arithmetic says so before a build does:

- `CandidateScorer` scores the rise axis as `rNorm = clamp((rise − 1) / 0.5, 0, 1)`
  (`CandidateScorer.cpp:104-106`), where `rise = mag_now / mag_ref` and the reference
  frame is the newest one at least `0.45 × riseReferenceMs` old (`:83`). With the
  default `kDefaultRiseReferenceMs = 250.0` (`CandidateScorer.h:39`) that floor is
  **112.5 ms**, and frames arrive one per hop (10.667 ms), so the reference is
  **112.5–123.2 ms old**.
- `score` is a PRODUCT of the axes, so `score ≤ rNorm`. Confirming needs
  `score ≥ kConfirmScore = 0.7`, therefore `rNorm ≥ 0.7`, therefore
  `rise ≥ 1 + 0.5 × 0.7 = 1.35`. To have margin, aim for `rNorm` saturated:
  **`rise ≥ 1.5`**, i.e. `20·log10(1.5) = 3.52 dB` over 112.5 ms ⇒ **≥ 0.0313 dB/ms
  ≈ 7.8 dB per 250 ms**.
- The other side: the placement must stay in the −6 band, so
  **`rise < kSteepRiseRatio = 2.0`** at the OLDEST reference the scan can pick,
  123.2 ms ⇒ `< 6.02 dB` over 123.2 ms ⇒ **< 0.0489 dB/ms ≈ 12.2 dB per 250 ms**.
- At +3 dB/250 ms = 0.012 dB/ms the rise over 117 ms is 1.4 dB ⇒ `rise = 1.18` ⇒
  `rNorm = 0.35 < 0.7`. It never confirms — which is why the v1 test would have hung
  on `ASSERT_GE(slot, 0)` rather than failing on the depth.

**Pick +9.5 dB per 250 ms** (`0.038 dB/ms`). At the youngest reference (112.5 ms):
`10^(0.038·112.5/20) = 1.64` ⇒ `rNorm` saturated. At the oldest (123.2 ms):
`10^(0.038·123.2/20) = 1.71 < 2.0` ⇒ still the −6 band. Both bounds hold across the
whole age window, with ~0.14 of headroom below the confirm floor and ~0.29 above the
steep-rise line.

**This number must still be tuned by RUNNING the test (TDD red → green).** The
derivation fixes the two ends; the analyser's window, the noise floor and the
peakiness threshold decide where inside them a real fixture lands. If the test does
not confirm, raise the slope toward 12.2 dB/250 ms; if it places at −12, lower it
toward 7.8. Do not move `kSteepRiseRatio` or `kConfirmScore` to make it pass.

```cpp
// A tone that CREEPS up instead of switching on. SineSource starts at full
// amplitude in one block, so its rise ratio is enormous and every test built
// on it places at -12; the -6 start of spec 4.3 is only observable behind a
// slow build (M-12).
//
// +9.5 dB / 250 ms, derived in the plan (B-5): the scorer's reference frame is
// 112.5-123.2 ms old, so this gives rise 1.64-1.71 -- above the 1.5 that
// saturates rNorm (score is a product, and score >= 0.7 needs rNorm >= 0.7),
// and below kSteepRiseRatio 2.0 so the placement stays in the -6 band. The
// spec's illustrative +3 dB/250 ms gives rise 1.18 => rNorm 0.35 => it never
// confirms at all.
struct RampSineSource
{
    double freq        = 1000.0;
    float  amp         = 0.02f;          // starts just over the noise floor
    double gainDbPerMs = 9.5 / 250.0;
    double nextSample  = 0.0;

    std::vector<float> hop()
    {
        std::vector<float> out ((std::size_t) Detector::kHopSize);
        for (int i = 0; i < Detector::kHopSize; ++i)
        {
            out[(std::size_t) i] = amp * static_cast<float> (
                std::sin (2.0 * kTestPi * freq * nextSample / kTestSr));
            nextSample += 1.0;
        }
        amp = static_cast<float> (std::min (1.0, static_cast<double> (amp)
                                  * std::pow (10.0, gainDbPerMs * kBlockMs / 20.0)));
        return out;
    }
};

// primeAndPlace's shape, driven by a source that ramps. Returns the index of
// the first Set, or -1. Stops at 80 blocks: RampSineSource reaches full scale
// after ~34 dB of gain (~894 ms, ~84 blocks) and stops rising there, so a
// fixture that has not confirmed by then never will -- and a loop that keeps
// pumping a FLAT tone would confirm later on novelty alone and quietly test
// something else. A -1 here means the slope needs retuning, not more blocks.
int primeAndPlaceSlowly (Harness& h)
{
    NoiseSource quiet;
    for (int i = 0; i < kWarmupBlocks; ++i)
        pump (h, quiet.hop());

    RampSineSource tone;
    for (int i = 0; i < 80; ++i)
    {
        pump (h, tone.hop());
        NotchCommand cmd {};
        if (h.commands.read (&cmd, 1) == 1 && cmd.type == NotchCommandType::Set)
            return cmd.index;
    }
    return -1;
}
```

Then append the tests:

```cpp
// RED IF placement goes back to reading notchDepthDb_ directly. A howl that
// creeps up gets the SHALLOWEST rung -- the whole point of lane G is that a
// room which only needs 6 dB does not lose 18 dB of tone (spec 4.3 step 1).
TEST (NotchControllerLadder, ASlowlyRisingHowlIsPlacedAtMinusSix)
{
    Recorder r; Harness h; h.controller.setEventSink (r.sink());
    h.controller.setDetectionActive (true);

    const int slot = primeAndPlaceSlowly (h);
    ASSERT_GE (slot, 0) << "the slow ramp never confirmed -- raise gainDbPerMs "
                           "toward the 12.2 dB/250 ms upper bound (B-5)";
    EXPECT_TRUE (h.controller.activeForTest (0, slot));
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, slot),   -6.0);
    EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, slot), -6.0);

    // B-5: prove the FIXTURE is the thing being tested, not an accident. -6 is
    // also what a fixture that barely confirmed on some other axis would
    // produce, so pin the raw rise inside the band the derivation aimed at:
    // >= 1.5 saturates rNorm (so the confirm was earned on rise), < 2.0 is why
    // the steep-rise branch did not fire. `rise` on the event is rNorm, which
    // saturates at 1.5 and cannot make this distinction -- riseRatio is the
    // raw ratio added in Task 4.
    const Ev* set = nullptr;
    for (const auto& e : r.events)
        if (e.kind == Ev::Kind::Set) { set = &e; break; }
    ASSERT_NE (set, nullptr);
    EXPECT_GE (set->riseRatio, 1.5f) << "the ramp was too slow to confirm on rise";
    EXPECT_LT (set->riseRatio, NotchController::kSteepRiseRatio)
        << "the ramp was steep enough to be a -12 placement";
}

// RED IF the steep-rise jump stops firing (spec 4.3 step 2, Q6). SineSource
// switches a full-scale tone on in one block, so riseRatio >> 2.0.
TEST (NotchControllerLadder, ASteeplyRisingHowlIsPlacedAtMinusTwelve)
{
    Harness h;
    h.controller.setDetectionActive (true);

    int slot = -1;
    ASSERT_NO_FATAL_FAILURE (primeAndPlace (h, slot));
    ASSERT_GE (slot, 0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, slot),   -12.0);
    EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, slot), -12.0);
}

// RED IF the ceiling stops clamping the STARTING depth (spec 4.3 step 4). A
// ceiling of -6 must never let even a steep rise place at -12.
TEST (NotchControllerLadder, ACeilingOfMinusSixCapsEvenASteepRise)
{
    Harness h;
    h.controller.setDetectionActive (true);
    h.controller.setNotchDefaults (30.0, -6.0);

    int slot = -1;
    ASSERT_NO_FATAL_FAILURE (primeAndPlace (h, slot));
    ASSERT_GE (slot, 0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, slot), -6.0);
}

// RED IF the ceiling is quantised down to a fixed rung instead of BEING the
// last rung (Q13). presets/Music.json ships depth -10; under v2's
// `ceilingRungDb` a steep rise there would have placed at -6, i.e. 4 dB
// shallower than 1.1.3, with nothing in the release note saying so.
TEST (NotchControllerLadder, AnOffRungCeilingIsItselfTheDeepestPlacement)
{
    Harness h;
    h.controller.setDetectionActive (true);
    h.controller.setNotchDefaults (30.0, -10.0);   // the shipped Music ceiling

    int slot = -1;
    ASSERT_NO_FATAL_FAILURE (primeAndPlace (h, slot));   // steep: wants -12
    ASSERT_GE (slot, 0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, slot),   -10.0);
    EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, slot), -10.0);
}

// RED IF a Soundcheck placement is dragged onto the ladder. KD-7 exempts
// soundcheck notches from every part of lane G: they are placed at the depth
// the operator asked for, on the first block, and never move.
TEST (NotchControllerLadder, SoundcheckPlacesAtTheFullSliderDepth)
{
    Harness h;
    h.controller.setNotchDefaults (30.0, -18.0);
    h.controller.startSoundcheck();

    int slot = -1;
    ASSERT_NO_FATAL_FAILURE (primeAndPlace (h, slot));
    ASSERT_GE (slot, 0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, slot), -18.0);
}

// RED IF a Detector notch stops following the LIVE slider, or a Preset notch
// starts following it (Q8). Detector: ceilingDb is NaN. Preset: its own depth.
TEST (NotchControllerLadder, PresetNotchesKeepTheirOwnCeilingAndDetectorNotchesFollowTheSlider)
{
    Harness h;
    h.controller.setNotchDefaults (30.0, -24.0);
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -12.0,
                                        NotchController::Origin::Preset));
    // A preset notch is placed AT its own depth and that depth is its ceiling:
    // deepestDb equals it, so nothing below can deepen past it.
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0),   -12.0);
    EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, 0), -12.0);

    h.controller.setDetectionActive (true);
    int slot = -1;
    ASSERT_NO_FATAL_FAILURE (primeAndPlace (h, slot));
    ASSERT_GE (slot, 0);
    ASSERT_NE (slot, 0);
    // The detector notch obeys the -24 slider, so the steep-rise rung stands.
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, slot), -12.0);
}
```

- [ ] **Step 3: Run to verify it fails**

Run: `cmake --build build --config Release && cd build && ctest -C Release -R NotchController --output-on-failure`
Expected: FAIL — `ASlowlyRisingHowlIsPlacedAtMinusSix` and `ASteeplyRisingHowlIsPlacedAtMinusTwelve` both report `-18`, and the two edited tests fail for the same reason.

- [ ] **Step 4: Implement the placement policy in `src/app/NotchController.cpp`**

**B-2 — read this before touching the file.** Today `placeConfirmed` reads the three
`const`s at `:585-587` and only then searches for a free index at `:589-596`. The v1
plan put the depth choice at `:585-587` and had Task 8 insert its room-memory lookup
into it — but the lookup needs `index >= 0` (an entry is CONSUMED, so it must not be
spent on a placement that then bails on a full chain) and needs the bin, which needs
the sample rate the index block already has in scope. So the **whole depth choice
moves BELOW the index lookup**, and Task 8 changes exactly one line of it.

Replace `:585-596` — the three `const` reads AND the index-search block — with:

```cpp
    // soundcheckActive() takes modelMutex_ itself, so it is asked BEFORE the
    // lock below -- never underneath it.
    const Origin origin  = soundcheckActive() ? Origin::Soundcheck : Origin::Detector;
    const double q       = notchQ_.load (std::memory_order_relaxed);
    // Lane G (spec 4.3, Q1): the depth default is now the CEILING, not the
    // depth. A notch is placed as SHALLOW as the policy allows and earns the
    // rest from the reinforce loop, so a room that only needs 6 dB keeps the
    // 12 dB of tone 1.1.3 threw away. Q13: the ceiling need not be a multiple
    // of 6 and is itself the deepest rung -- presets/Music.json ships -10.
    const double ceiling = notchDepthDb_.load (std::memory_order_relaxed);

    int index = -1, firstLane = lane, lastLane = lane;
    {
        const std::lock_guard<std::mutex> lock (modelMutex_);
        if (linkedNow) { index = firstFreeIndexAllLanesLocked(); firstLane = 0; lastLane = width_ - 1; }
        else           { index = firstFreeIndexLocked (lane); }
        // Task 8 hooks the room-memory lookup in HERE -- it needs `index >= 0`
        // (the entry is consumed) and the bin width, and it must run under
        // this same lock. Nothing to do in this task.
    }
    if (index < 0)
        return;   // chain full on the lanes concerned: same outcome as today

    // --- the depth choice (spec 4.3). It lives BELOW the index lookup so that
    // Task 8's step 3 can read the room memory the lookup above provides.
    double depthDb = kDepthLadderDb[0];                       // step 1: -6
    if (pc.breakdown.riseRatio >= kSteepRiseRatio)            // step 2: +6 dB or more
        depthDb = kDepthLadderDb[1];                          //         over the rise
                                                              //         window -> -12
    // step 3 (room memory) is inserted HERE by Task 8, and nowhere else.

    // step 4: never deeper than the ceiling, which under Q13 IS the deepest
    // rung. max() picks the SHALLOWER of the two because deeper is more
    // negative: ceiling -10 turns a steep-rise -12 into -10.
    depthDb = std::max (depthDb, ceiling);

    // KD-7, and this line stays LAST through Task 8: soundcheck has no ladder.
    // Those notches never deepen, never release and never reclamp, so starting
    // them shallow -- or letting a remembered depth decide for them -- would
    // leave a howl the operator explicitly asked to lock permanently under-cut.
    // Moving or dropping it reds SoundcheckPlacesAtTheFullSliderDepth and
    // SoundcheckNotchesNeverDeepen.
    if (origin == Origin::Soundcheck)
        depthDb = ceiling;
```

Then the `scored` event gains the raw rise (Task 4's `NotchEvent::riseRatio`, line 627):

```cpp
    scored.novelty = pc.breakdown.mNorm; scored.penalty = pc.breakdown.penalty;
    scored.riseRatio = pc.breakdown.riseRatio;
```

and the `setNotchImpl` call at line 631 passes the chosen depth:

```cpp
    int applied = 0;
    for (int l = firstLane; l <= lastLane; ++l)
        if (setNotchImpl (l, index, cand.frequencyHz, q, depthDb, origin, &scored))
            ++applied;
```

And the `depth=` field of the `[detect]` log line (line 698):

```cpp
        + " Q=" + juce::String (q, 1) + " depth=" + juce::String (depthDb, 1)
```

- [ ] **Step 5: Run the controller tests**

Run: `cmake --build build --config Release && cd build && ctest -C Release -R NotchController --output-on-failure`
Expected: PASS. The release ladder does not exist yet (Task 7), so every auto-release test still clears at 30 s exactly as it does today — nothing in this task moves their timing.

- [ ] **Step 6: Run the full suite**

Run: `cd build && ctest -C Release`
Expected: `100% tests passed` (489).

- [ ] **Step 7: Commit**

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add src/app/NotchController.cpp tests/test_notchcontroller.cpp
```
```bash
git commit -m "feat(controller): place notches at -6 dB (-12 on a steep rise) under the preset ceiling"
```

---

### Task 6: deepen and reclamp inside the reinforce loop

**Mức level dự kiến (spec §3):** a notch whose bin is STILL over the peakiness threshold gains one rung — **6 dB deeper** — every 300 ms of live time, ramped over 10 ms, until it reaches the ceiling's rung or the bin goes quiet. So a fast howl walks −12 → −18 in ~300 ms and −18 → −24 (ceiling permitting) in ~600 ms; the level at the bin ends up **the same as or deeper than 1.1.3**, just later. A notch that has begun releasing and is hit by the howl again jumps straight back to `deepestDb` in **one step of up to 18 dB** — deeper, immediately, with no 300 ms wait, because a returning howl is the emergency this whole feature exists for. Outside the bin: **0 dB**. Note M-9: deepening and "is it still howling" are the same test on the spectrum AFTER the notch, so the ladder STOPS at the first rung that quiets the bin (Q7) — a notch may legitimately live its whole life at −6 or −12 and never reach the ceiling. That is the "better tone" this lane was asked for, and it is also the row testers will notice.

**M-3 — the stopping behaviour is NOT testable in this suite, and no test here pretends it is.** The harness writes the raw tone straight into `h.tap` (`tests/test_notchcontroller.cpp:344-349`); nothing applies the notch chain between the tone and the Detector, so the analyser sees the UNNOTCHED spectrum on every frame. In these tests the bin therefore never goes quiet and the ladder always climbs to the ceiling — which is exactly what `AContinuingHowlDeepensOneRungPer300ms` asserts. The Q7 claim "stops at the first rung that quiets the bin" can only be observed on a real rig, so it belongs in the tester notes (Task 10), not in a fixture. **Do not build a fake notched-spectrum source to "cover" it**: a source that subtracts a modelled notch would be testing the model, not the loop, and would go green while the real chain did something else.

**Files:**
- Modify: `src/app/NotchController.cpp:737-763` (the reinforce loop inside `processSpectrumForDetection`)
- Test: `tests/test_notchcontroller.cpp`

**Interfaces:**
- Consumes (Task 4): `pushRetuneLocked`, `ceilingDbFor`, `nextDeeperRungDb` (two-arg, Q13), `activeForTest`, `kDeepenAfterMs`, `RetuneReason`, and the `ModelNotch` fields `deepestDb` / `stageChangedAtMs` / `quietMs` / `releasedSteps`.
- Produces: nothing new in the public API. Task 7 relies on `quietMs` being ZEROED here on every reinforce, and on `releasedSteps` returning to 0 on a reclamp.

**Thread facts this task must respect:** the loop at `.cpp:737-763` already holds `modelMutex_` (taken at line 738), and `modelMutex_` is NOT recursive — `setNotch`/`setNotchImpl` take it themselves at `.cpp:162`, so calling either from here deadlocks (B-1). Everything goes through `pushRetuneLocked`. `liveMs_` only advances in step 2 of `runOnce`, AFTER the whole drain loop, so the 300 ms gate cannot fire twice within one drain. Under LINKED the loop visits both lanes from one frame (`.cpp:740-742`), which is what keeps a linked pair on the same rung.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_notchcontroller.cpp`.

```cpp
// RED IF the ladder stops climbing while the bin is still over threshold
// (spec 4.4). The tone never stops, so every 300 ms of live time buys one
// rung until the ceiling's rung is reached.
TEST (NotchControllerLadder, AContinuingHowlDeepensOneRungPer300ms)
{
    Harness h;
    h.controller.setDetectionActive (true);
    h.controller.setNotchDefaults (30.0, -24.0);   // ceiling: the whole ladder

    int slot = -1;
    ASSERT_NO_FATAL_FAILURE (primeAndPlace (h, slot));
    ASSERT_GE (slot, 0);
    ASSERT_DOUBLE_EQ (h.controller.depthDbForTest (0, slot), -12.0);
    const double placedAt = h.controller.liveMsForTest();

    // Keep the howl going. 300 ms is ~29 blocks of 10.667 ms.
    SineSource tone;
    double sawMinus18At = -1.0, sawMinus24At = -1.0;
    for (int i = 0; i < 200; ++i)
    {
        pump (h, tone.hop());
        const double d = h.controller.depthDbForTest (0, slot);
        if (sawMinus18At < 0.0 && d <= -18.0) sawMinus18At = h.controller.liveMsForTest();
        if (sawMinus24At < 0.0 && d <= -24.0) sawMinus24At = h.controller.liveMsForTest();
    }

    ASSERT_GT (sawMinus18At, 0.0) << "the ladder never reached -18";
    ASSERT_GT (sawMinus24At, 0.0) << "the ladder never reached -24";
    EXPECT_GE (sawMinus18At - placedAt, NotchController::kDeepenAfterMs);
    EXPECT_GE (sawMinus24At - sawMinus18At, NotchController::kDeepenAfterMs);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, slot),   -24.0);
    EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, slot), -24.0);
}

// RED IF the ceiling stops capping the climb. A ceiling of -18 must leave the
// notch at -18 no matter how long the howl continues.
TEST (NotchControllerLadder, DeepeningStopsAtTheCeilingRung)
{
    Harness h;
    h.controller.setDetectionActive (true);
    h.controller.setNotchDefaults (30.0, -18.0);

    int slot = -1;
    ASSERT_NO_FATAL_FAILURE (primeAndPlace (h, slot));
    ASSERT_GE (slot, 0);

    SineSource tone;
    for (int i = 0; i < 300; ++i)
        pump (h, tone.hop());

    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, slot), -18.0);
}

// RED IF the last step of the climb is a full 6 dB past an off-rung ceiling,
// or is quantised away (Q13). Ceiling -13.7: the effective ladder is
// -6 -> -12 -> -13.7, so the final step is 1.7 dB, not 6.
TEST (NotchControllerLadder, TheLastStepLandsExactlyOnAnOffRungCeiling)
{
    Harness h;
    h.controller.setDetectionActive (true);
    h.controller.setNotchDefaults (30.0, -13.7);

    int slot = -1;
    ASSERT_NO_FATAL_FAILURE (primeAndPlace (h, slot));   // steep -> -12, capped -12
    ASSERT_GE (slot, 0);
    ASSERT_DOUBLE_EQ (h.controller.depthDbForTest (0, slot), -12.0);

    SineSource tone;
    for (int i = 0; i < 300; ++i)
        pump (h, tone.hop());

    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, slot),   -13.7);
    EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, slot), -13.7);
}

// RED IF a ceiling of -6 stops meaning "never deepen" (spec 4.1, Q13: the
// effective ladder for ceiling -6 is one rung long).
//
// M-2: this must NOT assert "no Set at all". Detection stays armed -- and it
// has to, because reinforcement lives behind the same gate -- so the armed
// detector keeps confirming and placing FRESH notches on the still-ringing
// lane, each of which is a legitimate Set on a DIFFERENT index. That is
// documented on the stereo auto-release tests at :1101-1102 and is exactly the
// trap this assertion fell into. Assert on the depth of THIS slot, and on
// commands carrying this (lane, index) only.
TEST (NotchControllerLadder, ACeilingOfMinusSixNeverDeepens)
{
    Harness h;
    h.controller.setDetectionActive (true);
    h.controller.setNotchDefaults (30.0, -6.0);

    int slot = -1;
    ASSERT_NO_FATAL_FAILURE (primeAndPlace (h, slot));
    ASSERT_GE (slot, 0);
    NotchCommand drained {};
    while (h.commands.read (&drained, 1) == 1) {}

    SineSource tone;
    int retunesOfThisSlot = 0;
    for (int i = 0; i < 300; ++i)
    {
        pump (h, tone.hop());
        while (h.commands.read (&drained, 1) == 1)
            if (drained.type == NotchCommandType::Set
                && drained.channel == 0 && drained.index == slot)
                ++retunesOfThisSlot;
    }

    EXPECT_TRUE (h.controller.activeForTest (0, slot));
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, slot),   -6.0);
    EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, slot), -6.0);
    EXPECT_EQ (retunesOfThisSlot, 0) << "a ceiling of -6 emitted a retune";
}

// RED IF a Preset notch is dragged down the ladder (spec 4.3): the file said
// what it wanted, and its own depth is its ceiling.
TEST (NotchControllerLadder, APresetNotchNeverDeepens)
{
    Harness h;
    h.controller.setDetectionActive (true);
    h.controller.setNotchDefaults (30.0, -24.0);
    // Place it exactly on the tone SineSource produces (bin 43 = 1007.8125 Hz)
    // so the reinforce loop finds it every frame.
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1007.8125, 30.0, -12.0,
                                        NotchController::Origin::Preset));

    NoiseSource quiet;
    for (int i = 0; i < kWarmupBlocks; ++i)
        pump (h, quiet.hop());
    SineSource tone;
    for (int i = 0; i < 200; ++i)
        pump (h, tone.hop());

    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -12.0);
}

// RED IF a reclamp waits for the 300 ms gate, or fails to return to the
// DEEPEST rung the notch ever held (spec 4.4, Q3). A howl coming back is the
// emergency this feature exists for -- it is answered on the same frame.
TEST (NotchControllerLadder, AReturningHowlReclampsImmediatelyToDeepestDb)
{
    Harness h;
    h.controller.setDetectionActive (true);
    h.controller.setNotchDefaults (30.0, -24.0);

    int slot = -1;
    ASSERT_NO_FATAL_FAILURE (primeAndPlace (h, slot));
    ASSERT_GE (slot, 0);
    SineSource tone;
    for (int i = 0; i < 200; ++i)      // climb to the ceiling rung
        pump (h, tone.hop());
    ASSERT_DOUBLE_EQ (h.controller.deepestDbForTest (0, slot), -24.0);

    // Simulate a release having already happened: the notch is sitting on a
    // shallower rung with releasedSteps > 0. Task 7's clock does this for
    // real; here the seam gets us there in one line.
    ASSERT_TRUE (h.controller.retuneForTest (0, slot, -12.0,
                                             NotchController::RetuneReason::Release));

    NotchCommand drained {};
    while (h.commands.read (&drained, 1) == 1) {}
    const double before = h.controller.liveMsForTest();

    pump (h, tone.hop());              // ONE frame of howl at that bin

    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, slot), -24.0)
        << "the reclamp waited instead of firing on the frame";
    EXPECT_LT (h.controller.liveMsForTest() - before, NotchController::kDeepenAfterMs);
    EXPECT_DOUBLE_EQ (h.controller.quietMsForTest (0, slot), 0.0);
}

// RED IF a LINKED pair can end up on different rungs. Both lanes are
// reinforced from the same frame (.cpp:740-742), so they must climb together.
TEST (NotchControllerLadder, LinkedLanesStayOnTheSameRung)
{
    StereoHarness h;
    h.controller.setLinked (true);
    h.controller.setDetectionActive (true);
    h.controller.setNotchDefaults (30.0, -24.0);

    NoiseSource quietL, quietR; quietR.rng.seed (999u);
    for (int i = 0; i < kWarmupBlocks; ++i)
        pumpStereo (h, quietL.hop(), quietR.hop());

    SineSource toneL, toneR;
    for (int i = 0; i < 200; ++i)
        pumpStereo (h, toneL.hop(), toneR.hop());

    // B-3: activeForTest, not `depthDbForTest < 0` -- a cleared slot keeps its
    // depth, so a depth probe would compare two dead lanes and pass on nothing.
    bool sawAny = false;
    for (int i = 0; i < NotchController::kSlots; ++i)
        if (h.controller.activeForTest (0, i) && h.controller.activeForTest (1, i))
        {
            sawAny = true;
            EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, i),
                              h.controller.depthDbForTest (1, i)) << "index " << i;
            EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, i),
                              h.controller.deepestDbForTest (1, i)) << "index " << i;
        }
    EXPECT_TRUE (sawAny) << "no linked pair was ever placed";
}

// RED IF an INDEP placement touches the other lane (spec 5.3, "INDEP: làn kia
// không đổi") -- the half the LINKED test above cannot see. The right lane is
// quiet throughout, so nothing may appear on it at any rung.
TEST (NotchControllerLadder, IndepLeavesTheOtherLaneUntouchedThroughTheWholeClimb)
{
    StereoHarness h;
    h.controller.setLinked (false);
    h.controller.setDetectionActive (true);
    h.controller.setNotchDefaults (30.0, -24.0);

    NoiseSource quietL, quietR; quietR.rng.seed (999u);
    for (int i = 0; i < kWarmupBlocks; ++i)
        pumpStereo (h, quietL.hop(), quietR.hop());

    SineSource toneL;
    for (int i = 0; i < 200; ++i)
        pumpStereo (h, toneL.hop(), quietR.hop());

    bool sawLeft = false;
    for (int i = 0; i < NotchController::kSlots; ++i)
    {
        sawLeft |= h.controller.activeForTest (0, i);
        EXPECT_FALSE (h.controller.activeForTest (1, i))
            << "INDEP placed or deepened on the quiet lane, index " << i;
    }
    EXPECT_TRUE (sawLeft) << "the ringing lane never placed anything";
}

// RED IF a Soundcheck notch is deepened or reclamped (KD-7).
TEST (NotchControllerLadder, SoundcheckNotchesNeverDeepen)
{
    Harness h;
    h.controller.setNotchDefaults (30.0, -24.0);
    h.controller.startSoundcheck();

    int slot = -1;
    ASSERT_NO_FATAL_FAILURE (primeAndPlace (h, slot));
    ASSERT_GE (slot, 0);
    const double placed = h.controller.depthDbForTest (0, slot);

    SineSource tone;
    for (int i = 0; i < 200; ++i)
        pump (h, tone.hop());

    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, slot), placed);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --config Release && cd build && ctest -C Release -R NotchControllerLadder --output-on-failure`
Expected: FAIL — `AContinuingHowlDeepensOneRungPer300ms` never leaves −12, and `AReturningHowlReclampsImmediatelyToDeepestDb` stays at −12.

- [ ] **Step 3: Implement the deepen/reclamp branch in `src/app/NotchController.cpp`**

Replace the body of the `if (PeakinessAnalyzer::peakinessAt(...) > la.analyzer.getThreshold())` block (lines 755-760) with:

```cpp
                // Same live threshold analyse() used to accept candidates --
                // a notch re-tested against a stale default would disagree
                // with the panel about whether it is still reinforced.
                if (PeakinessAnalyzer::peakinessAt (block.magnitudes,
                                                    Detector::kNumBins, bin)
                        > la.analyzer.getThreshold())
                {
                    n.lastDetectedMs = liveMs_;
                    // Lane G: the bin is loud again, so the release clock's
                    // banked quiet time is spent, not merely paused.
                    n.quietMs        = 0.0;

                    if (n.releasedSteps > 0)
                    {
                        // RECLAMP (spec 4.4, Q3). No 300 ms gate: the ladder
                        // had already PROVEN this bin needs deepestDb, and a
                        // howl coming back is the emergency the whole feature
                        // exists for. One step, ramped over kRampMs like every
                        // other depth change -- it may be more than 6 dB, and
                        // that is deliberate (invariant 3 bounds the DEEP
                        // direction per step; this is a return to a depth this
                        // notch already ran at).
                        if (pushRetuneLocked (c, i, n.deepestDb, RetuneReason::Reclamp))
                        {
                            n.releasedSteps    = 0;
                            n.stageChangedAtMs = liveMs_;
                        }
                    }
                    else if (n.origin == Origin::Detector)
                    {
                        // DEEPEN (spec 4.4). Only Detector notches climb:
                        // Preset/Manual said what they wanted (Q8) and
                        // Soundcheck is already excluded above (KD-7).
                        //
                        // M-9, and it is a CONSEQUENCE of Q2, not a bug: the
                        // test that decides "deepen" is the same test that
                        // decides "still howling", run on the spectrum AFTER
                        // the notch. So the ladder stops at the first rung
                        // that quiets the bin and may never reach the
                        // ceiling. That is the point -- depth by need.
                        //
                        // M-3: no headless test can SEE that, because the
                        // fixture feeds the analyser the un-notched tone. It
                        // is a rig claim; the tester notes carry it.
                        //
                        // Q13: the ceiling IS the last rung, and it need not
                        // be a multiple of 6 -- presets/Music.json ships -10,
                        // so this loop's final step there is 4 dB, not 6.
                        const double ceiling = ceilingDbFor (n);
                        if (n.depthDB > ceiling
                            && (liveMs_ - n.stageChangedAtMs) >= kDeepenAfterMs)
                        {
                            // nextDeeperRungDb caps at the ceiling itself, so
                            // the step can never overshoot it.
                            const double next = nextDeeperRungDb (n.depthDB, ceiling);
                            if (pushRetuneLocked (c, i, next, RetuneReason::Deepen))
                            {
                                n.deepestDb        = next;
                                n.stageChangedAtMs = liveMs_;
                            }
                        }
                    }
                }
```

- [ ] **Step 4: Run the controller tests**

Run: `cmake --build build --config Release && cd build && ctest -C Release -R NotchController --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Run the full suite**

Run: `cd build && ctest -C Release`
Expected: `100% tests passed` (498).

- [ ] **Step 6: Commit**

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add src/app/NotchController.cpp tests/test_notchcontroller.cpp
```
```bash
git commit -m "feat(controller): deepen one rung per 300ms while the bin still rings; reclamp on return"
```

---

### Task 7: the release ladder in `runOnce` step 3 — quiet clock, freeze, live ceiling

**Mức level dự kiến (spec §3):** this replaces 1.1.3's cliff (30 s un-reinforced → `Clear`, i.e. straight to 0 dB) with a staircase. From the last reinforce: **+6 dB at 30 s**, another **+6 dB at 40 s**, `Clear` at **50 s**. Between 30 s and 50 s the notch is therefore **DEEPER than 1.1.3** — 1.1.3 was already at 0 dB by then — so a tester hears up to **12 dB more tone missing** for 20 s after a howl stops. That is the direction of the trade Q3 chose, and it is one of the two rows testers will notice most. Beyond 50 s: identical to 1.1.3 (cleared, 0 dB). While RING RISK is at or above RISING (`score ≥ 0.55 × kConfirmScore = 0.385`) the clock **stops**, with **no time limit** (Q9), so in a room that stays tense the notch can hold its rung indefinitely — deeper than 1.1.3 for as long as the room is tense. A ceiling lowered mid-show pulls a Detector notch up to the new rung on the next tick, one ramped step, possibly more than 6 dB shallower at once (invariant 3 permits that). Outside the bin: **0 dB**.

**Files:**
- Modify: `src/app/NotchController.cpp:403-416` (step 3, replaced wholesale — the `releaseFrozen` publish is a NEW scope inside the replacement, not an edit to the snapshot block at `:361-372`. M-6: the v1 Files list cited `:369-371`, which is the `ringRiskScore`/`ringRiskValid`/`ringRiskThreshold` publish and is not touched by this task)
- Modify: `src/app/NotchController.h:83` (`kAutoReleaseMs` doc string)
- Test: `tests/test_notchcontroller.cpp`

**Interfaces:**
- Consumes (Task 4): `pushRetuneLocked`, `ceilingDbFor`, `nextShallowerRungDb`, `activeForTest`, `kReleaseFirstMs`, `kReleaseStepMs`, `kRiskFreezeFraction`, `ringRiskOverrideForTest_`, `SnapshotBuffer::releaseFrozen`, `quietMsForTest`. Consumes (Task 6): `quietMs` zeroed and `releasedSteps` reset on reinforce.

**Release timing — the arithmetic every pump length in this task and Task 8 is derived from.** From the rung a notch STANDS on, `kReleaseFirstMs` (30 s) buys the first step and `kReleaseStepMs` (10 s) each one after, including the Clear:

| Standing at | Steps to −6 | Time to the first release | Time to Clear |
|---|---|---|---|
| −24 | 3 (−18, −12, −6) | 30 s | 30 + 10 + 10 + 10 = **60 s** |
| −18 | 2 (−12, −6) | 30 s | 30 + 10 + 10 = **50 s** |
| −13.7 | 2 (−12, −6) | 30 s | **50 s** |
| −12 | 1 (−6) | 30 s | 30 + 10 = **40 s** |
| −10 | 1 (−6) | 30 s | **40 s** |
| −6 | 0 | — | 30 s |

B-4: the v1 plan used `pumpQuietFor(..., 55000.0)` on notches standing at −24, which is 5 s short of the Clear. Every pump length below states the rung it was computed from.
- Produces: the `ClearReason::AutoRelease` moment Task 8 hangs room memory on.

**Thread facts this task must respect:** step 3 runs on the detector thread and takes `modelMutex_`. `snapshotMutex_` must NOT be taken inside it — publishing `releaseFrozen` therefore happens in its own scope BEFORE `modelMutex_` is acquired, keeping the two mutexes un-nested exactly as they are today (M-5). The freeze reads `frameScoreValid_` / `frameMaxScore_` (`NotchController.h:433-434`), which are detector-thread-only members holding precisely the two numbers the snapshot publish just wrote — no lock needed, and no new lock order created.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_notchcontroller.cpp`. Add this helper to the anonymous namespace next to `pump` (line 344) first:

```cpp
// Pumps ambient noise for `ms` of LIVE time. The tap stays alive (D-06) so
// the release clock runs, and the noise never feeds peakiness at 1 kHz.
void pumpQuietFor (Harness& h, NoiseSource& quiet, double ms)
{
    const int blocks = (int) std::lround (ms / kBlockMs);
    for (int i = 0; i < blocks; ++i)
        pump (h, quiet.hop());
}
```

```cpp
// RED IF the release cliff comes back, or the rung timings move (spec 4.5,
// Q3). This is the headline behaviour of the whole lane.
TEST (NotchControllerLadder, ReleaseWalksTheLadderAt30sThen10sPerRung)
{
    Harness h;
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -18.0,
                                        NotchController::Origin::Detector));
    NoiseSource quiet;

    pumpQuietFor (h, quiet, 29000.0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -18.0) << "released early";

    pumpQuietFor (h, quiet, 1500.0);                     // past 30 s
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -12.0);

    pumpQuietFor (h, quiet, 9000.0);                     // 9 s into the 10 s rung
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -12.0);

    pumpQuietFor (h, quiet, 1500.0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -6.0);

    pumpQuietFor (h, quiet, 10500.0);                    // 30 + 10 + 10 = 50 s: Clear
    // B-3: the notch is GONE, which is `active == false`. depthDbForTest still
    // reads -6 here, because pushClearLocked deliberately leaves depthDB alone
    // for the Clear event to read (NotchController.cpp:208).
    EXPECT_FALSE (h.controller.activeForTest (0, 0))
        << "the notch never cleared at the bottom of the ladder";

    NotchController::SnapshotBuffer snap;
    std::vector<float> hop (512, 0.05f);
    h.tap.write (hop.data(), hop.size());
    h.controller.runOnce();
    h.controller.copySnapshot (snap);
    EXPECT_EQ (snap.notchCount, 0u);
}

// RED IF the final rung stops emitting a real Clear with the AutoRelease
// reason -- lane D's label and Task 8's room-memory write both hang off it.
TEST (NotchControllerLadder, TheBottomOfTheLadderClearsWithAutoRelease)
{
    Recorder r; Harness h; h.controller.setEventSink (r.sink());
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -18.0,
                                        NotchController::Origin::Detector));
    NoiseSource quiet;
    pumpQuietFor (h, quiet, 52000.0);   // from -18: 30 + 10 + 10 = 50 s, +2 s margin
    h.controller.runOnce();

    const auto clears = r.clears();
    ASSERT_EQ (clears.size(), 1u);
    EXPECT_EQ (clears[0].reason, NotchController::ClearReason::AutoRelease);
    EXPECT_FLOAT_EQ (clears[0].depthDb, -6.0f) << "cleared from the wrong rung";

    int retunes = 0;
    for (const auto& e : r.events)
        if (e.kind == Ev::Kind::Retune
            && e.retuneReason == NotchController::RetuneReason::Release)
            ++retunes;
    EXPECT_EQ (retunes, 2) << "-18 -> -12 -> -6 is two release steps";
}

// RED IF the freeze stops working (spec 4.5 step 1, Q4). A tense room must not
// have its notches wound back under it. There is deliberately NO time cap.
TEST (NotchControllerLadder, RingRiskAtRisingFreezesTheReleaseClock)
{
    Harness h;
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -18.0,
                                        NotchController::Origin::Detector));
    h.controller.setRingRiskOverrideForTest (
        std::make_pair (true, NotchController::kRiskFreezeFraction
                              * CandidateScorer::kConfirmScore));

    NoiseSource quiet;
    pumpQuietFor (h, quiet, 90000.0);   // three times the first rung's time
    EXPECT_DOUBLE_EQ (h.controller.quietMsForTest (0, 0), 0.0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -18.0);

    NotchController::SnapshotBuffer snap;
    h.controller.copySnapshot (snap);
    EXPECT_TRUE (snap.releaseFrozen) << "the frozen flag was never published (Q9)";

    // Room calms: the clock starts again from where it was, which is 0.
    h.controller.setRingRiskOverrideForTest (std::nullopt);
    pumpQuietFor (h, quiet, 31000.0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -12.0);
}

// RED IF an INVALID ring-risk reading is treated as a freeze (spec 4.5,
// invariant 7). Detection off must release exactly as 1.1.3 did.
TEST (NotchControllerLadder, InvalidRingRiskDoesNotFreezeTheClock)
{
    Harness h;
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -12.0,
                                        NotchController::Origin::Detector));
    h.controller.setRingRiskOverrideForTest (std::make_pair (false, 1.0f));

    NoiseSource quiet;
    pumpQuietFor (h, quiet, 31000.0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -6.0);

    NotchController::SnapshotBuffer snap;
    h.controller.copySnapshot (snap);
    EXPECT_FALSE (snap.releaseFrozen);
}

// RED IF a score just under the RISING band starts freezing the clock.
TEST (NotchControllerLadder, ScoreJustBelowTheRisingBandDoesNotFreeze)
{
    Harness h;
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -12.0,
                                        NotchController::Origin::Detector));
    h.controller.setRingRiskOverrideForTest (
        std::make_pair (true, NotchController::kRiskFreezeFraction
                              * CandidateScorer::kConfirmScore - 0.01f));

    NoiseSource quiet;
    pumpQuietFor (h, quiet, 31000.0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -6.0);
}

// RED IF lowering the depth slider mid-show stops pulling a Detector notch up
// to the new rung (spec 4.1). The ceiling is read LIVE, every tick.
TEST (NotchControllerLadder, LoweringTheCeilingPullsADetectorNotchUpOnTheNextTick)
{
    Recorder r; Harness h; h.controller.setEventSink (r.sink());
    h.controller.setNotchDefaults (30.0, -24.0);
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -18.0,
                                        NotchController::Origin::Detector));

    NoiseSource quiet;
    pumpQuietFor (h, quiet, 200.0);
    h.controller.setNotchDefaults (30.0, -12.0);   // ceiling drops two rungs' worth
    pumpQuietFor (h, quiet, 200.0);

    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0),   -12.0);
    EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, 0), -12.0);

    const Ev* ceil = nullptr;
    for (const auto& e : r.events)
        if (e.kind == Ev::Kind::Retune
            && e.retuneReason == NotchController::RetuneReason::Ceiling) { ceil = &e; break; }
    ASSERT_NE (ceil, nullptr);
    EXPECT_FLOAT_EQ (ceil->fromDepthDb, -18.0f);
    EXPECT_FLOAT_EQ (ceil->depthDb,     -12.0f);
}

// RED IF RAISING the ceiling deepens a notch on its own (spec 5.3: "không đào
// lại khi trần nâng về -18 cho tới khi reinforce"). The ceiling branch is a
// one-way valve -- it pulls a notch UP to a lowered ceiling, and a raised one
// buys nothing until the bin actually rings again and the deepen path (Task 6)
// runs. Detection is OFF here precisely so nothing can reinforce.
TEST (NotchControllerLadder, RaisingTheCeilingDoesNotDeepenUntilTheBinRingsAgain)
{
    Recorder r; Harness h; h.controller.setEventSink (r.sink());
    h.controller.setNotchDefaults (30.0, -24.0);
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -18.0,
                                        NotchController::Origin::Detector));

    NoiseSource quiet;
    h.controller.setNotchDefaults (30.0, -12.0);   // lower: pulls up to -12
    pumpQuietFor (h, quiet, 300.0);
    ASSERT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -12.0);

    h.controller.setNotchDefaults (30.0, -24.0);   // raise it all the way back
    pumpQuietFor (h, quiet, 5000.0);               // well past kDeepenAfterMs

    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0),   -12.0)
        << "raising the ceiling re-deepened a notch with no reinforce";
    EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, 0), -12.0);
    for (const auto& e : r.events)
        if (e.kind == Ev::Kind::Retune)
            EXPECT_NE (e.retuneReason, NotchController::RetuneReason::Deepen);
}

// RED IF the slider starts dragging a Preset or Manual notch (Q8).
TEST (NotchControllerLadder, LoweringTheCeilingLeavesPresetAndManualNotchesAlone)
{
    Harness h;
    h.controller.setNotchDefaults (30.0, -24.0);
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -18.0,
                                        NotchController::Origin::Preset));
    ASSERT_TRUE (h.controller.setNotch (0, 1, 1200.0, 30.0, -24.0,
                                        NotchController::Origin::Manual));

    NoiseSource quiet;
    h.controller.setNotchDefaults (30.0, -6.0);
    pumpQuietFor (h, quiet, 500.0);

    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -18.0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 1), -24.0);
}

// RED IF the ceiling branch fires for a non-Detector notch (B-1). This is the
// exact case that ships: tests/test_gui_wiring.cpp:1030 adopts a preset notch
// at -9.0, which is not a ladder rung. Under spec v2's quantisation
// (ceilingRungDb(-9) == -6) the FIRST runOnce() after adoption would have seen
// depthDB -9 < rung -6 and retuned it to -6 -- a preset silently 3 dB
// shallower than the file says, and a violation of Q8 / spec 4.1 "slider
// không chạm". Q13 removes the quantisation, and the Origin guard means the
// branch cannot come back even if the arithmetic changes again.
TEST (NotchControllerLadder, AnOffRungPresetDepthSurvivesTheFirstTick)
{
    Recorder r; Harness h; h.controller.setEventSink (r.sink());
    h.controller.setNotchDefaults (30.0, -24.0);
    // The shape tests/test_gui_wiring.cpp uses: an adopted preset notch at -9.
    PresetNotch p;
    p.index = 2; p.freq = 1234.0; p.Q = 28.0; p.depthDB = -9.0;
    ASSERT_EQ (h.controller.adoptPreset ({ p }), 1);
    ASSERT_DOUBLE_EQ (h.controller.depthDbForTest (0, 2), -9.0);

    std::vector<float> hop (512, 0.05f);
    h.tap.write (hop.data(), hop.size());
    h.controller.runOnce();

    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 2), -9.0)
        << "the ceiling branch moved a Preset notch";
    for (const auto& e : r.events)
        EXPECT_NE (e.kind, Ev::Kind::Retune);
}

// RED IF a Preset notch is exempted from the RELEASE ladder, or if its reclamp
// target drifts off its own depth (spec 5.3: "Preset: adoptPreset -12 => không
// đào; nhả thang; kẹp lại về -12"). Q8 makes the slider unable to touch it; it
// does NOT make it immortal -- only Soundcheck is (KD-7).
TEST (NotchControllerLadder, APresetNotchReleasesDownTheLadderAndReclampsToItsOwnDepth)
{
    Harness h;
    h.controller.setNotchDefaults (30.0, -24.0);
    PresetNotch p;
    p.index = 0; p.freq = 1007.8125; p.Q = 30.0; p.depthDB = -12.0;
    ASSERT_EQ (h.controller.adoptPreset ({ p }), 1);

    NoiseSource quiet;
    pumpQuietFor (h, quiet, 31000.0);   // from -12: the first rung costs 30 s
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -6.0);
    EXPECT_TRUE (h.controller.activeForTest (0, 0));

    // The howl comes back: a reclamp goes to deepestDb, which for a preset
    // notch is the depth the FILE named -- never deeper, never the slider's.
    h.controller.setDetectionActive (true);
    SineSource tone;
    for (int i = 0; i < 40; ++i)
        pump (h, tone.hop());

    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -12.0)
        << "a preset notch reclamped past its own depth";
}

// RED IF a Soundcheck notch is ever released (KD-7). It already had a test
// (SoundcheckNotchNeverAutoReleases); this one pins that the LADDER does not
// touch it either -- no Retune of any reason, at any rung.
TEST (NotchControllerLadder, SoundcheckNotchesNeverRelease)
{
    Recorder r; Harness h; h.controller.setEventSink (r.sink());
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -18.0,
                                        NotchController::Origin::Soundcheck));
    NoiseSource quiet;
    pumpQuietFor (h, quiet, 60000.0);
    h.controller.runOnce();

    for (const auto& e : r.events)
        EXPECT_NE (e.kind, Ev::Kind::Retune) << "the ladder moved a soundcheck notch";
    EXPECT_TRUE (r.clears().empty());
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -18.0);
}

// RED IF anything in the fixture can emit a Set outside [-24, 0] (invariant
// 1). 200 randomised blocks of reinforce / quiet / frozen, watching every
// command that leaves the controller.
TEST (NotchControllerLadder, NoCommandEverLeavesTheLegalDepthRange)
{
    Harness h;
    h.controller.setDetectionActive (true);
    h.controller.setNotchDefaults (30.0, -24.0);

    std::mt19937 rng { 20260907u };
    std::uniform_int_distribution<int> pick { 0, 2 };
    NoiseSource quiet;
    SineSource  tone;

    for (int i = 0; i < 200; ++i)
    {
        switch (pick (rng))
        {
            case 0: pump (h, tone.hop()); break;
            case 1: pump (h, quiet.hop()); break;
            case 2:
                h.controller.setRingRiskOverrideForTest (std::make_pair (true, 0.9f));
                pump (h, quiet.hop());
                h.controller.setRingRiskOverrideForTest (std::nullopt);
                break;
        }
        NotchCommand cmd {};
        while (h.commands.read (&cmd, 1) == 1)
            if (cmd.type == NotchCommandType::Set)
            {
                EXPECT_LE (cmd.depthDB,  0.0f);
                EXPECT_GE (cmd.depthDB, -24.0f);
            }
    }
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --config Release && cd build && ctest -C Release -R NotchControllerLadder --output-on-failure`
Expected: FAIL — `ReleaseWalksTheLadderAt30sThen10sPerRung` finds the notch already cleared at 30.5 s (today's cliff), and the freeze tests all release regardless of the override.

- [ ] **Step 3: Publish `releaseFrozen`'s neighbours and update the constant's doc**

In `src/app/NotchController.h:82-83`, replace the `kAutoReleaseMs` comment:

```cpp
    // Spec 5.2 step 7 as amended by lane G (spec 4.5, Q3): 30 s of quiet buys
    // the FIRST rung of the release ladder, not a Clear. kReleaseStepMs
    // (10 s) buys each rung after it, and only the notch already sitting at
    // -6 dB is cleared. The name and value are kept because this is still
    // "how long the first release takes"; kReleaseFirstMs is its lane-G alias.
    static constexpr double kAutoReleaseMs       = 30000.0;
```

- [ ] **Step 4: Replace step 3 in `src/app/NotchController.cpp`**

Replace lines 403-416 in `runOnce` with:

```cpp
    // 3. Release ladder (spec 4.5), replacing 1.1.3's "30 s -> Clear" cliff.

    // --- step 1: FREEZE ----------------------------------------------------
    // Computed and PUBLISHED unconditionally, above the tapAlive gate (M-6).
    // The v1 plan wrote latest_.releaseFrozen only while the tap was alive, so
    // the flag kept whatever it last said once the tap died -- a GUI or a log
    // reader would then be told "frozen" about a slot that has no audio at
    // all. A dead tap is not a frozen clock; it is no clock.
    //
    // Read the two detector-thread members the snapshot publish above just
    // wrote. snapshotMutex_ is deliberately NOT taken while modelMutex_ is
    // held: model -> snapshot would be a lock order this app has never had,
    // and inventing one for a readout is not a trade worth making (M-5).
    // These members are written and read only on this thread.
    bool  riskValid = frameScoreValid_;
    float riskScore = frameMaxScore_;
    if (ringRiskOverrideForTest_.has_value())
    {
        riskValid = ringRiskOverrideForTest_->first;
        riskScore = ringRiskOverrideForTest_->second;
    }
    // The GUI's RISING band, from ONE shared constant. An INVALID reading
    // never freezes (invariant 7): detection off must release exactly as
    // 1.1.3 did, or turning detection off would strand every notch.
    const bool frozen = tapAlive
                     && riskValid
                     && riskScore >= kRiskFreezeFraction * CandidateScorer::kConfirmScore;

    // Its own scope, before modelMutex_ is taken anywhere below, so the two
    // mutexes stay un-nested. Q9: no time cap on the freeze, and 1.2.0 draws
    // nothing with this flag -- it exists so a later GUI or the session log
    // can say WHY a notch is not winding back.
    {
        const std::lock_guard<std::mutex> lock (snapshotMutex_);
        latest_.releaseFrozen = frozen;
    }

    //    The ladder itself is only meaningful while audio actually flows (D-06).
    if (tapAlive)
    {
        const std::lock_guard<std::mutex> lock (modelMutex_);
        for (int c = 0; c < kChannels; ++c)
            for (int i = 0; i < kSlots; ++i)
            {
                auto& n = model_[slotOf (c, i)];
                // KD-7: soundcheck notches never release, never deepen, never
                // follow the ceiling. They clear only on an explicit request.
                if (! n.active || n.origin == Origin::Soundcheck)
                    continue;

                // --- live ceiling (spec 4.1) -------------------------------
                // The ceiling is read fresh every tick, so an operator who
                // pulls the depth slider up mid-show sees it take effect.
                //
                // B-1: DETECTOR ONLY, and the guard is load-bearing, not
                // decoration. Q8 / spec 4.1 say the slider must not touch a
                // Preset or Manual notch, and the shipping path proves it
                // matters: tests/test_gui_wiring.cpp:1030 adopts a preset
                // notch at -9.0. Under spec v2's quantisation the ceiling for
                // that notch resolved to -6, so `-9 < -6` fired on the very
                // first runOnce() and pulled a preset 3 dB shallower than its
                // file. Q13 removes the quantisation (ceilingDbFor returns -9
                // and the comparison is false), but the Origin test is what
                // keeps this branch off Preset/Manual whatever the arithmetic
                // does next.
                //
                // Raising the ceiling deepens nothing here, deliberately: the
                // comparison is one-way, so a raised ceiling only buys depth
                // through Task 6's reinforce path (spec 5.3).
                const double ceiling = ceilingDbFor (n);
                if (n.origin == Origin::Detector && n.depthDB < ceiling)
                {
                    // One ramped step, possibly more than 6 dB, and possibly
                    // landing off-rung (Q13: the ceiling IS the last rung).
                    // Invariant 3 bounds the DEEP direction only -- shallower
                    // is never dangerous.
                    if (pushRetuneLocked (c, i, ceiling, RetuneReason::Ceiling))
                    {
                        n.deepestDb        = std::max (n.deepestDb, ceiling);
                        n.stageChangedAtMs = liveMs_;
                        n.quietMs          = 0.0;   // a depth change restarts the clock
                    }
                    continue;   // one depth change per notch per tick
                }

                // --- steps 2-4: the quiet clock and the ladder -------------
                if (! frozen)
                    n.quietMs += dt;

                const double needed = (n.releasedSteps == 0) ? kReleaseFirstMs
                                                             : kReleaseStepMs;
                if (n.quietMs < needed)
                    continue;

                if (n.depthDB < kDepthLadderDb[0])
                {
                    // Up one rung. An odd Preset/Manual depth resolves to the
                    // nearest rung above it.
                    const double next = nextShallowerRungDb (n.depthDB);
                    if (pushRetuneLocked (c, i, next, RetuneReason::Release))
                    {
                        ++n.releasedSteps;
                        n.stageChangedAtMs = liveMs_;
                        n.quietMs          = 0.0;
                    }
                }
                else
                {
                    // Already on the shallowest rung: the notch has nothing
                    // left to give back, so it goes. Task 8 records what this
                    // bin needed before the Clear.
                    pushClearLocked (c, i, ClearReason::AutoRelease);
                }
            }
    }
```

- [ ] **Step 5: Run the controller tests**

Run: `cmake --build build --config Release && cd build && ctest -C Release -R NotchController --output-on-failure`
Expected: PASS. **Five** pre-existing tests now take LONGER to clear and must be checked, not "fixed" — the behaviour they assert (a Clear eventually arrives, carrying `AutoRelease`) is still correct, only the clock moved. Every new pump length is computed from the rung the notch actually stands on, using the table in this task's header.

**1. `NotchControllerAutoRelease.LiveTapReleasesAfter30s`** (line 152, notch at −12, pumps 7000 × 5 ms = 35 s). From −12 the Clear is at 40 s:

```cpp
TEST (NotchControllerAutoRelease, LiveTapReleasesThroughTheLadderAfter40s)
{
    Harness h;
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -12.0, NotchController::Origin::Detector));
    std::vector<float> hop (512, 0.1f);
    // From -12: 30 s buys -6, +10 s buys the Clear = 40 s. 8800 * 5 ms = 44 s.
    for (int i = 0; i < 8800; ++i) {
        h.tap.write (hop.data(), hop.size());
        h.clock.advance (5.0);
        h.controller.runOnce();
    }
    h.controller.runOnce();                    // make sure everything is flushed

    NotchCommand cmd {};
    bool sawClearOfSlot0 = false;
    while (h.commands.read (&cmd, 1) == 1)
        if (cmd.type == NotchCommandType::Clear && cmd.channel == 0 && cmd.index == 0)
            sawClearOfSlot0 = true;
    EXPECT_TRUE (sawClearOfSlot0);
}
```

**2. `NotchControllerPreset.AdoptedPresetsAutoReleaseLikeDetectorNotches`** (line 267) — **the plan missed this one entirely.** It adopts a preset notch at −12 and pumps 7000 × 5 ms = 35 s expecting a Clear on both lanes. Under the ladder a −12 notch releases to −6 at 30 s and clears at 40 s, so 35 s finds it alive at −6 and the test goes red. Same fix, same arithmetic:

```cpp
    // Preset notches ride the lane-G release ladder like any other non-
    // Soundcheck notch (only KD-7 exempts anything). From -12: -6 at 30 s,
    // Clear at 40 s. 8800 * 5 ms = 44 s.
    for (int i = 0; i < 8800; ++i) {
```

**3. `NotchControllerDetection.HowlThatStopsAutoReleasesAfter30s`** (line 471, `3500` blocks ≈ 37.3 s). The notch there is placed by the detector, so after Task 5 + Task 6 it stands at the **ceiling** — `kDefaultNotchDepthDb` is −18 (`NotchController.h:95`), giving a Clear at 50 s. Take the block count to `5200` (≈ 55.5 s) and rename to `…AutoReleasesThroughTheLadder`.

**4/5. The two stereo auto-release tests** at lines 1106 (`IndepAutoReleaseIsPerLane`) and 1133 (`LinkedAutoReleaseWaitsForBothLanes`). m-6: both use `kAutoReleaseMs / kBlockMs + 20`, **not** `+ 10` — only `EveryClearPathCarriesItsReason` at line 1325 uses `+ 10`. Their notches are detector-placed against the −18 default ceiling, so:

```cpp
    // From the -18 ceiling: 30 + 10 + 10 = 50 s to Clear. The + 20 blocks of
    // slack the 1.1.3 version carried are kept.
    const int blocks = (int) ((NotchController::kReleaseFirstMs
                              + 2 * NotchController::kReleaseStepMs) / kBlockMs) + 20;
```

**6. `NotchControllerEvents.EveryClearPathCarriesItsReason`**'s AutoRelease case (line 1320). Its notch is a **Manual** −12, so its own depth is its ceiling and the Clear is at 40 s. Line 1325's `+ 10` becomes:

```cpp
        // Manual -12: -6 at 30 s, Clear at 40 s.
        const int blocks = (int) ((NotchController::kReleaseFirstMs
                                  + NotchController::kReleaseStepMs) / kBlockMs) + 10;
```

The test also asserts `EXPECT_GT (c[0].ageMs, NotchController::kAutoReleaseMs)` — still true (40 s > 30 s) and left alone. If any of these five now reports TWO clears, the ladder is emitting a Clear per rung instead of a Retune: fix the loop, not the test.

- [ ] **Step 6: Run the full suite**

Run: `cd build && ctest -C Release`
Expected: `100% tests passed` (511).

- [ ] **Step 7: Commit**

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add src/app/NotchController.h src/app/NotchController.cpp tests/test_notchcontroller.cpp
```
```bash
git commit -m "feat(controller): release ladder with a freezable quiet clock and a live ceiling"
```

---

### Task 8: room memory — a howl that comes back to the same bin starts where it left off

**Mức level dự kiến (spec §3):** a howl returning to the **same FFT bin** within 5 minutes of an auto-release is notched at the depth it needed last time (up to the ceiling's rung, at most −24 dB) **on the first block**, instead of crawling up from −6. At that bin, at that instant, the cut is **equal to or deeper than 1.1.3** — up to 18 dB deeper than the −6 dB a first-time placement would give. Outside the bin: **0 dB**. A neighbouring bin is a NEW howl and starts at −6 (Q10: ±0 bin tolerance, because ±1 bin is ±21.5 Hz at 44.1 kHz/2048 and an instrument partial next door must not inherit a deep cut). Each remembered entry is used once.

**m-8 — an entry may hold an off-rung depth, and that is fine.** A Manual notch placed at −9 and auto-released records `deepestDb = −9`. Under Q13 the read path needs no quantisation at all: the remembered depth is clamped to the CURRENT ceiling (`std::max(remembered, ceiling)` — max picks the shallower) by the same step-4 clamp every placement already runs, and the ceiling is itself the deepest legal rung. Nothing else is required. Do not add a "snap to a rung" step; it would only re-introduce the Q13 defect on the memory path.

**Files:**
- Modify: `src/app/NotchController.h` (the `MemoryEntry` struct, `roomMemory_`, `roomMemoryHead_`, three private helpers — declare them next to `pushRetuneLocked`)
- Modify: `src/app/NotchController.cpp:80-117` (`setWidth`), `:223-229` (`clearAll`), `placeConfirmed`'s index-lookup block and depth choice **as Task 5 left them** (Task 5 moved both — do not go looking for the v1 line numbers `:589-596`), the release path's Clear branch (Task 7), `:864-877` (`setSampleRate`)
- Test: `tests/test_notchcontroller.cpp`

**Interfaces:**
- Consumes (Tasks 4, 5, 7): `activeForTest`, `deepestDbForTest`, `ClearReason::AutoRelease`'s branch in step 3, and the hook Task 5 left inside the index-lookup lock.
- Produces:
  ```cpp
  // private
  struct MemoryEntry { double frequencyHz, deepestDb, clearedAtMs; bool used; };
  void   rememberReleaseLocked (int lane, double frequencyHz, double deepestDb, bool bothLanes);
  double takeRememberedDepthLocked (int lane, double frequencyHz, double binWidthHz, bool bothLanes);
  void   clearRoomMemoryLocked();
  std::array<std::array<MemoryEntry, kMemoryEntriesPerLane>, kChannels> roomMemory_ {};
  std::array<int, kChannels> roomMemoryHead_ {};
  ```

**Thread facts:** `roomMemory_` is written from the release path (detector thread, `modelMutex_` held) and read from `placeConfirmed` (detector thread) — and cleared from `setWidth`/`clearAll`/`setSampleRate` (message thread). It therefore lives under `modelMutex_`, the lock all four already take or can take without creating a new order. No allocation: fixed-size arrays, oldest entry overwritten.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_notchcontroller.cpp`. `pumpQuietFor` comes from Task 7.

Two fixture rules apply to every test below and are not repeated in each comment:

- **B-3: liveness is `activeForTest`, never `depthDbForTest(...) < 0.0`.** `pushClearLocked` (`NotchController.cpp:195-212`) lowers only `active` — `depthDB` stays because the Clear event reads it at `:208`. A depth probe therefore matches every slot that has ever held a notch, and the v1 versions of these tests would have "found" a cleared slot and asserted the remembered depth against a corpse.
- **B-4: every quiet pump is derived from the rung the notch stands on**, using the table in Task 7's header. A notch that climbed to the −24 ceiling needs 30 + 10 + 10 + 10 = **60 s** to Clear, not the 55 s the v1 plan used, so the memory entry would never have been written at all.

```cpp
// A findable helper for "which index is holding a notch on this lane" -- the
// tests below all need it and none of them may use the depth to answer it.
int firstActiveIndex (NotchController& c, int lane)
{
    for (int k = 0; k < NotchController::kSlots; ++k)
        if (c.activeForTest (lane, k))
            return k;
    return -1;
}
```

```cpp
// RED IF room memory stops working (spec 4.6, Q3/Q6). A howl that returns to
// the same bin within 5 minutes must not start the ladder over.
TEST (NotchControllerLadder, AHowlReturningToTheSameBinStartsAtTheRememberedDepth)
{
    Harness h;
    h.controller.setDetectionActive (true);
    h.controller.setNotchDefaults (30.0, -24.0);

    int slot = -1;
    ASSERT_NO_FATAL_FAILURE (primeAndPlace (h, slot));
    ASSERT_GE (slot, 0);
    SineSource tone;
    for (int i = 0; i < 200; ++i)     // climb to the ceiling
        pump (h, tone.hop());
    const double deepest = h.controller.deepestDbForTest (0, slot);
    ASSERT_DOUBLE_EQ (deepest, -24.0);

    NoiseSource quiet;
    // From -24: 30 + 10 + 10 + 10 = 60 s to Clear. 62 s leaves 2 s of margin.
    pumpQuietFor (h, quiet, 62000.0);
    ASSERT_FALSE (h.controller.activeForTest (0, slot))
        << "the notch had not finished the release ladder, so nothing was remembered";

    // The same howl comes back. With no memory this places at -12 (SineSource
    // is a hard start, so the steep-rise branch fires); the memory must beat
    // that and go straight to what the room needed.
    SineSource again;
    int placed = -1;
    for (int i = 0; i < 60 && placed < 0; ++i)
    {
        pump (h, again.hop());
        placed = firstActiveIndex (h.controller, 0);
    }
    ASSERT_GE (placed, 0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, placed), deepest);
}

// RED IF the bin tolerance loosens (Q10). One bin away is a DIFFERENT howl --
// at 48 kHz / 2048 that is 23.4 Hz, and an instrument partial next door must
// not inherit a -24 dB cut on its first block.
TEST (NotchControllerLadder, OneBinAwayIsANewHowlAndStartsAtMinusSix)
{
    Harness h;
    const double binHz = kTestSr / Detector::kFftSize;   // 23.4375
    h.controller.setNotchDefaults (30.0, -24.0);

    // Place, deepen by hand, then let the ladder clear it: the memory entry
    // is written by the AutoRelease at the bottom of the ladder.
    ASSERT_TRUE (h.controller.setNotch (0, 0, 43.0 * binHz, 30.0, -24.0,
                                        NotchController::Origin::Detector));
    NoiseSource quiet;
    pumpQuietFor (h, quiet, 62000.0);   // from -24: 60 s to Clear, +2 s margin
    ASSERT_FALSE (h.controller.activeForTest (0, 0));

    // A candidate exactly ONE bin up. Memory must not answer for it.
    h.controller.setDetectionActive (true);
    SineSource neighbour; neighbour.freq = 44.0 * binHz;
    int placed = -1;
    for (int i = 0; i < 80 && placed < 0; ++i)
    {
        pump (h, neighbour.hop());
        placed = firstActiveIndex (h.controller, 0);
    }
    ASSERT_GE (placed, 0);
    EXPECT_GE (h.controller.depthDbForTest (0, placed), -12.0)
        << "a neighbouring bin inherited the remembered depth";
}

// RED IF the TTL stops expiring entries (spec 4.6). 5 minutes and one
// millisecond is a different show.
TEST (NotchControllerLadder, RoomMemoryExpiresAfterFiveMinutes)
{
    Harness h;
    h.controller.setNotchDefaults (30.0, -24.0);
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1007.8125, 30.0, -24.0,
                                        NotchController::Origin::Detector));
    NoiseSource quiet;
    pumpQuietFor (h, quiet, 62000.0);                       // -24: cleared, remembered
    ASSERT_FALSE (h.controller.activeForTest (0, 0));
    pumpQuietFor (h, quiet, NotchController::kMemoryTtlMs + 1000.0);   // expired

    h.controller.setDetectionActive (true);
    SineSource tone;
    int placed = -1;
    for (int i = 0; i < 80 && placed < 0; ++i)
    {
        pump (h, tone.hop());
        placed = firstActiveIndex (h.controller, 0);
    }
    ASSERT_GE (placed, 0);
    EXPECT_GE (h.controller.depthDbForTest (0, placed), -12.0)
        << "an expired entry was still used";
}

// RED IF a remembered entry can be used twice (spec 4.6). It describes ONE
// release; a second howl at that bin has to earn its own depth.
TEST (NotchControllerLadder, ARememberedEntryIsUsedOnlyOnce)
{
    Harness h;
    h.controller.setNotchDefaults (30.0, -24.0);
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1007.8125, 30.0, -24.0,
                                        NotchController::Origin::Detector));
    NoiseSource quiet;
    pumpQuietFor (h, quiet, 62000.0);   // from -24: 60 s to Clear, +2 s margin
    ASSERT_FALSE (h.controller.activeForTest (0, 0));

    h.controller.setDetectionActive (true);
    SineSource tone;
    int first = -1;
    for (int i = 0; i < 80 && first < 0; ++i)
    {
        pump (h, tone.hop());
        first = firstActiveIndex (h.controller, 0);
    }
    ASSERT_GE (first, 0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, first), -24.0);

    // Clear it by hand (no AutoRelease => no new memory entry) and let it
    // place again: the ladder must start from scratch.
    h.controller.clearNotch (0, first, NotchController::ClearReason::Manual);
    ASSERT_FALSE (h.controller.activeForTest (0, first));
    int second = -1;
    for (int i = 0; i < 80 && second < 0; ++i)
    {
        pump (h, tone.hop());
        second = firstActiveIndex (h.controller, 0);
    }
    ASSERT_GE (second, 0);
    EXPECT_GE (h.controller.depthDbForTest (0, second), -12.0)
        << "the entry was consumed twice";
}

// RED IF the ceiling stops capping a remembered depth (spec 4.3 step 4, m-8).
// A -24 memory under a -12 ceiling must place at -12. Under Q13 that is the
// ONLY thing the read path does to a remembered depth: no rung quantisation,
// because the ceiling IS the deepest legal rung. A memory holding an odd depth
// (a Manual -9 that auto-released) is therefore placed at -9 verbatim under a
// -24 ceiling -- correct, and covered by the second half below.
TEST (NotchControllerLadder, ARememberedDepthIsStillCappedByTheCeiling)
{
    Harness h;
    h.controller.setNotchDefaults (30.0, -24.0);
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1007.8125, 30.0, -24.0,
                                        NotchController::Origin::Detector));
    NoiseSource quiet;
    pumpQuietFor (h, quiet, 62000.0);   // from -24: 60 s to Clear, +2 s margin
    ASSERT_FALSE (h.controller.activeForTest (0, 0));

    h.controller.setNotchDefaults (30.0, -12.0);   // ceiling drops
    h.controller.setDetectionActive (true);
    SineSource tone;
    int placed = -1;
    for (int i = 0; i < 80 && placed < 0; ++i)
    {
        pump (h, tone.hop());
        placed = firstActiveIndex (h.controller, 0);
    }
    ASSERT_GE (placed, 0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, placed), -12.0);
}

// RED IF the read path snaps a remembered depth to a fixed rung (m-8). A
// Manual notch is placed at whatever the operator typed, so an entry can hold
// -9; under a ceiling that permits it, -9 is what comes back. Quantising to
// -6 would throw away 3 dB the room demonstrably needed, and quantising to -12
// would place DEEPER than the notch ever ran, which invariant 2 forbids.
TEST (NotchControllerLadder, AnOffRungRememberedDepthComesBackVerbatim)
{
    Harness h;
    h.controller.setNotchDefaults (30.0, -24.0);
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1007.8125, 30.0, -9.0,
                                        NotchController::Origin::Manual));
    NoiseSource quiet;
    pumpQuietFor (h, quiet, 42000.0);   // from -9: -6 at 30 s, Clear at 40 s
    ASSERT_FALSE (h.controller.activeForTest (0, 0));

    h.controller.setDetectionActive (true);
    SineSource tone;
    int placed = -1;
    for (int i = 0; i < 80 && placed < 0; ++i)
    {
        pump (h, tone.hop());
        placed = firstActiveIndex (h.controller, 0);
    }
    ASSERT_GE (placed, 0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, placed), -9.0);
}

// RED IF a room change stops wiping the memory (spec 4.6). setWidth is the
// boundary that actually fires in the shipping app -- MainComponent's
// onAfterRestart hook calls it on every engine restart, i.e. every device,
// rate and buffer change (NotchController.cpp:106-114).
TEST (NotchControllerLadder, SetWidthClearAllAndSetSampleRateWipeRoomMemory)
{
    for (int which = 0; which < 3; ++which)
    {
        Harness h;
        h.controller.setNotchDefaults (30.0, -24.0);
        ASSERT_TRUE (h.controller.setNotch (0, 0, 1007.8125, 30.0, -24.0,
                                            NotchController::Origin::Detector));
        NoiseSource quiet;
        pumpQuietFor (h, quiet, 62000.0);   // from -24: 60 s to Clear, +2 s margin
        ASSERT_FALSE (h.controller.activeForTest (0, 0)) << "which=" << which;

        if (which == 0) h.controller.setWidth (2);
        if (which == 1) h.controller.clearAll();
        if (which == 2) h.controller.setSampleRate (48000.0);

        h.controller.setDetectionActive (true);
        SineSource tone;
        int placed = -1;
        for (int i = 0; i < 80 && placed < 0; ++i)
        {
            pump (h, tone.hop());
            placed = firstActiveIndex (h.controller, 0);
        }
        ASSERT_GE (placed, 0) << "which=" << which;
        EXPECT_GE (h.controller.depthDbForTest (0, placed), -12.0)
            << "memory survived a room change, which=" << which;
    }
}

// RED IF a LINKED release leaves an orphan entry on one lane (M-11). One
// Clear writes both lanes; one placement consumes both.
TEST (NotchControllerLadder, LinkedReleaseWritesAndConsumesBothLanes)
{
    StereoHarness h;
    h.controller.setLinked (true);
    h.controller.setNotchDefaults (30.0, -24.0);
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1007.8125, 30.0, -24.0,
                                        NotchController::Origin::Detector));
    ASSERT_TRUE (h.controller.setNotch (1, 0, 1007.8125, 30.0, -24.0,
                                        NotchController::Origin::Detector));

    NoiseSource quietL, quietR; quietR.rng.seed (999u);
    // From -24: 30 + 10 + 10 + 10 = 60 s to Clear, +2 s margin.
    const int blocks = (int) std::lround (62000.0 / kBlockMs);
    for (int i = 0; i < blocks; ++i)
        pumpStereo (h, quietL.hop(), quietR.hop());
    ASSERT_FALSE (h.controller.activeForTest (0, 0));
    ASSERT_FALSE (h.controller.activeForTest (1, 0));

    h.controller.setDetectionActive (true);
    SineSource toneL, toneR;
    int placed = -1;
    for (int i = 0; i < 80 && placed < 0; ++i)
    {
        pumpStereo (h, toneL.hop(), toneR.hop());
        placed = firstActiveIndex (h.controller, 0);
    }
    ASSERT_GE (placed, 0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, placed), -24.0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (1, placed), -24.0);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --config Release && cd build && ctest -C Release -R NotchControllerLadder --output-on-failure`
Expected: FAIL — `AHowlReturningToTheSameBinStartsAtTheRememberedDepth` places at −12 (the steep-rise rung) instead of the remembered depth.

- [ ] **Step 3: Declare the memory in `src/app/NotchController.h`**

Next to the `pushRetuneLocked` declaration (Task 4's block):

```cpp
    // --- "Room memory" (spec 4.6, Q3/Q6/Q10) ------------------------------
    // What a bin needed LAST time, so a howl that comes back inside
    // kMemoryTtlMs is answered at the depth that killed it rather than
    // crawling up the ladder again while the room rings. Per lane, fixed size,
    // oldest entry overwritten -- no allocation, ever. Detector-thread reads
    // (placeConfirmed, the release path) and message-thread wipes (setWidth,
    // clearAll, setSampleRate) all go through modelMutex_, the lock every one
    // of those already takes: no new lock, no new order.
    //
    // NOT persisted. A preset describes a rig; this describes the last five
    // minutes of one room, and writing it to disk would let a soundman open a
    // file that silently deep-notches a frequency that is not ringing.
    struct MemoryEntry
    {
        double frequencyHz = 0.0;
        double deepestDb   = 0.0;
        double clearedAtMs = 0.0;
        bool   used        = false;   // false == empty slot
    };

    // modelMutex_ HELD. Records what `frequencyHz` needed. `bothLanes` writes
    // the pair (M-11: a LINKED clear must not leave an orphan on one lane).
    // An existing entry for the SAME frequency is merged rather than
    // duplicated -- both lanes of a linked pair clear in the same tick and
    // would otherwise each write twice.
    void   rememberReleaseLocked (int lane, double frequencyHz, double deepestDb, bool bothLanes);
    // modelMutex_ HELD. The remembered depth for (lane, bin), CONSUMING the
    // entry -- one use only. NaN when nothing matched. Bin equality is exact
    // (Q10): one bin away is a new howl.
    double takeRememberedDepthLocked (int lane, double frequencyHz, double binWidthHz, bool bothLanes);
    void   clearRoomMemoryLocked();
```

and, next to `model_` (line 454):

```cpp
    std::array<std::array<MemoryEntry, kMemoryEntriesPerLane>, kChannels> roomMemory_ {};
    std::array<int, kChannels> roomMemoryHead_ {};
```

- [ ] **Step 4: Implement in `src/app/NotchController.cpp`**

Add the three helpers after `pushRetuneLocked`:

```cpp
void NotchController::rememberReleaseLocked (int lane, double frequencyHz,
                                             double deepestDb, bool bothLanes)
{
    const int first = bothLanes ? 0 : lane;
    const int last  = bothLanes ? width_ - 1 : lane;
    for (int l = first; l <= last && l < kChannels; ++l)
    {
        auto& bank = roomMemory_[(std::size_t) l];

        // Merge onto an existing entry for the same frequency rather than
        // writing a second one. A LINKED pair clears on the same tick and both
        // lanes write both banks, so without this the ring fills with
        // duplicates and a lookup consumes one while leaving its twin behind.
        int target = -1;
        for (int k = 0; k < kMemoryEntriesPerLane; ++k)
            if (bank[(std::size_t) k].used
                && bank[(std::size_t) k].frequencyHz == frequencyHz)
            {
                target = k;
                break;
            }

        const bool merging = target >= 0;
        if (! merging)
        {
            target = roomMemoryHead_[(std::size_t) l];
            roomMemoryHead_[(std::size_t) l] = (target + 1) % kMemoryEntriesPerLane;
        }

        auto& e = bank[(std::size_t) target];
        // The DEEPEST of the two wins: whichever lane needed more is what the
        // room needed (M-11).
        e.deepestDb   = merging ? std::min (e.deepestDb, deepestDb) : deepestDb;
        e.frequencyHz = frequencyHz;
        e.clearedAtMs = liveMs_;
        e.used        = true;
    }
}

double NotchController::takeRememberedDepthLocked (int lane, double frequencyHz,
                                                   double binWidthHz, bool bothLanes)
{
    if (! (binWidthHz > 0.0))
        return std::numeric_limits<double>::quiet_NaN();

    const long targetBin = std::lround (frequencyHz / binWidthHz);
    const int  first = bothLanes ? 0 : lane;
    const int  last  = bothLanes ? width_ - 1 : lane;

    double best = std::numeric_limits<double>::quiet_NaN();
    for (int l = first; l <= last && l < kChannels; ++l)
        for (auto& e : roomMemory_[(std::size_t) l])
        {
            if (! e.used)
                continue;
            // Q10: SAME bin, +-0. One bin is 21.5 Hz at 44.1 kHz / 2048, and a
            // partial next door must not inherit a deep cut on its first block.
            if (std::lround (e.frequencyHz / binWidthHz) != targetBin)
                continue;
            if (liveMs_ - e.clearedAtMs > kMemoryTtlMs)
            {
                e.used = false;   // expired: drop it while we are here
                continue;
            }
            best   = std::isnan (best) ? e.deepestDb : std::min (best, e.deepestDb);
            e.used = false;       // one use only (spec 4.6)
        }
    return best;
}

void NotchController::clearRoomMemoryLocked()
{
    for (auto& bank : roomMemory_)
        for (auto& e : bank)
            e = MemoryEntry {};
    roomMemoryHead_.fill (0);
}
```

Wipe it at the three boundaries. In `setWidth`, after the `blocksSinceReset` loop (line 115-116):

```cpp
    for (auto& l : lanes_)
        l.blocksSinceReset = 0;

    // Same boundary, same reason (spec 4.6): a restart means a different
    // device, rate or room, and a remembered depth from the old one would be
    // applied to a bin that now means a different frequency.
    {
        const std::lock_guard<std::mutex> lock (modelMutex_);
        clearRoomMemoryLocked();
    }
```

In `clearAll` (lines 223-229), inside the existing lock:

```cpp
void NotchController::clearAll (ClearReason reason)
{
    const std::lock_guard<std::mutex> lock (modelMutex_);
    for (int c = 0; c < kChannels; ++c)
        for (int i = 0; i < kSlots; ++i)
            pushClearLocked (c, i, reason);
    // CLEAR ALL is the operator saying "forget everything you think you know
    // about this room" -- leaving the memory would have the next howl come
    // back deep-notched immediately.
    clearRoomMemoryLocked();
}
```

In `setSampleRate` (lines 864-877), after the lane loop:

```cpp
    // m-7: this has no production caller today (the device path reaches the
    // controller through setWidth), but the wipe belongs here for the day it
    // does -- and for tools/snapshot.cpp and the tests, which do call it.
    {
        const std::lock_guard<std::mutex> lock (modelMutex_);
        clearRoomMemoryLocked();
    }
```

In `placeConfirmed`, fill in the hook Task 5 left inside the index-lookup lock (B-2 — Task 5 already moved this block above the depth choice, so `remembered` is in scope by the time the depth choice reads it; the v1 plan had them the other way round and would not have compiled):

```cpp
    int index = -1, firstLane = lane, lastLane = lane;
    double remembered = std::numeric_limits<double>::quiet_NaN();
    {
        const std::lock_guard<std::mutex> lock (modelMutex_);
        if (linkedNow) { index = firstFreeIndexAllLanesLocked(); firstLane = 0; lastLane = width_ - 1; }
        else           { index = firstFreeIndexLocked (lane); }
        // Looked up only when a placement is actually going to happen: the
        // entry is CONSUMED by the lookup, and spending it on a placement that
        // then bails on a full chain would lose the room's history for nothing.
        if (index >= 0)
            remembered = takeRememberedDepthLocked (lane, cand.frequencyHz,
                                                    pc.sampleRate / (double) Detector::kFftSize,
                                                    linkedNow);
    }
    if (index < 0)
        return;   // chain full on the lanes concerned: same outcome as today
```

Then insert step 3 of §4.3 into Task 5's depth choice, at the marker it left. **The Soundcheck line stays LAST** — moving it above step 3, or dropping it as the v1 plan's replacement block did, reds `SoundcheckPlacesAtTheFullSliderDepth` and `SoundcheckNotchesNeverDeepen`. The final shape of the whole block:

```cpp
    double depthDb = kDepthLadderDb[0];                       // step 1: -6
    if (pc.breakdown.riseRatio >= kSteepRiseRatio)            // step 2: steep -> -12
        depthDb = kDepthLadderDb[1];
    // step 3 (spec 4.6): this bin howled before, recently, and we know what it
    // took. Skip the crawl.
    if (! std::isnan (remembered))
        depthDb = remembered;

    // step 4: never deeper than the ceiling, which under Q13 IS the deepest
    // rung. This is also the ONLY thing done to a remembered depth on read
    // (m-8): an entry may legitimately hold an off-rung value -- a Manual -9
    // that auto-released -- and max() clamps it to the current ceiling without
    // any rung quantisation. Snapping it to a rung would either throw away
    // depth the room needed or place DEEPER than the notch ever ran.
    depthDb = std::max (depthDb, ceiling);

    // KD-7, LAST: soundcheck has no ladder and no room memory.
    if (origin == Origin::Soundcheck)
        depthDb = ceiling;
```

Finally, in Task 7's release path, record before the Clear:

```cpp
                else
                {
                    // What this bin needed, before the record of it goes away
                    // with the notch (spec 4.6). ONLY on an AutoRelease: a
                    // manual clear, a CLEAR ALL, a width change, a FALSE
                    // verdict or an unwind are all statements that this notch
                    // should not have been there.
                    rememberReleaseLocked (c, n.frequency, n.deepestDb, effectiveLinked());
                    pushClearLocked (c, i, ClearReason::AutoRelease);
                }
```

- [ ] **Step 5: Run the controller tests**

Run: `cmake --build build --config Release && cd build && ctest -C Release -R NotchController --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Run the full suite**

Run: `cd build && ctest -C Release`
Expected: `100% tests passed` (519).

- [ ] **Step 7: Commit**

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add src/app/NotchController.h src/app/NotchController.cpp tests/test_notchcontroller.cpp
```
```bash
git commit -m "feat(controller): room memory -- a howl returning to the same bin skips the crawl"
```

---

### Task 9: the log and the preset learn about retunes

**Mức level dự kiến (spec §3):** **0 dB** on the audio path. `savePreset` changes what a saved file CONTAINS: a notch is written at its `deepestDb` instead of its current rung, so reloading a preset saved during a quiet moment now restores the depth the room actually needed — up to **18 dB deeper on reload** than a 1.1.3-format save of the same moment would have been, but never deeper than the ceiling on the next tick. And `notchDefaults` is now written, so a reloaded preset keeps its ceiling instead of silently falling back to `PresetNotchDefaults`' −12 dB (`PresetManager.h:146`) — which on a rig tuned at −18 was capping every detector notch two rungs too shallow.

**Files:**
- Modify: `src/app/MainComponent.cpp:54-77` (a **free** `retuneReasonName` in the same anonymous namespace as `originName` (`:54`) and `reasonName` (`:66`) — m-5: it is not a `MainComponent::` member, and declaring it as one will not link), `:559-612` (`notchEventToVar`), `:950-962` (`savePreset`'s notch loop), `:983-985` (`notchDefaults`)
- Modify: `src/app/MainComponent.h:188` (the `notchEventToVarForTest` wrapper goes in the **public** section, next to `getNotchControllerForTest`; m-4: `savePreset` is declared at `:103` and `notchEventToVar` is a private `static` at `:256`, so the wrapper cannot live beside either)
- Modify: `tools/logstats.py:43-64` (the event dispatch), `:91-118` (`print_report`), `:124-127` (the `--expect-*` flags in `main`)
- Modify: `tests/fixtures/session-sample.jsonl`, `tests/CMakeLists.txt:113-117` (m-1: `add_test(NAME logstats_fixture` starts on line **113**, inside the `if(Python3_Interpreter_FOUND)` opened at 112, and closes at 117 — verified by grep in this worktree on 2026-09-07)
- Test: `tests/test_gui_wiring.cpp`, `tests/test_presetmanager.cpp`

**Interfaces:**
- Consumes (Tasks 4, 7, 8): `NotchEvent::Kind::Retune`, `RetuneReason`, `fromDepthDb`, `SnapshotNotch::deepestDb`.
- Produces: the log schema `ev: "notch_retune"` with keys `slot`, `lane`, `index`, `hz`, `q`, `depth_db`, `origin`, `reason` (`deepen|release|reclamp|ceiling`), `from_db`, `age_ms`. Task 10 documents it.

- [ ] **Step 1: Write the failing tests**

In `tests/test_gui_wiring.cpp`, after `savePreset`'s existing lane test (around line 1129).

**M-1 — use this file's real helpers.** `TempDir`, `pumpOneBlockThroughSlotZero` and `notchControllerForTest` do not exist anywhere in the repo; the v1 plan invented all three and the tests would not have compiled. `SavePresetWritesAFileThatLoadPresetReopensIdentically` at `:1016-1041` is the shape to copy, and it is copied literally below:

- `const juce::ScopedJuceInitialiser_GUI juceInit;` is the **first line** of the test — constructing `MainComponent` without it is undefined;
- the accessor is `app.getNotchControllerForTest (0)` (declared `MainComponent.h:188`), and it returns a pointer that must be null-checked;
- there is no `TempDir`: the file is an explicit `juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile (...)`, `deleteFile()`d before the save and again after the load;
- a snapshot is published by hand — `controller0->setSampleRate (48000.0)`, then `app.getAudioEngine().getTapBuffer (0).write (hop.data(), hop.size())`, then `controller0->runOnce()`. The rate matters: `saveToFile` validates each notch's freq against the published rate's Nyquist, and `savePreset` refuses a preset whose rate is 0 (memory/preset-save-roundtrip-2026-09-05.md).

```cpp
// RED IF savePreset writes the RUNNING depth instead of deepestDb (Q11,
// M-10). A preset saved during a quiet moment must record what the room
// needed, not the rung the release ladder had wound back to.
TEST (GuiWiring, SavePresetRecordsTheDeepestDepthNotTheRestingOne)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    MainComponent app;
    auto* controller0 = app.getNotchControllerForTest (0);
    ASSERT_NE (controller0, nullptr);
    controller0->setSampleRate (48000.0);

    // A notch placed deep, then wound back the way the release ladder does.
    ASSERT_TRUE (controller0->setNotch (0, 0, 1000.0, 30.0, -18.0,
                                        NotchController::Origin::Detector));
    ASSERT_TRUE (controller0->retuneForTest (0, 0, -6.0,
                                             NotchController::RetuneReason::Release));

    std::vector<float> hop (512, 0.25f);
    app.getAudioEngine().getTapBuffer (0).write (hop.data(), hop.size());
    controller0->runOnce();          // publish a snapshot carrying deepestDb

    auto outFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("az-handsfree-laneg-deepest.json");
    outFile.deleteFile();
    ASSERT_TRUE (app.savePreset (outFile));

    const auto result = PresetManager::loadFromFile (outFile);
    outFile.deleteFile();

    ASSERT_TRUE (result.ok) << result.errors.joinIntoString ("; ");
    // One entry per (slot, lane, index) since lane S, and setNotch named lane
    // 0 only -- so exactly one notch comes back.
    ASSERT_EQ (result.preset.notches.size(), 1u);
    EXPECT_DOUBLE_EQ (result.preset.notches[0].depthDB, -18.0);
}

// RED IF notchDefaults stops round-tripping (Q11). Without it a reloaded
// preset caps every detector notch at PresetNotchDefaults' -12 dB, silently
// undoing the ceiling the show was tuned at.
TEST (GuiWiring, SavePresetRoundTripsTheCeilingThroughNotchDefaults)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    MainComponent app;
    auto* controller0 = app.getNotchControllerForTest (0);
    ASSERT_NE (controller0, nullptr);
    controller0->setSampleRate (48000.0);
    controller0->setNotchDefaults (44.0, -24.0);
    ASSERT_TRUE (controller0->setNotch (0, 0, 1000.0, 44.0, -24.0,
                                        NotchController::Origin::Detector));

    std::vector<float> hop (512, 0.25f);
    app.getAudioEngine().getTapBuffer (0).write (hop.data(), hop.size());
    controller0->runOnce();

    auto outFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("az-handsfree-laneg-ceiling.json");
    outFile.deleteFile();
    ASSERT_TRUE (app.savePreset (outFile));

    const auto result = PresetManager::loadFromFile (outFile);
    outFile.deleteFile();

    ASSERT_TRUE (result.ok) << result.errors.joinIntoString ("; ");
    EXPECT_DOUBLE_EQ (result.preset.notchDefaults.Q,       44.0);
    EXPECT_DOUBLE_EQ (result.preset.notchDefaults.depthDB, -24.0);
}

// RED IF a Retune event is serialised as notch_clear (B-3 of the spec's own
// critique list). The `ev` key is what logstats.py dispatches on, and a
// mislabelled retune closes the notch's record at the first 300 ms deepening.
// No MainComponent is needed: the serialiser is static.
TEST (GuiWiring, RetuneEventsAreLoggedUnderTheirOwnEventName)
{
    NotchController::NotchEvent e;
    e.kind = NotchController::NotchEvent::Kind::Retune;
    e.slot = 0; e.lane = 1; e.index = 3;
    e.hz = 1007.8f; e.q = 30.0f;
    e.fromDepthDb = -12.0f; e.depthDb = -18.0f;
    e.origin = NotchController::Origin::Detector;
    e.retuneReason = NotchController::RetuneReason::Deepen;
    e.ageMs = 612.5;

    const juce::var v = MainComponent::notchEventToVarForTest (e);
    EXPECT_EQ (v["ev"].toString(), "notch_retune");
    EXPECT_EQ (v["reason"].toString(), "deepen");
    EXPECT_DOUBLE_EQ ((double) v["from_db"], -12.0);
    EXPECT_DOUBLE_EQ ((double) v["depth_db"], -18.0);
    EXPECT_DOUBLE_EQ ((double) v["age_ms"], 612.5);
    EXPECT_EQ ((int) v["lane"], 1);
    EXPECT_EQ ((int) v["index"], 3);
}

// RED IF a reason name is dropped or renamed -- logstats.py and the tester
// notes both read these four strings.
TEST (GuiWiring, EveryRetuneReasonHasItsOwnName)
{
    using RR = NotchController::RetuneReason;
    const RR reasons[] = { RR::Deepen, RR::Release, RR::Reclamp, RR::Ceiling };
    const char* names[] = { "deepen", "release", "reclamp", "ceiling" };
    for (int i = 0; i < 4; ++i)
    {
        NotchController::NotchEvent e;
        e.kind = NotchController::NotchEvent::Kind::Retune;
        e.retuneReason = reasons[i];
        EXPECT_EQ (MainComponent::notchEventToVarForTest (e)["reason"].toString(),
                   juce::String (names[i]));
    }
}
```

`notchEventToVar` is a private `static` at `MainComponent.h:256`; expose it the way the file already exposes other internals — in the **public** section, next to `getNotchControllerForTest` at `:188` (m-4):

```cpp
    NotchController* getNotchControllerForTest (int slot);

    // TEST ACCESSOR ONLY -- the serialiser is otherwise reachable only through
    // a live detector thread and a real log file. Public here, next to the
    // controller accessor, because notchEventToVar itself is private (:256)
    // and stays that way.
    static juce::var notchEventToVarForTest (const NotchController::NotchEvent& e)
        { return notchEventToVar (e); }
```

In `tests/test_presetmanager.cpp`, after `NotchDefaultsSurviveTheRoundTrip` — which starts at line **496**, not 497 (grepped in this worktree on 2026-09-07; the cross-check's m-2 was one off):

```cpp
// RED IF the deepest rung of the lane-G ladder stops round-tripping. -24 dB
// is a legal ceiling and a legal notch depth, and a preset that cannot carry
// it caps the app two rungs shallower than the operator asked for.
TEST (PresetManager, TheFullLadderRangeSurvivesTheRoundTrip)
{
    Preset p;
    p.sampleRate = 48000.0;
    p.notchDefaults.Q       = 30.0;
    p.notchDefaults.depthDB = -24.0;
    PresetNotch n;
    n.index = 0; n.freq = 1000.0; n.Q = 30.0; n.depthDB = -24.0; n.slot = 0; n.lane = 0;
    p.notches.push_back (n);

    const auto result = PresetManager::fromJSON (PresetManager::toJSON (p));
    ASSERT_TRUE (result.ok) << result.errors.joinIntoString ("; ");
    EXPECT_DOUBLE_EQ (result.preset.notchDefaults.depthDB, -24.0);
    ASSERT_EQ (result.preset.notches.size(), 1u);
    EXPECT_DOUBLE_EQ (result.preset.notches[0].depthDB, -24.0);
}
```

- [ ] **Step 2: Extend the log fixture and its test**

**M-7 — the fixture has to tell a story that could actually have happened.** The v1 lines said `from_db:-12, depth_db:-18, reason:deepen` for the lane-1 index-0 notch, whose `notch_set` on line 4 already reads `depth_db:-18`: it deepens FROM a depth it never held, TO the depth it was placed at. A fixture that contradicts itself is worse than none — it is what the next reader will copy. Make the whole life of that notch consistent with the lane-G ladder:

1. **Edit line 4** (`notch_set`, `t: 5001.0`, lane 1, index 0): `"depth_db":-18` → `"depth_db":-6`. That is a lane-G placement — the shallowest rung. Nothing else on the line changes; `rise:1` is rNorm, which saturates, so it is consistent with either band.
2. **Insert after line 4** — a deepen 300 ms later, the `kDeepenAfterMs` gate exactly:

```
{"ev":"notch_retune","t":5301.0,"slot":0,"lane":1,"index":0,"hz":1007.8,"q":30,"depth_db":-12,"from_db":-6,"origin":"detector","reason":"deepen","age_ms":300.0}
```

3. **Insert after line 11** (the `verdict` at `t: 31000.0`), so the file stays in ascending `t` order — a release, 10 s before the `auto_release` that already sits at `t: 41000.0`, which is exactly `kReleaseStepMs`:

```
{"ev":"notch_retune","t":31000.0,"slot":0,"lane":1,"index":0,"hz":1007.8,"q":30,"depth_db":-6,"from_db":-12,"origin":"detector","reason":"release","age_ms":25999.0}
```

The notch now reads: placed −6 at 5001, deepened to −12 at 5301, released back to −6 at 31000, cleared by `auto_release` at 41000 with `age_ms:36000` — which is what the existing line 12 already says. A reader that CLOSES on a retune loses that clear and the fixture's counts move, which is what the `--expect-*` flags below catch.

Then extend the fixture test in `tests/CMakeLists.txt:113-117`:

```cmake
    add_test(NAME logstats_fixture
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/logstats.py
                ${CMAKE_SOURCE_DIR}/tests/fixtures/session-sample.jsonl
                --expect-notches 4 --expect-verdicts 3 --expect-false 1
                --expect-recurrence-max 2 --expect-retunes 2)
```

- [ ] **Step 3: Run to verify it fails**

Run: `cmake --build build --config Release 2>&1 | tail -5`
Expected: compile error — `notchEventToVarForTest` and `retuneForTest` unresolved on `MainComponent`.

Run: `python tools/logstats.py tests/fixtures/session-sample.jsonl --expect-notches 4 --expect-verdicts 3 --expect-false 1 --expect-recurrence-max 2 --expect-retunes 2`
Expected: FAIL — `unrecognized arguments: --expect-retunes`. Then, with that flag removed, it exits 0 and reports nothing new: m-7 — the current reader does **not** miscount, it **ignores** the two lines. `summarise` is an `if/elif` chain on `ev` with no `else` (`tools/logstats.py:45-63`), so an unknown name simply falls through: `notches` stays 4 and every other total is unchanged. That is the correct behaviour for an old reader meeting a new log, and the branch added in Step 5 must preserve it. The defect being fixed is not a wrong number — it is a **blind spot**: the depth column shows the placement depth forever, and the retunes are invisible.

- [ ] **Step 4: Implement in `src/app/MainComponent.cpp`**

Add next to `reasonName` (line 66) — m-5: a **free** function in the same anonymous namespace as `originName` (`:54`) and `reasonName` (`:66`), with no `MainComponent::` qualifier. `notchEventToVar` calls it unqualified from the same translation unit; writing it as a member would need a header declaration it does not have and would not link.

```cpp
const char* retuneReasonName (NotchController::RetuneReason r)
{
    switch (r)
    {
        case NotchController::RetuneReason::Deepen:  return "deepen";
        case NotchController::RetuneReason::Release: return "release";
        case NotchController::RetuneReason::Reclamp: return "reclamp";
        case NotchController::RetuneReason::Ceiling: return "ceiling";
    }
    return "deepen";
}
```

Replace the head of `notchEventToVar` (lines 559-577):

```cpp
juce::var MainComponent::notchEventToVar (const NotchController::NotchEvent& e)
{
    using Ev = NotchController::NotchEvent;
    // B-3: `ev` is the key tools/logstats.py dispatches on, and the ternary
    // this replaced would have written every Retune as a notch_clear -- which
    // closes the notch's record at its first 300 ms deepening and makes every
    // deepened notch look like a 300 ms false positive.
    const char* evName = e.kind == Ev::Kind::Set    ? "notch_set"
                       : e.kind == Ev::Kind::Retune ? "notch_retune"
                                                    : "notch_clear";
    auto v = SessionLogger::makeEvent (evName);
    auto* o = v.getDynamicObject();
    o->setProperty ("slot", e.slot);
    o->setProperty ("lane", e.lane);
    o->setProperty ("index", e.index);
    o->setProperty ("hz", (double) e.hz);
    o->setProperty ("q", (double) e.q);
    o->setProperty ("depth_db", (double) e.depthDb);
    o->setProperty ("origin", originName (e.origin));

    if (e.kind == Ev::Kind::Retune)
    {
        o->setProperty ("reason", retuneReasonName (e.retuneReason));
        o->setProperty ("from_db", (double) e.fromDepthDb);
        o->setProperty ("age_ms", e.ageMs);
        return v;   // no score, no ctx: a retune is not a placement decision
    }

    if (e.kind == Ev::Kind::Clear)
    {
        o->setProperty ("reason", reasonName (e.reason));
        o->setProperty ("age_ms", e.ageMs);
        return v;
    }
```

In `savePreset`, line 958:

```cpp
            // Q11 / M-10: the depth the room NEEDED, not the rung the release
            // ladder happens to be resting on when SAVE was pressed. A preset
            // saved during a quiet stretch would otherwise reload two rungs
            // too shallow and let the same howl come back.
            pn.depthDB = sn.deepestDb;
```

and after `preset.sampleRate = presetRate;` (line 985):

```cpp
    // Q11: the CEILING round-trips too. Without this a reloaded preset falls
    // back to PresetNotchDefaults' -12 dB (PresetManager.h:146) and caps every
    // detector notch two rungs shallower than the show was tuned at -- a bug
    // that predates lane G and that lane G's ladder makes load-bearing.
    preset.notchDefaults.Q       = notchControllers_[0]->getNotchQ();
    preset.notchDefaults.depthDB = notchControllers_[0]->getNotchDepthDb();
```

- [ ] **Step 5: Implement in `tools/logstats.py`**

In `summarise`, extend the `notch_set` record and add the branch (lines 48-63):

```python
        if ev == "notch_set":
            n = {"slot": e.get("slot"), "lane": e.get("lane"), "index": e.get("index"),
                 "hz": float(e.get("hz", 0.0)), "origin": e.get("origin", "?"),
                 "set_t": float(e.get("t", 0.0)), "clear_t": None, "reason": None,
                 "verdict": None, "score": e.get("score"),
                 # lane G: the depth a notch is RUNNING at, and how many times
                 # it moved. Both start at the placement values.
                 "depth_db": e.get("depth_db"), "deepest_db": e.get("depth_db"),
                 "retunes": 0}
            notches.append(n)
            open_by_key[key(e)] = n
        elif ev == "notch_retune":
            # lane G: a retune UPDATES the open record. It must never close it
            # -- a deepening 300 ms after placement would otherwise read as a
            # 300 ms notch, and every deepened howl in the log would look like
            # a false positive. An unknown ev name falls through every branch
            # here, so a NEW event added later cannot corrupt an old reader
            # either; that is why this is an if/elif chain and not a lookup
            # that raises.
            n = open_by_key.get(key(e))
            if n is not None:
                n["depth_db"] = e.get("depth_db")
                if n["deepest_db"] is None or (e.get("depth_db") is not None
                                               and float(e["depth_db"]) < float(n["deepest_db"])):
                    n["deepest_db"] = e.get("depth_db")
                n["retunes"] += 1
        elif ev == "verdict":
```

Add the total to the returned dict (line 82-88):

```python
    return {
        "header": header, "duration_ms": duration, "modes": modes, "notches": notches,
        "groups": sorted(groups, key=lambda g: -g["count"]),
        "verdicts": len(judged), "false": false_count,
        "unjudged": len(notches) - len(judged),
        "retunes": sum(n["retunes"] for n in notches),
        "dropped": end.get("dropped_events"),
    }
```

In `print_report`, widen the table (lines 98-103):

```python
    print(f"{'#':>3} {'slot':>4} {'lane':>4} {'hz':>8} {'origin':<10} {'depth':>7} {'deep':>6} "
          f"{'rt':>3} {'held':>8} {'verdict':<8} {'cleared by':<20}")
    for i, n in enumerate(s["notches"], 1):
        held = (n["clear_t"] if n["clear_t"] is not None else s["duration_ms"]) - n["set_t"]
        lane = "R" if n["lane"] == 1 else "L"
        depth = "?" if n["depth_db"] is None else f"{float(n['depth_db']):.0f}dB"
        deep = "?" if n["deepest_db"] is None else f"{float(n['deepest_db']):.0f}dB"
        print(f"{i:>3} {n['slot']:>4} {lane:>4} {n['hz']:>8.1f} {n['origin']:<10} "
              f"{depth:>7} {deep:>6} {n['retunes']:>3} {fmt_ms(held):>8} "
              f"{(n['verdict'] or '-'):<8} {(n['reason'] or 'still active'):<20}")
```

and add the total to the summary line (line 114-116):

```python
    print(f"notches {total}  retunes {s['retunes']}  judged {v}  false {s['false']}"
          f"  false-rate {(s['false'] / v * 100 if v else 0):.0f}%"
          f"  unjudged {s['unjudged']} ({(s['unjudged'] / total * 100 if total else 0):.0f}%)")
```

In `main`, add the flag and its check:

```python
    ap.add_argument("--expect-recurrence-max", type=int)
    ap.add_argument("--expect-retunes", type=int)
```

```python
    if args.expect_retunes is not None and s["retunes"] != args.expect_retunes:
        failures.append(f"retunes {s['retunes']} != {args.expect_retunes}")
```

The file is already opened with `encoding="utf-8"` in `load()` (line 19) and writes nothing back — repo rule 6 is satisfied; do not change that call.

- [ ] **Step 6: Run the log tool by hand, then the suites**

Run: `python tools/logstats.py tests/fixtures/session-sample.jsonl --expect-notches 4 --expect-verdicts 3 --expect-false 1 --expect-recurrence-max 2 --expect-retunes 2`
Expected: exit 0, and the table's first row shows `-6dB` running / `-12dB` deepest / `2` retunes with `held` still ~36 s (5001 → 41000) — the record stayed OPEN across both retunes.

Run:
```
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
cd build && ctest -C Release -R "GuiWiring|PresetManager|logstats" --output-on-failure
```
Expected: PASS.

- [ ] **Step 7: Run the full suite**

Run: `cd build && ctest -C Release`
Expected: `100% tests passed` (525).

- [ ] **Step 8: Commit**

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add src/app/MainComponent.h src/app/MainComponent.cpp tools/logstats.py tests/fixtures/session-sample.jsonl tests/CMakeLists.txt tests/test_gui_wiring.cpp tests/test_presetmanager.cpp
```
```bash
git commit -m "feat(app): log notch_retune, save deepestDb, round-trip the notch ceiling"
```

---

### Task 10: docs, tester notes, release notes, memory, roadmap

**Mức level dự kiến:** **0 dB** — no code changes. This task exists because a behaviour change nobody wrote down is a behaviour change the next session will "fix" back. Every level figure quoted below must be copied from spec §3 and from the number `NotchChain.MeasuredAttenuationMatchesEveryLadderRungWithinHalfADecibel` actually printed in Task 2, not invented here.

**Two claims this task must carry BECAUSE no test can (M-3, §5.4).** Both belong in the tester notes, phrased as things to listen for rather than as facts already established:

1. **"The ladder stops at the first rung that quiets the bin" (Q7) is a RIG claim.** The test harness writes the raw tone into `h.tap` (`tests/test_notchcontroller.cpp:344-349`), so the analyser never sees a notched spectrum and the ladder always climbs to the ceiling in the suite. On a real rig it should frequently stop at −6 or −12 — that is the tone win this lane was asked for. If it never does, if every notch on the rig ends at the ceiling, the reinforce loop is reading the wrong spectrum: that is a bug report, not a preference.
2. **The snapshot image** (`console-live.png`, Step 9) is the only check anywhere that the ACTIVE NOTCHES depth column reads a ladder rung at all.

**Files:**
- Modify: `docs/GIOI-THIEU.md:28`, `:41`, `:45`, `:50`
- Modify: `docs/KY-THUAT-CHONG-HU.md:32`, `:79-92`, `:177`, `:193-196`, `:225`, `:280-285`, `:322-326`, `:350-360`, `:389`
- Modify: `docs/spec-ring-risk.md` (new section before "## Out of scope")
- Modify: `docs/superpowers/specs/2026-09-05-data-loop-design.md:103-111`
- Modify: `docs/superpowers/specs/2026-09-04-anti-feedback-v2-roadmap.md:23`, `:70`, and lane A's row
- Modify: `docs/superpowers/specs/2026-09-06-gain-aware-notch-design.md:5` (status line)
- Modify: `installer/TESTER-NOTES.md` (header block + a new "Mới trong 1.2.0" section)
- Create: `docs/release-notes/1.2.0-alpha.md`
- Create: `memory/gain-aware-notch-lane-g-2026-09-07.md`; modify `memory/MEMORY.md`

**Interfaces:** none — prose only.

- [ ] **Step 1: `docs/GIOI-THIEU.md`**

Line 28, inside the overview diagram, replace `tự nhả filter khi hết hú`:
```
                            nhả dần từng bậc khi hết hú
```

Line 41, the **Notch siêu hẹp** row — the depth default is now a ceiling:
```
| **Notch siêu hẹp** | 16 notch/làn, mỗi slot mono (1 làn) hoặc stereo (2 làn), 8 slot — tối đa 256 chuỗi notch. Q chỉnh được 8–50 (mặc định 30). Độ sâu −6 đến −24 dB (mặc định −18) nay là **TRẦN**: notch đặt ở −6 dB (hoặc −12 nếu đỉnh lên dốc) rồi chỉ đào sâu thêm 6 dB mỗi 300 ms chừng nào bin đó còn hú, không bao giờ quá trần. Chỉ mất đúng vài Hz quanh tần số hú |
```

Line 45, replace the **Tự nhả sau 30 giây** row:
```
| **Nhả dần theo bậc** | Hết hú 30 giây → nông đi 6 dB; mỗi 10 giây yên tiếp theo nông thêm một bậc; tới −6 dB thì nhả hẳn (tổng ~50 giây). Hú quay lại là kẹp ngay về bậc sâu nhất đã từng đứng. Phòng đang căng (chip RING RISK ≥ RISING) thì đồng hồ **đứng yên** — notch giữ nguyên bậc chừng nào phòng còn căng |
| **Nhớ phòng 5 phút** | Hú quay lại đúng bin cũ trong 5 phút sau khi nhả hẳn → đặt lại thẳng ở độ sâu đã từng cần, không dò lại từ −6 dB |
```

Line 50, the presets row — say the two numbers are ceilings now:
```
| **Preset có sẵn** | `Speech` (Q=40, trần −18 dB — hà khắc cho loa hội thoại) và `Music` (Q=25, trần −10 dB — dịu cho nhạc sống) đúng giá trị trong repo. Trần −10 của Music **tới được**: theo Q13 thang hiệu lực là −6 → −10, bậc cuối chính là trần, nên Music vẫn cắt đủ 10 dB như 1.1.3 chứ không bị lượng tử về −6. Installer chép hai preset vào máy, app tự seed chúng vào `%APPDATA%` lúc first-run (không bao giờ ghi đè file người dùng đã sửa), và GUI có hai nút **LOAD… / SAVE…** dưới mục INTERFACE để nạp/lưu preset (`*.json`). Từ 1.2.0 file lưu ra mang **độ sâu phòng đã cần** (`deepestDb`) và mang cả trần trong `notchDefaults`, nên nạp lại đúng như lúc lưu. |
```

- [ ] **Step 2: `docs/KY-THUAT-CHONG-HU.md`**

Line 32, the mermaid box: `auto-release 30 s` → `thang nhả 30 s + 10 s/bậc`.

Line 225, the mermaid note: `Khóa khi vượt ngưỡng,<br/>tự nhả sau 30 s im` → `Đặt nông rồi đào sâu theo nhu cầu,<br/>nhả dần từng bậc khi im`.

Section **### Bộ lọc notch và độ sâu** (lines 79-92) — append after the existing `depthDB > 0` paragraph:

```markdown
**Đổi độ sâu trên notch đang chạy (1.2.0).** `Biquad::setNotchFilter` gọi
`reset()` mỗi lần đổi hệ số, đúng khi tần số hoặc Q đổi và **sai** khi chỉ độ
sâu đổi: xóa `z1/z2` giữa dòng tín hiệu là một bước nhảy vào loa.
`Biquad::rampNotchDepth` giữ nguyên state và nội suy tuyến tính 5 hệ số trong
`NotchChain::kRampMs` = **10 ms** (≤ 0,6 dB/ms). `NotchChain::setNotch` chỉ đi
đường ramp khi slot đang Active **và** `freq`, `Q` bằng đúng giá trị đã lưu;
mọi trường hợp khác vẫn reset như cũ.

Vì sao bộ hệ số nội suy an toàn: cố định `freq/Q/sr`, mọi tổ hợp lồi của các bộ
peaking đã chuẩn hóa vẫn **là** một peaking RBJ với gain tử `A_n ≤ 1 ≤ 1/A_d`,
nên `|H| ≤ 1` ở **mọi** tần số — ramp không thể khuếch đại gì, kể cả tần số nó
đang nhắm — và bán kính cực `sqrt((1 − α/A_d)/(1 + α/A_d)) < 1` nên không phân
kỳ. Đo 06/09/2026: gain lớn nhất 1,9e-15 dB trên 3 sample rate × 5 tần số × 3 Q
× 7 cặp độ sâu × 101 điểm, và 20 000 tổ hợp lồi ngẫu nhiên × 400 tần số. Test
`Biquad.RampMidpointsNeverBoostAnyFrequency` chốt điều này bằng cách đọc hệ số
**đang chạy** giữa ramp rồi tính `|H|` giải tích.

Suy giảm đo được tại f0 sau khi ramp xong, từng bậc, sai số ±0,5 dB:
`NotchChain.MeasuredAttenuationMatchesEveryLadderRungWithinHalfADecibel`.
```

Line 177, the RESPONSE preset line — note that the depth figure is a ceiling:
```
SAFE (500 ms/4/trần −12 dB/Q40/thr 12.0), BALANCED (250/3/trần −18/30/10.0), AGGRESSIVE
```

Section **### 3.4 NotchController**, lines 193-196 — replace the whole "Auto-release 30 s" paragraph:

```markdown
**Thang độ sâu (1.2.0, lane G).** Depth không còn là một số cố định đọc lúc
đặt. Bậc cố định: `kDepthLadderDb = {−6, −12, −18, −24}`; slider độ sâu của
preset là **trần**, đọc **sống** mỗi tick. **Thang hiệu lực (Q13) = các bậc cố
định NÔNG HƠN trần, cộng chính trần làm bậc cuối**: trần −10 (`presets/Music.json`)
⇒ −6 → −10; trần −13,7 ⇒ −6 → −12 → −13,7; trần −18 ⇒ −6 → −12 → −18; trần −6
⇒ không bao giờ đào. Bậc cuối = trần, có thể là số lẻ — đây là chỗ DUY NHẤT một
notch Detector không đứng trên bậc cố định. Không có bước lượng tử hóa nào:
lượng tử trần −10 xuống −6 sẽ làm Music nông hơn 1.1.3 4 dB mà không ai báo.

- **Đặt**: −6 dB, hoặc −12 nếu `riseRatio ≥ 2.0` (đỉnh lên ≥ 6 dB trong cửa sổ
  rise), rồi kẹp về trần. Notch Soundcheck không có thang (KD-7).
- **Đào**: trong vòng reinforce, bin còn vượt ngưỡng peakiness ⇒ sâu thêm một
  bậc mỗi `kDeepenAfterMs` = 300 ms, dừng ở trần (bước cuối có thể NHỎ hơn
  6 dB: trần −10 thì bước cuối là 4 dB). Tiêu chí đào và tiêu
  chí "còn hú" là **cùng một phép thử trên phổ SAU notch**, nên thang dừng ở
  bậc đầu tiên làm bin hết vượt ngưỡng — có thể là −6 hoặc −12 suốt đời notch.
  Đó là chủ ý (Q7): độ sâu theo nhu cầu, không theo mặc định. **Không test nào
  trong suite chứng minh được điều này** (M-3): harness ghi tone THÔ vào
  `h.tap` (`tests/test_notchcontroller.cpp:344-349`) nên analyser không bao giờ
  thấy phổ đã bị notch, và trong test thang luôn leo tới trần. Chỉ dàn thật
  kiểm chứng được — nêu trong tester notes, đừng dựng test giả cho nó.
- **Nhả**: `quietMs` tích lũy khi bin im; ≥ 30 s ⇒ nông một bậc, mỗi 10 s tiếp
  theo một bậc nữa, tới −6 thì `Clear(AutoRelease)`. Đồng hồ **đóng băng** khi
  `frameScoreValid_ && frameMaxScore_ ≥ 0.55 × kConfirmScore` (băng RISING của
  chip), không có trần thời gian (Q9); cờ `SnapshotBuffer::releaseFrozen`
  publish ra nhưng 1.2.0 chưa vẽ.
- **Kẹp lại**: bin hú lại khi đang nhả ⇒ về `deepestDb` **ngay trong frame
  đó**, không chờ 300 ms.
- **Nhớ phòng**: mỗi làn 16 mục `{tần số, deepestDb, thời điểm nhả}`, TTL 5
  phút, khớp **đúng cùng bin** (±0 — lệch một bin là 21,5 Hz @44,1k/2048, đủ
  để một partial nhạc cụ bên cạnh kế thừa nhầm một vết cắt sâu). Dùng một lần.
  Xóa sạch khi `setWidth` / `clearAll` / `setSampleRate`. Không persist ra đĩa.
- Mọi lần đổi độ sâu đi qua `pushRetuneLocked` — anh em của `pushClearLocked`,
  gọi khi đã cầm `modelMutex_`. Không gọi `setNotch`/`setNotchImpl` từ đó
  được: mutex **không đệ quy**, và `setNotchImpl` sẽ ghi đè `lockedAtMs`, tức
  nhãn tuổi mà lane D ghi vào mọi `notch_clear`.

Notch Soundcheck vẫn **miễn trừ** mọi thứ ở trên (KD-7) — chỉ nhả qua
`clearNotch`/`clearAll` tường minh.
```

Section **## 4. Preset** (around line 285) — append:

```markdown
Từ 1.2.0 `savePreset` ghi `deepestDb` (độ sâu phòng đã cần) chứ không phải bậc
đang đứng lúc bấm SAVE, và ghi cả `notchDefaults {Q, depth}` từ tuning đang
chạy — trước đó `savePreset` không set `notchDefaults`, nên nạp lại rơi về mặc
định −12 dB của `PresetNotchDefaults` và trần bị hạ hai bậc mà không ai báo.
Độ sâu sâu hơn −24 dB trong file bị **kẹp về −24** lúc adopt, cho mọi Origin,
và `adoptPreset` ghi một dòng `juce::Logger` đếm số notch bị kẹp (Q12).
```

Section **## 5. Tổng hợp các giới hạn an toàn** (line 322-326) — add three rows:

```markdown
| Đổi độ sâu giữa dòng tín hiệu thành tiếng "cạch" | `Biquad::rampNotchDepth` giữ state, nội suy 10 ms; chỉ đi đường này khi cùng `freq`/`Q` |
| Ramp khuếch đại giữa chừng | Chứng minh tổ hợp lồi `A_n ≤ 1 ≤ 1/A_d` + test đo `\|H\|` tại 5 điểm giữa ramp |
| Notch sâu hơn −24 dB từ file preset | Kẹp ở `setNotchImpl` cho MỌI Origin, có log (Q12); `pushRetuneLocked` từ chối |
```

Section **## 7. Log session** (line 359-360) — add the row and amend `notch_set`:

```markdown
| `notch_retune` | lane G đổi độ sâu một notch đang chạy | slot, làn, index, Hz, Q, `depth_db` (mới), `from_db` (cũ), `origin`, `reason`: `deepen` / `release` / `reclamp` / `ceiling`, `age_ms` (tính từ lúc ĐẶT, không phải từ lần retune trước) |
```

Section **## 8. Trạng thái & kiểm chứng** (line 389) — replace the version line with 1.2.0 and the suite count the final run reports.

- [ ] **Step 3: `docs/spec-ring-risk.md`**

Insert before `## Out of scope`:

```markdown
## Người đọc thứ hai: thang nhả lane G (1.2.0)

Cho tới 1.1.3 chỉ GUI đọc `ringRiskScore`. Từ 1.2.0 `NotchController` tự đọc
**số của chính nó** — `frameMaxScore_` / `frameScoreValid_`, hai member chỉ
sống trên detector thread và chứa đúng hai số vừa publish vào snapshot — để
**đóng băng đồng hồ nhả** khi `frameScoreValid_ && frameMaxScore_ ≥ 0.55 ×
CandidateScorer::kConfirmScore`, tức đúng ranh RISING của
`SpectrumView::riskForScore`. Hằng số là MỘT: `NotchController::
kRiskFreezeFraction` và `SpectrumView::kRingRiskRisingFraction` cùng giá trị
0.55 và cùng ý nghĩa; đổi một chỗ phải đổi chỗ kia.

Ba điều cần biết khi đọc chip cạnh thang nhả:

- **Không lấy `snapshotMutex_`.** Hôm nay `modelMutex_` và `snapshotMutex_`
  chưa bao giờ lồng nhau; đọc snapshot từ trong vòng nhả sẽ tạo thứ tự khóa
  mới cho một readout — không đáng (M-5). Cờ `releaseFrozen` được publish
  trong scope RIÊNG, **trước** khi lấy `modelMutex_`.
- **Chip và đồng hồ có thể lệch nhau** (M-8). Chip còn qua hold 750 ms và chỉ
  đọc `displayedSlot_`, nên chip có thể báo RISING khi đồng hồ đã chạy lại, và
  một slot không hiển thị có thể đang đóng băng mà màn hình không nói gì.
  `SnapshotBuffer::releaseFrozen` tồn tại để sau này nói được điều đó — 1.2.0
  publish nhưng chưa vẽ (Q9).
- **`ringRiskValid == false` KHÔNG đóng băng** (invariant 7). Tắt detection
  phải nhả y như 1.1.3, nếu không thì tắt detection sẽ giam mọi notch lại.
- **Không có trần thời gian cho đóng băng** (Q9). Phòng còn căng thì notch còn
  giữ bậc, không giới hạn.
```

- [ ] **Step 4: `docs/superpowers/specs/2026-09-05-data-loop-design.md`**

In the §3.2 table (lines 103-111), insert after the `notch_set` row:

```markdown
| `notch_retune` | NotchController (detector thread) | `slot`, `lane`, `index`, `hz`, `q`, `depth_db` (mới), `from_db` (cũ), `origin`, `reason`: `deepen` / `release` / `reclamp` / `ceiling`, `age_ms` |
```

and append below the table:

```markdown
`notch_retune` (lane G, 1.2.0) là **cập nhật**, không phải đóng/mở: một notch
có thể retune nhiều lần giữa `notch_set` và `notch_clear` của nó.
`tools/logstats.py` xử lý nó bằng cách sửa bản ghi đang mở (`depth_db`,
`deepest_db`, `retunes`) và **không** đóng bản ghi — nếu đóng thì một lần đào
sâu ở mốc 300 ms sẽ biến mọi notch được đào thành "notch sống 300 ms", tức một
false positive giả trên mọi dòng thống kê.

Reader là một chuỗi `if/elif` trên `ev` và **bỏ qua tên lạ**, nên một file log
cũ đọc bằng tool mới vẫn chạy đúng, và một `ev` thêm sau này cũng không làm
hỏng reader cũ.
```

- [ ] **Step 5: roadmap + spec status**

`docs/superpowers/specs/2026-09-04-anti-feedback-v2-roadmap.md` line 23 — replace the lane G row's last column with `đã hạ cánh 1.2.0`, and line 70's `| G | chờ S, D | |` with `| G | đã hạ cánh 1.2.0 | thang độ sâu + nhả dần + nhớ phòng; ramp 10 ms trong Biquad |`. In lane A's row, add: `fallback notch = thang G (đặt −6, đào 6 dB/300 ms, nhả 30 s + 10 s/bậc)`.

`docs/superpowers/specs/2026-09-06-gain-aware-notch-design.md` line 5 — change `**Trạng thái:** spec v2 ... chờ owner duyệt trước khi viết plan` to `**Trạng thái:** đã thực thi, 1.2.0 alpha (plan: docs/superpowers/plans/2026-09-07-gain-aware-notch.md).`

- [ ] **Step 6: `docs/release-notes/1.2.0-alpha.md`**

Create it in the shape of `1.1.3-alpha.md`. It must contain, in this order: the version and date; the suite count from the final `ctest` run; the four measured attenuations from Task 2's test; a "cái gì đổi" list covering placement (−6/−12), deepening (6 dB/300 ms), the release ladder (30 s + 10 s/rung, Clear at −6), the freeze (no time cap), room memory (5 min, same bin, one use), the −24 clamp, the 10 ms ramp, `notch_retune` in the log, and `savePreset` writing `deepestDb` + `notchDefaults`; and an explicit **"nghe ở âm lượng thấp trước"** block naming the three rows from spec §3 that testers will hear: placement (up to ~0.6 s more howl before it is fully suppressed), the release window from 30 s to the Clear at 40–60 s depending on the rung reached (up to 18 dB more tone missing than 1.1.3), and the "stuck on a shallow rung" case (6–12 dB shallower for the notch's whole life — the intended tone win, and the one thing the suite cannot check). It must also state the Q13 consequence for the shipped presets: `Music` still reaches its −10 dB ceiling and `Speech` its −18, so neither is quieter than on 1.1.3 once the ladder has climbed.

- [ ] **Step 7: `installer/TESTER-NOTES.md`**

Update the header block (version, build date, suite count, SHA-256 — the SHA comes from the installer the release script actually produces, so fill it in AFTER Step 9). Add a `## Mới trong 1.2.0` section before `## Mới trong 1.0.5`, written for a soundman, not a developer. It must say:

- notches now start shallow and get deeper only while the howl continues, so a sudden howl may be audible ~0.3–0.6 s longer than on 1.1.3 — **test at low volume first**;
- a notch that never needs to go deep will stay at −6 or −12 for its whole life, and that is correct, not a bug — **and this is the one behaviour no automated test in the project can check** (M-3), so it is the rig's job: if every notch on your rig ends up at the DEPTH slider's value, say so, because that is the reinforce loop misreading the spectrum;
- after a howl stops the notch now backs off in steps instead of vanishing at 30 s: the first step is at 30 s and each one after it costs 10 s, so a notch that reached the DEPTH slider's value takes **40–60 s** to disappear (40 s from −12, 50 s from −18, 60 s from −24) and between 30 s and that moment **more tone is missing than on 1.1.3**;
- while RING RISK reads RISING or CRITICAL, notches stop backing off entirely, with no time limit;
- the same howl coming back within 5 minutes is notched deep immediately;
- what to report: a click or a "zip" when a notch changes depth (there must be none — every change is ramped over 10 ms); a howl the app never gets on top of; a notch that goes deeper than the DEPTH slider says.

- [ ] **Step 8: `memory/`**

Create `memory/gain-aware-notch-lane-g-2026-09-07.md` with the front-matter shape the other notes use, recording whatever this lane actually taught — at minimum: that a coefficient ramp needs a state-identity assertion because every black-box measurement passes a silent `reset()` (M-7); that the reinforce loop and `runOnce` step 3 both already hold a non-recursive `modelMutex_`, which is why a `*Locked` sibling was the only option (B-1); that `pushClearLocked` leaves every field but `active` intact, so slot reuse must be re-initialised at `setNotchImpl` (B-2); that `ev` — not `kind` — is the log's dispatch key (B-3); and the M-9 consequence that "deepen" and "still howling" are the same test, so the ladder stops at the first rung that works.

Add, from this plan's own 2026-09-07 revision — these are lessons about writing the plan, and they are the ones that cost the most:

- **`pushClearLocked` retaining `depthDB` makes "depth < 0" a false liveness test.** Six tests across three tasks were written on it and would all have passed against cleared slots. The accessor `activeForTest` exists only because of this.
- **A ladder's timings must be derived from the rung, not from a round number.** `55000.0` looked like "past the 50 s release" and was 5 s short of the 60 s a −24 notch actually needs. Write the arithmetic in the comment or it drifts.
- **A scoring fixture has to be derived from the scorer's own formula before it is written.** `RampSineSource` at the spec's illustrative +3 dB/250 ms can never confirm — `rNorm = (rise−1)/0.5` against a ≥ 112.5 ms reference gives 0.35 against a 0.7 floor. Two lines of algebra beat a build.
- **Q13: quantising a ceiling to a rung silently made the shipped `Music` preset 4 dB shallower than 1.1.3.** The defect was invisible in the spec, in the plan and in every test, because no test used a ceiling that was not a multiple of 6. When a constant can take values off the grid your tests use, test one that is.
- **A plan is not verified until someone opens the files it cites.** Three helpers used in Task 9 (`TempDir`, `pumpOneBlockThroughSlotZero`, `notchControllerForTest`) did not exist anywhere in the repo; the plan itself flagged them as unverified and the flag was not acted on until a second reader looked.

Add one line to `memory/MEMORY.md`'s `## Notes` list, in the same style as its neighbours, linking the new file.

- [ ] **Step 9: Verify and commit**

Run: `cd build && ctest -C Release`
Expected: `100% tests passed` — record the number for the release note and the tester notes.

Run: `build/tools/Release/HandsFreeSnapshot.exe shots 1440 920 --fast`
Then READ `shots/console-live.png` back and check the ACTIVE NOTCHES depth column shows a ladder rung (−6 or −12), not −18. Send it to the owner — GUI-visible behaviour is not reported without a picture (CLAUDE.md).

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add docs/GIOI-THIEU.md docs/KY-THUAT-CHONG-HU.md docs/spec-ring-risk.md docs/release-notes/1.2.0-alpha.md installer/TESTER-NOTES.md memory/MEMORY.md memory/gain-aware-notch-lane-g-2026-09-07.md
```
```bash
git add docs/superpowers/specs/2026-09-04-anti-feedback-v2-roadmap.md docs/superpowers/specs/2026-09-05-data-loop-design.md docs/superpowers/specs/2026-09-06-gain-aware-notch-design.md
```
```bash
git commit -m "docs: lane G -- depth ladder, release staircase, room memory, notch_retune schema"
```

- [ ] **Step 10: Release to the alpha testers**

Per CLAUDE.md's standing instruction, a finished change ships. This one is a minor version.

```bash
pwsh -File installer\release-alpha.ps1 -Part minor
```

Then fill the produced installer's SHA-256 and size into `installer/TESTER-NOTES.md`, and commit the version bump `CMakeLists.txt` change plus the notes:

```bash
git add CMakeLists.txt installer/TESTER-NOTES.md
```
```bash
git commit -m "chore: release 1.2.0 to alpha (lane G)"
```

---

## Self-review

Run against the spec with fresh eyes after the plan was written, then **re-run
2026-09-07 against the real files** by a second, read-only session. Everything
in the "Revision 2026-09-07" table at the top of this document was found by
that pass; this section reflects the plan AFTER those fixes.

### 1. Spec coverage

| Spec § | Requirement | Task |
|---|---|---|
| §2.1 | place at −6, deepen 6 dB, never past the preset ceiling | 5, 6 |
| §2.2 | release ladder −18 → −12 → −6 → Clear, 30 s then 10 s, reclamp immediately | 6 (reclamp), 7 (ladder) |
| §2.3 | room memory: same frequency within 5 min restarts at the old depth | 8 |
| §2.4 | release clock freezes at RISING/CRITICAL | 7 |
| §2.5 | every depth change is a state-preserving 10 ms ramp | 1, 2 |
| §3 | the level table — expected change stated per task | every audio-path task's "Mức level dự kiến" line; §3's rows are quoted into 1, 2, 5, 6, 7, 8 |
| §4.1 (Q13) | effective ladder = rungs shallower than the ceiling + the ceiling itself; no quantisation; live ceiling LOWERING (reason `Ceiling`, Detector only); RAISING buys nothing until reinforce; Preset/Manual own ceiling | 4 (`nextDeeperRungDb` two-arg + `TheEffectiveLadderEndsOnTheCeilingItself`), 5 (`AnOffRungCeilingIsItselfTheDeepestPlacement`), 6 (`TheLastStepLandsExactlyOnAnOffRungCeiling`), 7 (`LoweringTheCeilingPullsADetectorNotchUpOnTheNextTick`, `RaisingTheCeilingDoesNotDeepenUntilTheBinRingsAgain`, `AnOffRungPresetDepthSurvivesTheFirstTick`) |
| §4.2 | 5 new `ModelNotch` fields, `setNotchImpl` initialises all of them (B-2), −24 clamp + adopt log (Q12) | 4 |
| §4.2 | `ScoreBreakdown::riseRatio` | 3; carried onto `NotchEvent::riseRatio` in 4 so a test can prove which band a fixture landed in (B-5) |
| §4.3 | placement steps 1–4, Preset/Manual/Soundcheck rules. The block sits BELOW the index lookup (B-2) and the Soundcheck line is LAST | 5 (steps 1, 2, 4 + the final shape), 8 (step 3 filled in, same shape re-shown) |
| §4.4 | `pushRetuneLocked`, deepen with the 300 ms gate, immediate reclamp, LINKED same rung, M-9 note | 4 (helper), 6 (loop) |
| §4.5 | freeze from `frameMaxScore_`/`frameScoreValid_`, `quietMs`, thresholds, `releaseFrozen` (published EVERY tick, false when the tap is dead — M-6), test seams | 4 (seams), 7 (loop) |
| §4.6 | `ReleasedMemory`: 16/lane, ring, AutoRelease only, same bin, TTL, one use, LINKED both lanes, wipes; an off-rung remembered depth is clamped to the ceiling on read and NOT quantised (m-8) | 8 |
| §4.7 | `Biquad::rampNotchDepth`, the no-boost proof in the doc string, `setNotchFilter`/`reset` cancel, `clearNotch` note, `NotchChain::setNotch` routing, `kRampMs` | 1, 2 |
| §4.8 | `Kind::Retune`, `RetuneReason`, `fromDepthDb`, `ev: "notch_retune"`, `logstats.py`, `SnapshotNotch::deepestDb`, `SnapshotBuffer::releaseFrozen`, `savePreset` (Q11) | 4 (types, snapshot), 9 (writer, reader, preset) |
| §4.9 | the constant table, one place, not on the GUI | 4 |
| §4.10 inv. 1 | never a Set outside [−24, 0] | 4 (clamp + refusal), 7 (fuzz test) |
| §4.10 inv. 2 | never deeper than the ceiling | 5, 6, 7 |
| §4.10 inv. 3 | ≤ 6 dB per deepening step (Q13 can make the LAST step smaller, never larger); shallow steps may be larger | 6 (`nextDeeperRungDb`), 7 (ceiling/reclamp comments) |
| §4.10 inv. 4 | a rejected design leaves the slot untouched | 1, 2, 4 |
| §4.10 inv. 5 | ramp only between designs sharing freq/Q/sr | 2 |
| §4.10 inv. 6 | `processSample` allocates nothing, one branch | 1 |
| §4.10 inv. 7 | freeze only when valid; detection off releases as before | 7 |
| §4.10 inv. 8 | Soundcheck exempt from all of it | 5, 6, 7 |
| §5.1 | eight Biquad tests, state identity and gain measurement included | 1 (nine tests) |
| §5.2 | NotchChain ramp / reset routing, ±0.5 dB per rung, `NotchInfo.depthDB` immediate | 2 |
| §5.3 | controller tests | 4, 5, 6, 7, 8 — see the row-by-row list below |
| §5.3 "trần nâng về −18 ⇒ không đào lại cho tới khi reinforce" | the RAISE half of the live-ceiling rule | 7 (`RaisingTheCeilingDoesNotDeepenUntilTheBinRingsAgain`) — **added by the 2026-09-07 revision; the v1 plan covered only the lowering half** |
| §5.3 "Preset: adoptPreset −12 ⇒ không đào; nhả thang; kẹp lại về −12" | a preset notch obeys the release ladder and reclamps to its OWN depth | 6 (`APresetNotchNeverDeepens`), 7 (`APresetNotchReleasesDownTheLadderAndReclampsToItsOwnDepth`) — **the release/reclamp half added by the revision** |
| §5.3 "INDEP: làn kia không đổi" | the INDEP half of the LINKED/INDEP rung rule | 6 (`IndepLeavesTheOtherLaneUntouchedThroughTheWholeClimb`) — **added by the revision; the v1 plan covered only LINKED** |
| §5.3 "Đào … dừng ở bậc đầu tiên làm bin hết vượt ngưỡng" (Q7) | NOT covered by any test, and cannot be — see item 6 of §2 below | 6 (level note), 10 (tester notes) |
| §5.4 | snapshot image showing a ladder rung | 10 (Step 9) |
| §6 | docs, tester notes, release notes, roadmap, memory | 10 |
| §7 / Q13 | the decisions locked in the spec | honoured throughout; none re-opened. Q13 is the owner's 2026-09-07 addition and is applied in Tasks 4, 5, 6, 7, 8 and 10 |

### 2. Spec requirements I could NOT map to a task

Stated explicitly, as required:

1. **§5.3, "`riseRatio` 1.9 → −6".** The boundary value cannot be produced deterministically from the audio fixture — the rise axis is a ratio between two real FFT frames, and no source in `tests/test_notchcontroller.cpp` can be dialled to land on 1.9 rather than 1.87 or 1.94. What Task 5 pins instead is the two sides of the line by construction: `RampSineSource` at +9.5 dB/250 ms (derived in Task 5, B-5) places at −6 with its logged `riseRatio` asserted to be in **[1.5, 2.0)**, and `SineSource`'s hard start (riseRatio ≫ 2) places at −12. Since the revision the band is asserted rather than assumed, so a fixture that drifts out of it fails loudly instead of passing for the wrong reason — but **the exact threshold value 2.0 is still pinned only by the constant, not by a test.** If a reviewer wants the boundary itself covered, the honest way is a pure test on a hand-built `ScoreBreakdown` — which would require extracting the depth choice from `placeConfirmed` into a static helper. That refactor is NOT in this plan; flag it rather than fake it.

2. **§5.3, "Slot tái dùng (B-2): … frame reinforce kế KHÔNG Reclamp".** Task 4's `ReusingASlotResetsEveryLadderField` pins the proximate cause (all five fields re-initialised, `releasedSteps == 0`), and Task 6's reclamp branch only fires on `releasedSteps > 0`. But no test drives the full sequence "deepen to −24 → Clear → manual −6 on the same index → one reinforce frame → assert no Reclamp". **Partially mapped only.**

3. **§5.4, "ảnh `console-live.png` gửi owner".** This is a human handoff, not something a test can assert. Task 10 Step 9 names the command and requires the image be READ back before sending, per CLAUDE.md — but nothing in the suite fails if it is skipped.

4. **§4.9, `kDepthStepDb = 6`.** Declared in Task 4 because the spec's constant table names it, but **no code in this plan reads it** — the step size is implicit in `kDepthLadderDb`'s spacing and in `nextDeeperRungDb`. It is documentation in constant form, and under Q13 it is now slightly misleading as well: the last step of an effective ladder can be smaller than 6 dB. A reviewer may reasonably ask for it to be deleted; the plan keeps it because the spec's table is the reference the release note quotes, and the header comment says what it does and does not mean.

5. **§5.3 / §4.4, "the ladder stops at the first rung that quiets the bin" (Q7, M-9).** **Not covered, and not coverable in this suite.** The harness writes the raw tone into `h.tap` (`tests/test_notchcontroller.cpp:344-349`); nothing puts the notch chain between the source and the Detector, so the analyser sees the un-notched spectrum every frame, the bin never goes quiet, and the ladder always reaches the ceiling. Every deepening test in Tasks 6 and 8 therefore asserts "climbs to the ceiling", which is the correct assertion *for this fixture* and says nothing about the rung the real system stops on. **This is the headline user-visible claim of the whole lane and it is verified only on a rig.** Task 6's level note and Task 10's tester notes both say so. The alternative — a source that subtracts a modelled notch from its own output — would test the model, not the loop, and would stay green while the real chain misbehaved; it is deliberately not in this plan.

6. **`NotchController::setSampleRate` still has no production caller.** Task 8 wipes room memory there anyway, for the tests and `tools/snapshot.cpp` that do call it, and because it is where the wipe belongs the day the device path reaches it. Carried over from lane R's parked item, not resolved here.

7. **§6, "sổ quyết định cập nhật nếu phản biện lật".** Nothing in this plan overturns Q1–Q13, so `docs/superpowers/decisions/2026-09-06-lane-g-gain-aware-notch.md` is left untouched — **Q13 itself came the other way**: the 2026-09-07 cross-check found the spec v2 quantisation making `presets/Music.json` 4 dB shallower than 1.1.3, the owner ruled, and the decision record and spec §4.1/§7 were updated before this plan was. If executing a task forces a further decision to change, that file must be updated in the same commit — it is the reason the spec says "đừng hỏi lại".

Also carried forward unchanged, as the spec's §7 requires: `PresetManager`'s `notchDefaults.depthDB` default of −12 (`PresetManager.h:146`) still disagrees with `NotchController::kDefaultNotchDepthDb` of −18 (`NotchController.h:95`). Task 9 makes `savePreset` write `notchDefaults`, which stops the disagreement mattering for a file this app SAVED, but a preset written by hand with no `notchDefaults` block still silently gets a −12 ceiling. **Not lane G's to fix (M-3); recorded in Task 10's `docs/KY-THUAT-CHONG-HU.md` §4 edit.** Lane R's parked items (I-3, `setSampleRate` having no production caller, A-R7) also remain open — the owner said not to fold them in.

### 3. API names used

**VERIFIED to exist** (read in the worktree on 2026-09-07, at these lines):

| Name | Where |
|---|---|
| `Biquad::setNotchFilter(double,double,double,double)` | `src/dsp/Biquad.h:102`, `.cpp:72` |
| `Biquad::processSample`, `Biquad::reset` | `src/dsp/Biquad.h:103-104`, `.cpp:130,139` |
| `Biquad` members `b0_ b1_ b2_ a1_ a2_ z1_ z2_` | `src/dsp/Biquad.h:108-112` |
| `NotchChain::setNotch`, `clearNotch`, `getNotchInfo`, `NotchInfo`, `NotchState`, `sampleRate_`, `filters_`, `notchInfo_` | `src/dsp/NotchChain.h:41-80`, `.cpp:25,55,129` |
| `CandidateScorer::ScoreBreakdown`, `scoreCandidateDetailed`, `kConfirmScore`, `kDefaultRiseReferenceMs` | `src/dsp/CandidateScorer.h:82-93,49,39`; rise branch `.cpp:76-110` |
| `NotchController::Origin`, `ClearReason`, `kAutoReleaseMs`, `kDefaultNotchDepthDb`, `kChannels`, `kSlots`, `kTotalSlots` | `src/app/NotchController.h:60,65-68,83,95,70-72` |
| `NotchController::ModelNotch`, `slotOf`, `model_`, `outbox_`, `modelMutex_`, `snapshotMutex_`, `latest_`, `liveMs_`, `width_`, `lanes_`, `notchQ_`, `notchDepthDb_` | `src/app/NotchController.h:320-331,453-455,481-482,474,390,415,447-448` |
| `NotchController::frameMaxScore_`, `frameScoreValid_` | `src/app/NotchController.h:433-434` |
| `NotchController::setNotchImpl`, `pushClearLocked`, `pushEventLocked`, `placeConfirmed`, `processSpectrumForDetection`, `firstFreeIndexLocked`, `firstFreeIndexAllLanesLocked`, `PlacementContext` | `src/app/NotchController.h:334-376`; `.cpp:135,195,183,580,702,557,565` |
| `NotchController::runOnce` step 2 `dt`, step 3 auto-release, reinforce loop | `src/app/NotchController.cpp:377-379, 403-416, 737-763` |
| `NotchController::SnapshotNotch`, `SnapshotBuffer`, `copySnapshot`, `ringRiskValid/Score/Threshold` | `src/app/NotchController.h:268-308` |
| `NotchController::setWidth`, `clearAll`, `setSampleRate`, `adoptPreset`, `setNotchDefaults`, `getNotchQ`, `getNotchDepthDb`, `effectiveLinked`, `liveMsForTest` | `.cpp:80,223,864,231,463,472,477`; `.h:142`; `.cpp:423` |
| `NotchCommand`, `NotchCommandType::Set/Clear` | `src/dsp/NotchCommand.h:5-21` |
| `AudioEngine::drain` Set/Clear dispatch | `src/app/AudioEngine.cpp:430-450` |
| `MainComponent::notchEventToVar`, `savePreset`, `originName`, `reasonName` | `src/app/MainComponent.cpp:559,909,54,66`; `.h:103` |
| `SessionLogger::makeEvent` | `src/app/SessionLogger.h:50`, `.cpp:25` |
| `PresetNotch`, `PresetNotchDefaults` (Q 30 / depth −12), `Preset::notchDefaults` | `src/app/PresetManager.h:103,143-147,160` |
| `gui::SpectrumView::kRingRiskRisingFraction` = 0.55f, `riskForScore` | `src/gui/SpectrumView.h:239,258` |
| test fixture: `FakeClock`, `Harness`, `SlotHarness`, `StereoHarness`, `SineSource`, `NoiseSource`, `pump`, `pumpStereo`, `primeAndPlace`, `anyCandidateInSnapshot`, `warmThenDrive`, `Recorder`, `Ev`, `kBlockMs`, `kTestSr`, `kTestPi`, `kWarmupBlocks` | `tests/test_notchcontroller.cpp:12,20,28,39,310,329,344,351,384,362,945,1278` |
| test fixture: `sineWave`, `rms` | `tests/test_biquad.cpp:25,38` and `tests/test_notchchain.cpp:20,31` |
| test fixture: `Rig`, `makeToneInNoise`, `kHop`, `kFrameMs`, `kSampleRate` | `tests/test_candidatescorer.cpp:72,36,33,34,31` |
| the `-18` assert this plan corrects | `tests/test_notchcontroller.cpp:438` |
| `logstats.py` dispatch (an `if/elif` chain with no `else`, so unknown `ev` names are IGNORED, not miscounted — m-7), `--expect-*` flags, `encoding="utf-8"` | `tools/logstats.py:45-63, 124-127, 19` |
| `logstats_fixture` ctest entry | `tests/CMakeLists.txt:113-117` (inside `if(Python3_Interpreter_FOUND)` at 112) |

**VERIFIED by the 2026-09-07 cross-check, and the reason six of its findings exist:**

| Name / fact | Where |
|---|---|
| `pushClearLocked` lowers only `active`; the Clear event reads `n.depthDB` | `src/app/NotchController.cpp:195-212`, depth read at `:208` (B-3) |
| `adoptPreset`'s `firstLane >= width_` skip, above every `setNotch` | `src/app/NotchController.cpp:242-246` (M-5) |
| `placeConfirmed`: the three `const` reads, then the index search | `src/app/NotchController.cpp:585-587`, `:589-596` (B-2) |
| the snapshot publish block, incl. `ringRisk*` at `:368-370` and `++latest_.sequence` at `:371` — NOT where `releaseFrozen` goes | `src/app/NotchController.cpp:337-372` (M-6) |
| `rNorm = (rise − 1) / 0.5`, clamped; reference frame `>= 0.45 × riseReferenceMs` old | `src/dsp/CandidateScorer.cpp:83, 104-106`; `kDefaultRiseReferenceMs = 250.0` at `CandidateScorer.h:39` (B-5) |
| fixture order: `SineSource` `:310-327`, `NoiseSource` `:329-342`, `pump` `:344-349`, `kWarmupBlocks` `:308` | `tests/test_notchcontroller.cpp` (B-6) |
| the armed detector keeps placing fresh notches on a ringing lane | documented in-place at `tests/test_notchcontroller.cpp:1101-1102` (M-2) |
| `pump` writes the RAW tone into `h.tap`; no notch chain in the path | `tests/test_notchcontroller.cpp:344-349` (M-3) |
| the stereo auto-release tests use `kAutoReleaseMs / kBlockMs + 20`; only `EveryClearPathCarriesItsReason` uses `+ 10` | `tests/test_notchcontroller.cpp:1106, 1133` vs `:1325` (m-6) |
| `AdoptedPresetsAutoReleaseLikeDetectorNotches` adopts −12 and pumps 7000 × 5 ms = 35 s | `tests/test_notchcontroller.cpp:267-291` — the existing test the v1 plan missed |
| `test_gui_wiring.cpp`'s real savePreset shape: `ScopedJuceInitialiser_GUI`, `getNotchControllerForTest(0)`, explicit temp `juce::File`, `getAudioEngine().getTapBuffer(0).write(...)` + `runOnce()`; the preset notch is adopted at −9.0 | `tests/test_gui_wiring.cpp:1016-1041`, notch depth at `:1030` (M-1, B-1) |
| `MainComponent::getNotchControllerForTest` public at `:188`; `savePreset` at `:103`; `notchEventToVar` private static at `:256` | `src/app/MainComponent.h` (m-4) |
| `originName` / `reasonName` are FREE functions in an anonymous namespace | `src/app/MainComponent.cpp:54, 66` (m-5) |
| `NotchDefaultsSurviveTheRoundTrip` starts at line **496** | `tests/test_presetmanager.cpp:496` (m-2 — the cross-check said 497) |

**INTRODUCED by this plan** (nothing above defines them today):

`Biquad::rampNotchDepth`, `Biquad::designPeaking`, `Biquad::State`, `Biquad::Coeffs`, `Biquad::stateForTest`, `Biquad::coeffsForTest`, `Biquad::rampRemainingForTest`, `Biquad::target_/delta_/rampRemaining_`; `NotchChain::kRampMs`; `CandidateScorer::ScoreBreakdown::riseRatio`; `NotchController::kDepthLadderDb`, `kDepthLadderSize`, `kDepthStepDb`, `kMaxDepthDb`, `kDeepenAfterMs`, `kSteepRiseRatio`, `kReleaseFirstMs`, `kReleaseStepMs`, `kMemoryTtlMs`, `kMemoryEntriesPerLane`, `kRiskFreezeFraction`, `RetuneReason`, `NotchEvent::Kind::Retune`, `NotchEvent::retuneReason`, `NotchEvent::fromDepthDb`, `NotchEvent::riseRatio`, `nextDeeperRungDb` (two-arg), `nextShallowerRungDb`, `ceilingDbFor`, `pushRetuneLocked`, `MemoryEntry`, `rememberReleaseLocked`, `takeRememberedDepthLocked`, `clearRoomMemoryLocked`, `roomMemory_`, `roomMemoryHead_`, `ringRiskOverrideForTest_`, `depthDbForTest`, `deepestDbForTest`, `quietMsForTest`, `activeForTest`, `retuneForTest`, `setRingRiskOverrideForTest`, `SnapshotNotch::deepestDb`, `SnapshotBuffer::releaseFrozen`, `ModelNotch::{deepestDb, stageChangedAtMs, quietMs, releasedSteps, ceilingDb}`; the free `retuneReasonName` in `MainComponent.cpp`'s anonymous namespace, `MainComponent::notchEventToVarForTest`; test fixture `RampSineSource`, `primeAndPlaceSlowly`, `pumpQuietFor`, `firstActiveIndex`, `magnitudeAt`, `noBoostProbeFrequencies`; `logstats.py` `--expect-retunes` and the `notch_retune` branch.

**WITHDRAWN by the 2026-09-07 revision** (named in v1, not introduced by this plan any more): `NotchController::ceilingRungDb` — deleted with its test, replaced by the two-arg `nextDeeperRungDb` (Q13); `MainComponent::retuneReasonName` as a member — it is a free function (m-5); the one-arg `nextDeeperRungDb`.

**Names the v1 plan used without verifying, and which do NOT exist:** `TempDir`, `pumpOneBlockThroughSlotZero`, `notchControllerForTest`. The v1 self-review flagged them as unverified and the flag was not acted on; Task 9 now uses the real shape from `tests/test_gui_wiring.cpp:1016-1041` and names no helper that a grep of the repo does not find (M-1). **There are no unverified names left in this plan.**

### 4. Where the cross-check itself was wrong

Recorded so the next reader does not "re-fix" these back:

- **m-1**: the cross-check said `add_test(NAME logstats_fixture …)` is at "112-117 (not 113-117)". `grep -n` in this worktree puts `add_test(NAME logstats_fixture` on line **113**; line 112 is the `if(Python3_Interpreter_FOUND)` that opens the block. The plan keeps **113-117** and notes the enclosing `if`.
- **m-2**: the cross-check said `NotchDefaultsSurviveTheRoundTrip` "starts at 497". `grep -n` puts it at **496**. The plan uses 496.
- **m-3**: the cross-check said the `--expect-*` flags are "at 128-131". They are at **124-127** (`--expect-notches` 124 … `--expect-recurrence-max` 127), which is what the plan already said and still says.

Everything else the cross-check reported was reproduced against the files before being applied.

### 5. Type consistency

Checked across tasks: `deepestDb` (not `deepestDB`) everywhere — `ModelNotch::deepestDb`, `SnapshotNotch::deepestDb`, `deepestDbForTest`, and the log key `deepest_db` in `logstats.py` only. `depthDB` keeps its existing capitalisation everywhere it already exists (`NotchInfo::depthDB`, `ModelNotch::depthDB`, `NotchCommand::depthDB`, `SnapshotNotch::depthDB`, `PresetNotch::depthDB`) and the new accessor is `depthDbForTest` to match the file's existing `getNotchDepthDb` style. `RetuneReason` values are `Deepen/Release/Reclamp/Ceiling` in C++ and `deepen/release/reclamp/ceiling` in JSON, in that one mapping, in `retuneReasonName`. `kRampMs` lives on `NotchChain` only; `kDeepenAfterMs`, `kReleaseFirstMs`, `kReleaseStepMs`, `kMemoryTtlMs` on `NotchController` only. `nextDeeperRungDb (currentDb, ceilingDb)` and `nextShallowerRungDb (currentDb)` are used with those exact spellings and arities in Tasks 4, 6 and 7; `ceilingDbFor (n)` is the only way any task obtains a ceiling, and no task calls a quantiser, because there is none.
