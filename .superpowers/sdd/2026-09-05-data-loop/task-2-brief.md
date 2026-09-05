### Task 2: `CandidateScorer::ScoreBreakdown`

**Files:**
- Modify: `src/dsp/CandidateScorer.h:71-75`, `src/dsp/CandidateScorer.cpp:34-130`
- Test: `tests/test_candidatescorer.cpp` (append)

**Interfaces:**
- Produces:
  ```cpp
  struct ScoreBreakdown
  {
      float rawPeakiness = 0.0f;            // Candidate::peakiness, unclamped ratio
      float pNorm = 0.0f, rNorm = 0.0f, mNorm = 0.0f;   // the three 0..1 axes
      float penalty = 1.0f;                 // 1.0 or kHarmonicPenalty
      float score = 0.0f;                   // pNorm * rNorm * mNorm * penalty, bit-exact with scoreCandidate()
      const float* refFrame = nullptr;      // frame the rise axis compared against; nullptr when none
      double refAgeMs = 0.0;                // clockMs_ - that frame's timeMs; 0 when refFrame == nullptr
  };
  ScoreBreakdown scoreCandidateDetailed (const PeakinessAnalyzer::Candidate& candidate,
                                         const float* magnitudes,
                                         const LockedFrequencyView& lockedFrequencies);
  ```
  `refFrame` points into `history_` and is valid only until the next `commitBlock()`.

- [ ] **Step 1: Write the failing tests** — append to `tests/test_candidatescorer.cpp`:

```cpp
//==============================================================================
// Lane D (data loop): the score breakdown the session log records.

// Spec test 5. Red if scoreCandidateDetailed() diverges from scoreCandidate()
// by even one ULP, or if the recorded axes stop multiplying to the score.
TEST (CandidateScorerBreakdown, DetailedScoreIsBitExactWithTheFloatOverloadAndItsAxesMultiplyToIt)
{
    Rig rig;
    const auto signal = makeToneInNoise (1000.0, 0.5f, 0.01f, 4242u, (std::size_t) kHop * 80);

    // Drive the real pipeline so the breakdown is taken against genuine
    // history and baseline state, not a fresh scorer.
    std::size_t offset = 0;
    for (int i = 0; i < 60; ++i, offset += (std::size_t) kHop)
        rig.cycle (signal, offset);

    EXPECT_EQ (rig.tap.write (signal.data() + offset, (std::size_t) kHop), (std::size_t) kHop);
    const auto spectrum = rig.detector.processLatestBlock (rig.tap);
    ASSERT_NE (spectrum.magnitudes, nullptr);
    rig.scorer.beginBlock (kSampleRate);
    const auto result = rig.analyzer.analyse (spectrum);
    ASSERT_GT (result.count, 0u);

    const std::vector<double> locked { 300.0 };   // 1 kHz sits at 3.3x -> penalty applies
    const CandidateScorer::LockedFrequencyView view { locked.data(), locked.size() };

    for (std::size_t i = 0; i < result.count; ++i)
    {
        const auto& cand = result.candidates[i];
        const float plain = rig.scorer.scoreCandidate (cand, spectrum.magnitudes, view);
        const auto  b     = rig.scorer.scoreCandidateDetailed (cand, spectrum.magnitudes, view);
        EXPECT_EQ (plain, b.score) << "bin " << cand.bin;
        EXPECT_EQ (b.pNorm * b.rNorm * b.mNorm * b.penalty, b.score) << "bin " << cand.bin;
        EXPECT_FLOAT_EQ (b.rawPeakiness, cand.peakiness);
        if (cand.peakiness > PeakinessAnalyzer::kDefaultThreshold)
            EXPECT_FLOAT_EQ (b.penalty, CandidateScorer::kHarmonicPenalty);
    }
}

// Spec test 6. Red if refFrame/refAgeMs stop describing the frame the rise
// axis actually used: with rise 250 ms and frames at 300/120/100 ms of age,
// the scorer compares against the NEWEST frame at least 0.45 x 250 = 112.5 ms
// old, which is the 120 ms one.
TEST (CandidateScorerBreakdown, RefFrameIsTheNewestFrameAtLeastFortyFivePercentOfRiseReferenceOld)
{
    CandidateScorer scorer;
    scorer.setRiseReferenceMs (250.0);
    scorer.beginBlock (kSampleRate);

    std::array<float, CandidateScorer::kBins> f300 {}, f120 {}, f100 {}, now {};
    f300.fill (1.0f); f120.fill (2.0f); f100.fill (3.0f); now.fill (6.0f);

    // commitBlock stamps clockMs_ AFTER adding elapsedMs: frames land at
    // t = 0, 180, 200; the scoring instant is t = 300.
    scorer.commitBlock (f300.data(), 0.0);
    scorer.commitBlock (f120.data(), 180.0);
    scorer.commitBlock (f100.data(), 20.0);
    // Advance the clock to 300 with a block that must NOT become the reference
    // (age 0): push it, then read the breakdown before anything else moves.
    scorer.commitBlock (now.data(), 100.0);

    PeakinessAnalyzer::Candidate cand;
    cand.bin = 43; cand.frequencyHz = 1007.8125; cand.magnitude = 6.0f; cand.peakiness = 20.0f;

    const auto b = scorer.scoreCandidateDetailed (cand, now.data(), {});
    ASSERT_NE (b.refFrame, nullptr);
    EXPECT_DOUBLE_EQ (b.refAgeMs, 120.0);
    EXPECT_FLOAT_EQ (b.refFrame[43], 2.0f);
    // And the rise axis agrees with that frame: 6 / 2 = 3x -> saturates at 1.
    EXPECT_FLOAT_EQ (b.rNorm, 1.0f);
}
```

`Rig::cycle` already exists at `tests/test_candidatescorer.cpp:87`; read it before using it — it feeds one hop and runs analyse/score/commit, and its `lockedHz` parameter is what the first test bypasses by scoring manually.

- [ ] **Step 2: Run to verify failure** — `cmake --build build --config Release`. Expected: compile error, `scoreCandidateDetailed` is not a member.

- [ ] **Step 3: Implement** — in `CandidateScorer.h` add the struct and declaration under `scoreCandidate` (keep the existing comment, add):

```cpp
    // Lane D (data loop): the same computation, with every axis and the
    // reference frame the rise axis compared against exposed, so a session
    // log can record WHY a notch was placed. `refFrame` points into history_
    // and dies at the next commitBlock(). scoreCandidate() returns .score of
    // this and nothing else -- one arithmetic path, never two.
    struct ScoreBreakdown
    {
        float rawPeakiness = 0.0f;
        float pNorm = 0.0f, rNorm = 0.0f, mNorm = 0.0f;
        float penalty = 1.0f;
        float score = 0.0f;
        const float* refFrame = nullptr;
        double refAgeMs = 0.0;
    };
    ScoreBreakdown scoreCandidateDetailed (const PeakinessAnalyzer::Candidate& candidate,
                                           const float* magnitudes,
                                           const LockedFrequencyView& lockedFrequencies);
```

In `CandidateScorer.cpp` rename the existing body to `scoreCandidateDetailed`, returning a `ScoreBreakdown`. Concretely:

```cpp
float CandidateScorer::scoreCandidate (const PeakinessAnalyzer::Candidate& candidate,
                                       const float* magnitudes,
                                       const LockedFrequencyView& lockedFrequencies)
{
    return scoreCandidateDetailed (candidate, magnitudes, lockedFrequencies).score;
}

CandidateScorer::ScoreBreakdown CandidateScorer::scoreCandidateDetailed (
    const PeakinessAnalyzer::Candidate& candidate,
    const float* magnitudes,
    const LockedFrequencyView& lockedFrequencies)
{
    ScoreBreakdown out;
    out.rawPeakiness = candidate.peakiness;

    if (candidate.peakiness <= PeakinessAnalyzer::kDefaultThreshold)
        return out;   // score 0, axes 0, penalty 1 -- product is 0 as before

    // ... existing axis-1 code, assigning out.pNorm instead of a local ...
    // ... existing axis-2 code; inside the history loop, when `reference` is
    //     chosen, ALSO record:
    //         out.refFrame = reference;
    //         out.refAgeMs = clockMs_ - history_[idx].timeMs;
    //     and assign out.rNorm instead of a local ...
    // ... existing axis-3 code; out.mNorm = static_cast<float> (mNorm) after the clamp ...
    // ... existing penalty code, assigning out.penalty ...

    out.score = out.pNorm * out.rNorm * out.mNorm * out.penalty;
    return out;
}
```

Bit-exactness: the old return was `pNorm * rNorm * static_cast<float> (mNorm) * penalty` with `mNorm` a clamped `double`; `out.mNorm` holds exactly `static_cast<float> (mNorm)`, so the product has the same operands in the same order. Do not reorder the multiplication.

- [ ] **Step 4: Run** — `cmake --build build --config Release && cd build && ctest -C Release --output-on-failure -R CandidateScorer`. Expected: all pass, including the two new ones.

- [ ] **Step 5: Full suite** — `ctest -C Release`: `100% tests passed` (408).

- [ ] **Step 6: Commit**

```bash
git add src/dsp/CandidateScorer.h src/dsp/CandidateScorer.cpp tests/test_candidatescorer.cpp
git commit -m "feat(scorer): scoreCandidateDetailed exposes the axes and the reference frame"
```

---

