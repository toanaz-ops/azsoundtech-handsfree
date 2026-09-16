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
// capture relative to the reference.
//
// Spec rev 1 chose 5 ms and 200 ms, both of which fit inside the 0.7 s tail, so
// the test could never go red (F18). The plan's rev 1 chose 900 ms but rendered
// it with a 1.6 s tail -- a 4.6 s buffer for a delayed sweep that ends at 3.9 s,
// so again NOTHING was truncated (I-11): the same defect, one layer down.
//
// Both cases now render with the REAL kTailSeconds = 0.7. The 900 ms delay then
// genuinely pushes the last part of the sweep past the capture window, so the
// answer is allowed to read low -- what it must NOT do is swing with delay the
// way a time-aligned method would.
TEST (LoopGainEstimator, DelayDoesNotChangeTheAnswer)
{
    const auto x = renderSweep();

    const auto quick = runRoom (x, flatRoom (x,   5.0, 0.5, 0.7), 1.0e-4f);
    const auto slow  = runRoom (x, flatRoom (x, 900.0, 0.5, 0.7), 1.0e-4f);

    ASSERT_TRUE (quick.measured);
    ASSERT_TRUE (slow.measured);

    EXPECT_NEAR (meanMidBandDb (quick), -6.02, 1.0);
    EXPECT_NEAR (meanMidBandDb (slow),  -6.02, 3.0)
        << "a 900 ms delay may cost a little energy off the end of the window, "
           "but the per-bin energy ratio must not TRACK the delay";
}

// RED IF: truncation is hidden instead of measured. A resonance that outlives the
// capture window MUST read low, and by roughly the fraction of energy that was
// cut off. An implementation that silently "corrected" for this would pass
// FlatRoom and fail here.
//
// THE ARITHMETIC, because a number without its derivation drifts (lane G B-4).
// A T60 of t60 decays at 60 dB per t60 seconds, so the energy remaining after
// `d` seconds is 10^(-6 * d / t60) of the total. The sweep crosses 1 kHz at
//
//     t_1k = kSweepSeconds * ln(1000/100) / ln(10000/100) = 3.0 * 0.5 = 1.5 s
//
// so at 1 kHz the resonance is excited 1.5 s in and the short window keeps
// (3.0 - 1.5) + 0.7 = 2.2 s of its ring-out while the long window keeps 13.5 s,
// i.e. effectively all of it. The captured fraction is therefore
// 1 - 10^(-6 * 2.2 / t60), and the shortfall is -10*log10 of it:
//
//     t60 = 2.0 s  ->  fraction 0.99921  ->  shortfall 0.0035 dB   (unmeasurable)
//     t60 = 8.0 s  ->  fraction 0.5477   ->  shortfall 2.62 dB     (measurable)
//
// Plan rev 1 shipped t60 = 2.0 with an expectation of 3.0 dB. That is off by a
// factor of ~750 and the test would have failed on the first run (I-2). t60 is
// 8.0 s here, and 2.6 dB is the number the algebra above produces.
TEST (LoopGainEstimator, DecayLongerThanTheTailIsUnderRead)
{
    const auto x = renderSweep();

    const auto full   = runRoom (x, resonantRoom (x, 1000.0, 8.0, 1.0, 12.0), 1.0e-4f);
    const auto short_ = runRoom (x, resonantRoom (x, 1000.0, 8.0, 1.0,  0.7), 1.0e-4f);

    ASSERT_TRUE (full.measured);
    ASSERT_TRUE (short_.measured);

    const int k = LoopGainEstimator::hzToBin (1000.0, kSr);
    const double lost = full.hDb[(std::size_t) k] - short_.hDb[(std::size_t) k];

    // Both halves matter: "lower" on its own would also pass for an estimator
    // that simply reads too low everywhere.
    EXPECT_GT (lost, 0.0);
    EXPECT_LT (lost, 12.0);
    EXPECT_NEAR (lost, 2.62, 1.5)
        << "truncation error moved; re-derive it from the block comment above "
           "before editing this number";
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

**If `DecayLongerThanTheTailIsUnderRead`'s 2.62 dB is not what the synthetic room produces, do NOT edit the number to match** — re-derive it from the block comment in that test, which is the authority. The chain is: the sweep reaches 1 kHz at `t_1k = kSweepSeconds · ln(1000/100) / ln(10000/100) = 3.0 × 0.5 = 1.5 s`, so the short window keeps `(3.0 − 1.5) + 0.7 = 2.2 s` of the ring-out; with `T60 = 8.0 s` the captured energy fraction is `1 − 10^(−6 × 2.2 / 8.0) = 0.5477`, and the shortfall is `−10·log10(0.5477) = 2.62 dB`. **Do not use the earlier 2.0 s figure**: at `T60 = 2.0` the same arithmetic gives `1 − 10^(−6 × 2.2 / 2.0) = 0.99921`, a shortfall of 0.0035 dB, which no test can measure — that was plan rev 1's defect (I-2). Keep the derivation in the comment beside the number, as lane G's B-4 lesson requires ("write the arithmetic in the comment or it drifts").

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

