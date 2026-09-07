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

