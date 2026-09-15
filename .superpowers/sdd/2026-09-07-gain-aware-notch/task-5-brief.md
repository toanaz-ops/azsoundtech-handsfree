### Task 5: `placeConfirmed` places SHALLOW — −6 dB, or −12 on a steep rise

**Mức level dự kiến (spec §3):** this is the first task the testers hear. A detector notch is now placed at **−6 dB**, or **−12 dB** when the candidate's raw rise ratio is ≥ 2.0 (+6 dB over the rise window) — instead of today's fixed **−18 dB**. At the notch's own bin that is **12 dB shallower** (or 6 dB) at the instant of placement; Task 6 walks it back down at 6 dB per 300 ms while the bin keeps ringing, so a fast-building howl reaches −18 about **300–600 ms later than 1.1.3 does**. Outside the bin: **0 dB**. A room that howls explosively will therefore be audibly howling for up to ~0.6 s longer than on 1.1.3 — **this is the row testers must try at low volume first.** Soundcheck notches are exempt (KD-7): they still take the full slider depth on the first block.

**Files:**
- Modify: `src/app/NotchController.cpp:580-632` (`placeConfirmed`: the depth choice moves BELOW the index lookup — see Step 4) and `:693-699` (the `[detect]` log line). **m-C: the replaced region is `:583-596`, not `:585-596`** — it starts at the two comment lines `// soundcheckActive() takes modelMutex_ itself, ...` / `// lock below -- never underneath it.`, which the replacement block below re-emits. Replacing only `585-596` leaves those comments standing and the file ends up with them twice.
- Modify: `tests/test_notchcontroller.cpp:349` (fixture: `RampSineSource` + `primeAndPlaceSlowly` go AFTER `pump` closes — B-6), `:436-438` (the stale `-18` assert), `:875-889` (`SetNotchDefaultsFlowIntoPlacedNotch`)
- Test: `tests/test_notchcontroller.cpp`

**Interfaces:**
- Consumes (Task 3): `pc.breakdown.riseRatio`. Consumes (Task 4): `kDepthLadderDb`, `kSteepRiseRatio`, `NotchEvent::riseRatio`, `activeForTest`, and `setNotchImpl`'s initialisation of `deepestDb` / `ceilingDb`.
- Produces: the placement depths Task 6 deepens from and Task 7 releases from. New test fixture symbols:
  ```cpp
  constexpr float kRampStartAmp = 3.0e-4f;   // the sine amplitude whose bin
                                             // magnitude EQUALS NoiseSource's
  struct RampSineSource { double freq; float amp; double gainDbPerMs; double nextSample;
                          std::vector<float> hop(); };   // amp clamped to 0.9
  int primeAndPlaceSlowly (Harness&);        // 64 noise blocks + <= 150 ramp blocks
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

**Deriving the ramp (B-5, rewritten at rev 3).** The rev-2 fixture — 64 noise blocks,
then tone blocks starting at `amp = 0.02` — cannot place at −6 either, and for a
different reason than the spec's +3 dB/250 ms example. **The decisive parameter is
the START amplitude, not the slope.** Work it in this order:

- **The reference frame.** `rNorm = clamp((rise − 1) / 0.5, 0, 1)`
  (`CandidateScorer.cpp:104-106`), `rise = mag_now / mag_ref`, and the reference is
  the newest frame at least `0.45 × riseReferenceMs` old (`:83`). With
  `kDefaultRiseReferenceMs = 250.0` (`CandidateScorer.h:39`) the floor is **112.5 ms**;
  frames arrive one per 512-sample hop (10.667 ms), so the reference is the **11th
  frame back, 117.3 ms old**.
- **Why the rev-2 fixture fails.** Take the fixture's own numbers. `NoiseSource` is
  uniform on ±0.01, so σ = 0.01/√3 = 5.77e-3, and a Hann-windowed 2048-point FFT
  (`Detector.cpp:15-16, 90`; JUCE's transform is unnormalised) gives a per-bin
  magnitude of about `σ·√(Σw²) = 5.77e-3 × √768 = 0.16`. A sine of amplitude `A` in
  the same window lands at about `A·Σw/2 = A × 512`, so `amp = 0.02` is `≈ 10` —
  **64× the noise floor**. For the first 11 tone blocks the reference frame is still
  NOISE, so `riseRatio ≈ 64 ≫ 2.0` no matter what the slope is.
- **And the confirm happens inside those 11 blocks.** `peakinessAt` is
  `mag[bin] / mean(±3..±5 bins)` (`PeakinessAnalyzer.cpp:59-76`) — **scale-invariant**.
  A pure tone is maximally peaky as soon as the 2048-sample window holds only tone,
  which at a 512-sample hop is **4 blocks**; with `kPersistenceBlocks = 3` the confirm
  lands at block ~4–6. That is inside the tone-vs-noise window ⇒ the steep branch ⇒
  **−12, for every slope in [7.8, 12.2] dB/250 ms.** No slope fixes this.
- **The fix: start the ramp AT the noise floor**, so `riseRatio` is never a
  tone-vs-noise ratio. Equating the two magnitudes above:
  `A₀ × 512 = 0.16` ⇒ **`A₀ ≈ 3.1e-4`** ⇒ `kRampStartAmp = 3.0e-4`.
- **Slope +9.5 dB per 250 ms** (`0.038 dB/ms`, 38 dB/s). Over the 117.3 ms reference
  gap that is `+4.45 dB` ⇒ **`riseRatio ≈ 1.67`**, inside `[1.5, 2.0)`: above the 1.5
  that saturates `rNorm` (score is a product, and `score ≥ 0.7` needs `rNorm ≥ 0.7`,
  i.e. `rise ≥ 1.35`), below `kSteepRiseRatio = 2.0`. And while the reference is still
  a noise frame — the first 11 tone blocks — the ratio is `tone(now)/noise ≈
  10^(0.4053·k/20) ≤ 1.67` as well, **because the tone starts at the noise level**.
  So `riseRatio` never exceeds ~1.7 at ANY block, which is the whole point of the fix.
- **Nothing is scored before the reference is tone-vs-tone anyway.** The scorer
  refuses any candidate whose peakiness does not clear the analyser threshold
  (`CandidateScorer.cpp:51`), so the first frame that gets a score at all needs
  `peakiness > 10`, i.e. the tone ≈ 20 dB over the noise mean ⇒ `20/38 = 0.53 s`
  ⇒ block ~49. By then the 11th-frame-back reference is deep inside the tone.
- **When the confirm lands.** `pNorm = (peakiness/10 − 1)/9` (`CandidateScorer.cpp:57`),
  so `pNorm > 0.7` needs `peakiness > 73`, i.e. the tone ≈ 37 dB over the noise mean
  ⇒ `37/38 ≈ 1.0 s` ⇒ **block ~93**, plus persistence ⇒ ~96. `mNorm` is
  `log(mag/baseline)/log 4` against a 3 s EMA (`:117-118`), saturated at +12.04 dB,
  which the ramp passes after 0.32 s — so by confirm time `mNorm = 1`, `rNorm = 1`,
  `penalty = 1` and `score = pNorm`. Confirm at ~block 96 with `riseRatio ≈ 1.67`
  ⇒ **−6**.
- **Headroom.** At block 150 the ramp has gained `150 × 0.4053 = 60.8 dB`
  ⇒ `A = 3e-4 × 10^(60.8/20) ≈ 0.33` — safe. At 2 s (block 188) it is `≈ 1.9`, which
  **clips**. So `primeAndPlaceSlowly` stops at **150 tone blocks** and
  `RampSineSource` clamps `amp ≤ 0.9` as a second line of defence.

**The tunable is now the START amplitude, and it must still be settled by RUNNING the
test (TDD red → green).** Too high ⇒ an onset step ⇒ tone-vs-noise rise ⇒ −12. Too low
⇒ more blocks before `peakiness > 73`, and the 150-block cap is reached first. The
slope only moves WHEN the confirm happens, not which band it lands in. Do not move
`kSteepRiseRatio` or `kConfirmScore` to make it pass.

```cpp
// A tone that CREEPS up out of the noise instead of switching on. SineSource
// starts at full amplitude in one block, so its rise ratio is enormous and
// every test built on it places at -12; the -6 start of spec 4.3 is only
// observable behind a slow build (M-12).
//
// WHY IT STARTS INAUDIBLE (B-5, and this is the load-bearing part): the
// scorer's rise reference is the 11th frame back, 117.3 ms old. If the tone
// switched on ABOVE the noise floor, that reference would be a NOISE frame for
// the first 11 blocks and riseRatio would be the tone-to-noise ratio -- 64x at
// amp 0.02 -- regardless of the slope. And peakiness is scale-invariant
// (PeakinessAnalyzer.cpp:59-76), so a pure tone confirms as soon as the 2048
// window is all tone (~4 blocks) -- i.e. INSIDE that window. Starting at the
// noise floor is the only way to build a rise history that is tone-vs-tone.
//
// kRampStartAmp: NoiseSource (uniform +-0.01, sigma 5.77e-3) has a per-bin
// magnitude of sigma * sqrt(sum w^2) = 0.16 in a Hann 2048 window; a sine of
// amplitude A lands at A * sum(w)/2 = A * 512. Equal at A = 3.1e-4.
//
// +9.5 dB / 250 ms: over the 117.3 ms gap that is +4.45 dB => riseRatio ~1.67,
// above the 1.5 that saturates rNorm and below kSteepRiseRatio 2.0. Confirm
// needs peakiness > 73 (pNorm > 0.7) = ~37 dB over the floor = ~1.0 s.
constexpr float kRampStartAmp = 3.0e-4f;

struct RampSineSource
{
    double freq        = 1000.0;
    float  amp         = kRampStartAmp;   // starts AT the noise floor, not over it
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
        // 0.9 clamp: at 38 dB/s this source passes full scale at ~2 s, and a
        // clipped tone is a different signal with a different spectrum. The
        // caller stops long before here; this is the second line of defence.
        amp = static_cast<float> (std::min (0.9, static_cast<double> (amp)
                                  * std::pow (10.0, gainDbPerMs * kBlockMs / 20.0)));
        return out;
    }
};

// primeAndPlace's shape, driven by a source that ramps. Returns the index of
// the first Set, or -1. Stops at 150 tone blocks = 1.6 s = +60.8 dB, where the
// amplitude is ~0.33 -- comfortably under the clamp, and ~54 blocks past the
// ~96 where the derivation puts the confirm. Pumping further would only reach
// the clamp and then confirm on novelty alone from a FLAT tone, which would
// quietly be testing something else. A -1 here means kRampStartAmp needs
// retuning, not more blocks.
int primeAndPlaceSlowly (Harness& h)
{
    NoiseSource quiet;
    for (int i = 0; i < kWarmupBlocks; ++i)
        pump (h, quiet.hop());

    RampSineSource tone;
    for (int i = 0; i < 150; ++i)
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
    ASSERT_GE (slot, 0) << "the slow ramp never confirmed -- LOWER kRampStartAmp "
                           "so the tone spends longer under the peakiness "
                           "threshold, or raise gainDbPerMs (B-5)";
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
    //
    // riseRatio is only MEANINGFUL once the scorer has history at least
    // 112.5 ms deep: with no history at all the rise branch returns rNorm 1
    // and riseRatio 1.0 by definition (CandidateScorer.cpp:76-80), which would
    // read as "too slow" here. kWarmupBlocks is 64 blocks of noise, so by the
    // time anything can be confirmed the history is ~683 ms deep and this
    // assertion is comparing two real frames.
    const Ev* set = nullptr;
    for (const auto& e : r.events)
        if (e.kind == Ev::Kind::Set) { set = &e; break; }
    ASSERT_NE (set, nullptr);
    EXPECT_GE (set->riseRatio, 1.5f)
        << "the ramp was too slow to confirm on rise (or the fixture confirmed "
           "before the scorer had 112.5 ms of history)";
    EXPECT_LT (set->riseRatio, NotchController::kSteepRiseRatio)
        << "the tone stepped ONTO the noise floor instead of starting at it: "
           "riseRatio is a tone-vs-noise ratio, so raise kRampStartAmp's "
           "derivation, not the slope (B-5)";
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

Replace `:583-596` — the two-line `soundcheckActive()` comment, the three `const`
reads AND the index-search block — with the code below. **m-C: the range starts at
the COMMENT, not at `const Origin origin`.** The replacement re-emits those two
comment lines, so a literal replace of `585-596` would leave a duplicated comment
above the new one. Anchor on the text `// soundcheckActive() takes modelMutex_
itself` and replace from there through the closing `}` of the index-search scope.

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

