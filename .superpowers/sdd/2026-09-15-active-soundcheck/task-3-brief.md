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

      // 1/3-octave moving average of hDb. Public so a later lane can reuse it;
      // NOT separately tested -- SpeakerRolloffIsNotACandidate exercises it
      // through pick(), and a no-op smoother turns that test red (m-19).
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
//
// B-7: mind the sign. The list is sorted by H_dB DESCENDING (hottest first), and
// marginDb == -H_dB, so along the sorted list marginDb ASCENDS -- it gets more
// negative. Plan rev 1 asserted it the other way round and would have failed on
// a correct implementation.
TEST (SoundcheckCandidates, AtMostSixPerLaneAndTheHottestSurvive)
{
    Field f;
    const double hz[]  = { 200.0, 400.0, 630.0, 1000.0, 1600.0, 2500.0, 4000.0, 5000.0 };
    const float  hot[] = {  1.0f, 20.0f,  3.0f,  18.0f,   5.0f,  16.0f,   7.0f,  14.0f };
    for (int i = 0; i < 8; ++i)
        f.poke (hz[i], hot[i]);

    const auto out = SoundcheckCandidates::pick (f.input());

    EXPECT_EQ (out.candidateCount, SoundcheckCandidates::kMaxPreventivePerLane);
    EXPECT_LE (out.candidates[0].marginDb, out.candidates[1].marginDb)
        << "hottest first means MOST NEGATIVE margin first";

    // The two coldest (H_dB 1.0 and 3.0, i.e. margin -1 and -3) must be dropped,
    // so every survivor has margin <= -5.
    for (int i = 0; i < out.candidateCount; ++i)
        ASSERT_LE (out.candidates[i].marginDb, -5.0f);
}

// RED IF: an untrusted bin can still become a candidate.
//
// m-17: the bin must sit INSIDE [kSweepLowHz, kTrustedHighHz], or step 1 of pick
// drops it on the band test alone and the trusted[] flag is never consulted --
// plan rev 1 poked 8 kHz, which proved nothing. 2 kHz is inside the band, so the
// only thing that can reject it is trusted == false.
TEST (SoundcheckCandidates, UntrustedBinIsNeverACandidate)
{
    Field f;
    f.poke (2000.0, 25.0f);

    // Control: trusted, it IS a candidate.
    ASSERT_EQ (SoundcheckCandidates::pick (f.input()).candidateCount, 1);

    f.trust[(std::size_t) LoopGainEstimator::hzToBin (2000.0, kSr)] = 0;
    EXPECT_EQ (SoundcheckCandidates::pick (f.input()).candidateCount, 0);
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

`smooth` is a moving average over `[k / 2^(1/6), k * 2^(1/6)]` in Hz (one third of an octave, centred), clamped to `[0, kNumBins)`. **It has no test of its own** (m-19): its header comment must say "exercised through `pick`, by `SpeakerRolloffIsNotACandidate`" rather than claiming it is exposed for tests. A smoother that returned its input unchanged would make every prominence zero and turn that test red, which is the coverage that matters. `pick` runs the seven steps in the order listed above, filling `marked`, `markedCount`, `candidateCount`, `saturatedBins` and the sorted `candidates` array; `q` on each candidate is `in.notchQ` verbatim, and `hz` is `LoopGainEstimator::binToHz (bin, in.sampleRate)`.

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

