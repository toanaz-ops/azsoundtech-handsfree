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

