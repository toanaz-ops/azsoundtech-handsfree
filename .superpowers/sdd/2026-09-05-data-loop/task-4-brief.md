### Task 4: `notch_set` with score and spectral context

**Files:**
- Modify: `src/app/NotchController.h:268` (`placeConfirmed` signature), `src/app/NotchController.cpp:455-543` (placeConfirmed), `:620-660` (scoring loop)
- Test: `tests/test_notchcontroller.cpp` (append)

**Interfaces:**
- Consumes: `CandidateScorer::scoreCandidateDetailed` (Task 2); `NotchEvent`, `SpectralContext`, `setNotchImpl` (Task 3).
- Produces: detector-origin `Set` events with `hasScore == true`, `ctx != nullptr`, `ctx->now` = the analysed frame, `ctx->ref` = `ScoreBreakdown::refFrame` (when any), `ctx->other` = the other lane's frame of the same drain iteration (when any). Private struct:
  ```cpp
  struct PlacementContext
  {
      const float* now = nullptr;
      const float* other = nullptr;        // may be null
      CandidateScorer::ScoreBreakdown breakdown;
      float finalScore = 0.0f;             // after the asymmetry multiplier
      float asymmetry = 1.0f;
      int   persistNeeded = 0;
      float thr = 0.0f;
      double sampleRate = 0.0;
  };
  void placeConfirmed (int lane, const PeakinessAnalyzer::Candidate& cand, bool linkedNow,
                       const PlacementContext& pc);
  ```

- [ ] **Step 1: Write the failing test** — append to `tests/test_notchcontroller.cpp`:

```cpp
// Spec test 7. Red if the detector's Set event stops carrying the frame it
// scored (ctx.now), the frame the rise axis compared against (ctx.ref, with
// its age), or the other lane's frame; or if the recorded axes stop
// multiplying to the recorded score.
TEST (NotchControllerEvents, DetectorPlacementCarriesTheScoredFrameAndTheScorersReference)
{
    StereoHarness h; Recorder r; h.controller.setEventSink (r.sink());
    h.controller.setDetectionActive (true);
    NoiseSource quietL; SineSource toneR;
    const auto cmds = warmThenDrive (h, quietL, toneR);
    ASSERT_FALSE (cmds.empty());

    const Ev* set = nullptr;
    for (const auto& e : r.events)
        if (e.kind == Ev::Kind::Set && e.hasScore) { set = &e; break; }
    ASSERT_NE (set, nullptr) << "no scored Set event reached the sink";

    EXPECT_EQ (set->lane, 1);
    EXPECT_EQ (set->confirmedLane, 1);
    EXPECT_EQ (set->origin, NotchController::Origin::Detector);
    EXPECT_GT (set->score, CandidateScorer::kConfirmScore);
    EXPECT_FLOAT_EQ (set->pNorm * set->rise * set->novelty * set->penalty * set->asymmetry, set->score);
    EXPECT_EQ (set->persistNeeded, NotchController::kPersistenceBlocks);
    EXPECT_FLOAT_EQ (set->thr, PeakinessAnalyzer::kDefaultThreshold);

    ASSERT_NE (set->ctx, nullptr);
    const auto& ctx = *set->ctx;
    EXPECT_EQ (ctx.bins, Detector::kNumBins);
    EXPECT_NEAR (ctx.binHz, kTestSr / Detector::kFftSize, 1e-9);

    // ctx.now IS the scored frame: its peak bin is the howl.
    const int bin = (int) std::lround (set->hz / ctx.binHz);
    int argmax = 0;
    for (int b = 1; b < Detector::kNumBins; ++b)
        if (ctx.now[(std::size_t) b] > ctx.now[(std::size_t) argmax]) argmax = b;
    EXPECT_EQ (argmax, bin);

    // ctx.ref is the frame the rise axis used (D-7): at least 0.45 x rise old,
    // and the rise it implies is the rise the score encodes (rNorm >= 0.7
    // means now/was >= 1.35).
    ASSERT_TRUE (ctx.hasRef);
    EXPECT_GE (ctx.refAgeMs, 0.45 * CandidateScorer::kDefaultRiseReferenceMs - 1e-6);
    EXPECT_LE (ctx.refAgeMs, CandidateScorer::kRiseHistoryWindowMs);
    EXPECT_GE (ctx.now[(std::size_t) bin], 1.35f * ctx.ref[(std::size_t) bin]);

    // The other lane was quiet: its frame is there and small at the howl bin.
    ASSERT_TRUE (ctx.hasOther);
    EXPECT_LT (ctx.other[(std::size_t) bin], 0.1f * ctx.now[(std::size_t) bin]);
}
```

- [ ] **Step 2: Run to verify failure** — build; expected: test compiles (the fields exist since Task 3) and fails at `ASSERT_NE (set, nullptr)`.

- [ ] **Step 3: Implement** — in the candidate loop of `processSpectrumForDetection` replace

```cpp
        float score = la.scorer.scoreCandidate (cand, block.magnitudes, { locked.data(), locked.size() });
        score *= asymmetryMultiplier (...);
```
with
```cpp
        const auto breakdown = la.scorer.scoreCandidateDetailed (cand, block.magnitudes,
                                                                 { locked.data(), locked.size() });
        const float asym = asymmetryMultiplier (block.magnitudes, otherLaneMagnitudes, cand.bin,
                                                laneAsymmetryBonus_.load (std::memory_order_relaxed));
        const float score = breakdown.score * asym;
```
and at the confirm site build the context:

```cpp
                PlacementContext pc;
                pc.now = block.magnitudes; pc.other = otherLaneMagnitudes;
                pc.breakdown = breakdown; pc.finalScore = score; pc.asymmetry = asym;
                pc.persistNeeded = (int) requiredBlocks;
                pc.thr = la.analyzer.getThreshold();
                pc.sampleRate = block.sampleRate;
                placeConfirmed (lane, cand, linkedNow, pc);
```

In `placeConfirmed`, before the `setNotch` loop, build the shared context and the scored template once (this is the one allocation, on the detector thread, per placed notch — A-4):

```cpp
    auto ctx = std::make_shared<SpectralContext>();
    ctx->binHz = pc.sampleRate / (double) Detector::kFftSize;
    std::copy_n (pc.now, Detector::kNumBins, ctx->now.begin());
    if (pc.breakdown.refFrame != nullptr)
    {
        ctx->hasRef = true;
        ctx->refAgeMs = pc.breakdown.refAgeMs;
        std::copy_n (pc.breakdown.refFrame, Detector::kNumBins, ctx->ref.begin());
    }
    if (pc.other != nullptr)
    {
        ctx->hasOther = true;
        std::copy_n (pc.other, Detector::kNumBins, ctx->other.begin());
    }

    NotchEvent scored;
    scored.hasScore = true;
    scored.confirmedLane = lane;
    scored.score = pc.finalScore; scored.peakiness = pc.breakdown.rawPeakiness;
    scored.pNorm = pc.breakdown.pNorm; scored.rise = pc.breakdown.rNorm;
    scored.novelty = pc.breakdown.mNorm; scored.penalty = pc.breakdown.penalty;
    scored.asymmetry = pc.asymmetry; scored.persistNeeded = pc.persistNeeded; scored.thr = pc.thr;
    scored.ctx = ctx;

    int applied = 0;
    for (int l = firstLane; l <= lastLane; ++l)
        if (setNotchImpl (l, index, cand.frequencyHz, q, depthDb, origin, &scored))
            ++applied;
```

`pc.breakdown.refFrame` points into `history_`; it is still valid here because `commitBlock` runs after the candidate loop. Add `#include <algorithm>` if `std::copy_n` is not yet visible.

- [ ] **Step 4: Build and run** — `cmake -B build ... && cmake --build build --config Release && cd build && ctest -C Release --output-on-failure -R NotchController`. Expected: all pass.

- [ ] **Step 5: Full suite** — `100% tests passed` (417).

- [ ] **Step 6: Commit**

```bash
git add src/app/NotchController.h src/app/NotchController.cpp tests/test_notchcontroller.cpp
git commit -m "feat(controller): detector placements log score axes and the scored/reference frames"
```

---

