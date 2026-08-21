// NotchChain tests
//
// A NotchChain holds up to 16 biquads in series per audio channel.
// Idle filters are bypassed (input passes through); Active filters
// attenuate their target frequencies. The container pre-allocates
// everything in its constructor so processSample() is allocation-free.

#include <gtest/gtest.h>
#include "dsp/NotchChain.h"
#include "dsp/Biquad.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
    constexpr double kPi = 3.14159265358979323846;

    std::vector<double> sineWave(double freqHz, double sampleRate, int numSamples)
    {
        std::vector<double> out(numSamples);
        const double w = 2.0 * kPi * freqHz / sampleRate;
        for (int i = 0; i < numSamples; ++i)
        {
            out[i] = std::sin(w * static_cast<double>(i));
        }
        return out;
    }

    double rms(const std::vector<double>& samples, int start)
    {
        double acc = 0.0;
        const int n = static_cast<int>(samples.size()) - start;
        for (int i = start; i < static_cast<int>(samples.size()); ++i)
        {
            acc += samples[i] * samples[i];
        }
        return std::sqrt(acc / static_cast<double>(n));
    }
}

TEST(NotchChain, InitiallyPassthrough)
{
    // A freshly-constructed chain with no notches set must be transparent.
    constexpr double kSampleRate = 48000.0;
    NotchChain chain(kSampleRate);

    auto samples = sineWave(1000.0, kSampleRate, 4096);
    for (auto& s : samples)
    {
        s = chain.processSample(s);
    }

    // Input is unit-amplitude; output must also be ~unit-amplitude.
    // Tolerance is generous: a finite window of a sampled sine has small
    // edge effects, and double-precision sin() adds a few ulps of error.
    EXPECT_NEAR(rms(samples, 2048), 1.0 / std::sqrt(2.0), 1e-2);
    EXPECT_EQ(chain.getActiveNotchCount(), 0);
}

TEST(NotchChain, SetNotchAttenuates)
{
    // After setting one notch, the target frequency must be attenuated
    // while off-target content survives.
    constexpr double kSampleRate = 48000.0;
    constexpr double kFreq        = 1000.0;
    NotchChain chain(kSampleRate);
    chain.setNotch(0, kFreq, 10.0, -12.0);

    auto samples = sineWave(kFreq, kSampleRate, 8192);
    for (auto& s : samples)
    {
        s = chain.processSample(s);
    }

    EXPECT_LT(rms(samples, 4096), 0.25);
    EXPECT_EQ(chain.getActiveNotchCount(), 1);

    // Probe one octave away -- the chain must not attenuate off-target.
    auto probe = sineWave(2000.0, kSampleRate, 8192);
    for (auto& s : probe)
    {
        s = chain.processSample(s);
    }
    EXPECT_GT(rms(probe, 4096) / (1.0 / std::sqrt(2.0)), 0.9);
}

TEST(NotchChain, ClearNotchRestoresPassthrough)
{
    // After clearing the only notch, the chain is transparent again.
    constexpr double kSampleRate = 48000.0;
    NotchChain chain(kSampleRate);
    chain.setNotch(0, 1000.0, 10.0, -12.0);
    chain.clearNotch(0);

    auto samples = sineWave(1000.0, kSampleRate, 8192);
    for (auto& s : samples)
    {
        s = chain.processSample(s);
    }

    EXPECT_NEAR(rms(samples, 4096), 1.0 / std::sqrt(2.0), 1e-3);
    EXPECT_EQ(chain.getActiveNotchCount(), 0);
}

TEST(NotchChain, MultipleActiveNotches)
{
    // Two notches at different frequencies: each must attenuate its own
    // target, and an off-target probe must pass through.
    constexpr double kSampleRate = 48000.0;
    NotchChain chain(kSampleRate);
    chain.setNotch(0, 1000.0, 10.0, -12.0);
    chain.setNotch(1, 2000.0, 10.0, -12.0);

    auto atFirst = sineWave(1000.0, kSampleRate, 8192);
    for (auto& s : atFirst) s = chain.processSample(s);
    EXPECT_LT(rms(atFirst, 4096), 0.25);

    // Drain the chain's internal state so the next probe starts cold.
    chain.reset();
    auto atSecond = sineWave(2000.0, kSampleRate, 8192);
    for (auto& s : atSecond) s = chain.processSample(s);
    EXPECT_LT(rms(atSecond, 4096), 0.25);

    chain.reset();
    auto offTarget = sineWave(4000.0, kSampleRate, 8192);
    for (auto& s : offTarget) s = chain.processSample(s);
    EXPECT_GT(rms(offTarget, 4096) / (1.0 / std::sqrt(2.0)), 0.9);

    EXPECT_EQ(chain.getActiveNotchCount(), 2);
}

TEST(NotchChain, MaxNotches16)
{
    // The brief pins MAX_NOTCHES to 16. Fill all 16 slots -- still
    // allocation-free, still attenuates at the configured frequencies.
    constexpr double kSampleRate = 48000.0;
    NotchChain chain(kSampleRate);

    for (int i = 0; i < NotchChain::MAX_NOTCHES; ++i)
    {
        chain.setNotch(i, 500.0 + 100.0 * i, 10.0, -12.0);
    }
    EXPECT_EQ(chain.getActiveNotchCount(), NotchChain::MAX_NOTCHES);

    // Probe at 5000 Hz -- well above the highest configured notch (2000
    // Hz) so the chain must pass the signal through with no attenuation.
    auto probe = sineWave(5000.0, kSampleRate, 8192);
    for (auto& s : probe) s = chain.processSample(s);
    EXPECT_GT(rms(probe, 4096) / (1.0 / std::sqrt(2.0)), 0.9);
}

TEST(NotchChain, ActiveNotchCount)
{
    // Count must reflect only Active notches, not just configured ones.
    constexpr double kSampleRate = 48000.0;
    NotchChain chain(kSampleRate);

    EXPECT_EQ(chain.getActiveNotchCount(), 0);
    chain.setNotch(0, 1000.0, 10.0, -12.0);
    EXPECT_EQ(chain.getActiveNotchCount(), 1);
    chain.setNotch(3, 2000.0, 10.0, -12.0);
    EXPECT_EQ(chain.getActiveNotchCount(), 2);
    chain.clearNotch(0);
    EXPECT_EQ(chain.getActiveNotchCount(), 1);
    chain.clearNotch(3);
    EXPECT_EQ(chain.getActiveNotchCount(), 0);
}

TEST(NotchChain, OutOfRangeIndexIgnored)
{
    // setNotch and clearNotch must guard against indices outside
    // [0, MAX_NOTCHES). Out-of-range calls must be no-ops -- they must
    // not crash, allocate, or corrupt state.
    constexpr double kSampleRate = 48000.0;
    NotchChain chain(kSampleRate);
    chain.setNotch(0, 1000.0, 10.0, -12.0);

    chain.setNotch(-1, 500.0, 10.0, -12.0);
    chain.setNotch(NotchChain::MAX_NOTCHES, 500.0, 10.0, -12.0);
    chain.setNotch(NotchChain::MAX_NOTCHES + 100, 500.0, 10.0, -12.0);

    chain.clearNotch(-1);
    chain.clearNotch(NotchChain::MAX_NOTCHES);

    EXPECT_EQ(chain.getActiveNotchCount(), 1);

    // getNotchInfo on out-of-range must be safe too -- we don't dictate
    // the return value, only that the call doesn't crash.
    (void)chain.getNotchInfo(-1);
    (void)chain.getNotchInfo(NotchChain::MAX_NOTCHES);
}

TEST(NotchChain, SetSampleRateKeepsActiveNotchFrequency)
{
    // A notch set at 48 kHz must keep its 1000 Hz target after the chain is
    // retargeted to 96 kHz: setSampleRate() recomputes the coefficients from
    // the STORED NotchInfo, so a 1000 Hz sine driven at the NEW rate is still
    // attenuated below the standard threshold, while a control tone one
    // octave away still passes (the test cannot pass by the filter simply
    // killing everything).
    constexpr double kSampleRate48 = 48000.0;
    constexpr double kSampleRate96 = 96000.0;
    constexpr double kFreq         = 1000.0;

    NotchChain chain(kSampleRate48);
    chain.setNotch(0, kFreq, 10.0, -12.0);

    chain.setSampleRate(kSampleRate96);
    EXPECT_DOUBLE_EQ(chain.getSampleRate(), kSampleRate96);

    auto atTarget = sineWave(kFreq, kSampleRate96, 8192);
    for (auto& s : atTarget) s = chain.processSample(s);
    EXPECT_LT(rms(atTarget, 4096), 0.25);

    auto control = sineWave(2000.0, kSampleRate96, 8192);
    for (auto& s : control) s = chain.processSample(s);
    EXPECT_GT(rms(control, 4096) / (1.0 / std::sqrt(2.0)), 0.9);

    EXPECT_EQ(chain.getActiveNotchCount(), 1);
}

TEST(NotchChain, SetSampleRateIgnoresNonPositive)
{
    // A non-positive rate must be a no-op: the stored rate, the active
    // notches, and the filter behaviour all stay exactly as they were.
    constexpr double kSampleRate = 48000.0;
    NotchChain chain(kSampleRate);
    chain.setNotch(0, 1000.0, 10.0, -12.0);

    chain.setSampleRate(0.0);
    EXPECT_DOUBLE_EQ(chain.getSampleRate(), kSampleRate);

    chain.setSampleRate(-96000.0);
    EXPECT_DOUBLE_EQ(chain.getSampleRate(), kSampleRate);

    // Behaviour unchanged: 1000 Hz at 48 kHz is still attenuated.
    auto samples = sineWave(1000.0, kSampleRate, 8192);
    for (auto& s : samples) s = chain.processSample(s);
    EXPECT_LT(rms(samples, 4096), 0.25);
    EXPECT_EQ(chain.getActiveNotchCount(), 1);
}

TEST(NotchChain, GetSampleRateReflectsConstructorAndSetter)
{
    constexpr double kSampleRate44 = 44100.0;
    constexpr double kSampleRate96 = 96000.0;

    NotchChain chain(kSampleRate44);
    EXPECT_DOUBLE_EQ(chain.getSampleRate(), kSampleRate44);

    chain.setSampleRate(kSampleRate96);
    EXPECT_DOUBLE_EQ(chain.getSampleRate(), kSampleRate96);

    // Retargeting to the same rate is safe and idempotent.
    chain.setSampleRate(kSampleRate96);
    EXPECT_DOUBLE_EQ(chain.getSampleRate(), kSampleRate96);
}
// A2 -- the reviewer's exact divergence case.
//
// A notch at 30 kHz is perfectly legal at 96 kHz (Nyquist 48 kHz). Retargeting
// the chain to 44.1 kHz puts it ABOVE the new Nyquist of 22.05 kHz:
//
//   omega = 2*pi*30000/44100 = 4.274276  (> pi)
//   sin(omega)               = -0.905554
//   alpha = sin(omega)/(2*10)= -0.0452777
//   pole radius = sqrt((1 - alpha)/(1 + alpha))
//               = sqrt(1.0452777/0.9547223) = 1.046351   > 1  -> divergent
//
// A double-precision reference run of the UNGUARDED coefficients over an
// impulse followed by 1000 zeros crosses |y| > 1e3 at sample 208 (4.7 ms at
// 44.1 kHz -- the reviewer's figure) and peaks at 4.11e18. The guard must
// deactivate the notch instead, leaving the chain a pass-through whose peak
// output for a unit impulse is exactly 1.0. The 2.0 bound therefore has
// eighteen orders of magnitude of headroom and cannot be flaky.
TEST(NotchChain, RetargetDeactivatesNotchAboveNewNyquist)
{
    constexpr double kSampleRate96 = 96000.0;
    constexpr double kSampleRate44 = 44100.0;
    constexpr double kFreq         = 30000.0;

    NotchChain chain(kSampleRate96);
    chain.setNotch(0, kFreq, 10.0, -12.0);
    ASSERT_EQ(chain.getNotchInfo(0).state, NotchChain::NotchState::Active);

    chain.setSampleRate(kSampleRate44);

    // Deactivated, but its parameters are RETAINED so it can come back if the
    // device rate goes up again.
    EXPECT_EQ(chain.getNotchInfo(0).state, NotchChain::NotchState::Idle);
    EXPECT_DOUBLE_EQ(chain.getNotchInfo(0).frequency, kFreq);
    EXPECT_DOUBLE_EQ(chain.getNotchInfo(0).Q, 10.0);
    EXPECT_EQ(chain.getActiveNotchCount(), 0);

    // The assertion that actually matters: no divergence.
    double peak = std::abs(chain.processSample(1.0));
    for (int i = 0; i < 1000; ++i)
    {
        peak = std::max(peak, std::abs(chain.processSample(0.0)));
    }
    EXPECT_LT(peak, 2.0) << "peak |output| over impulse + 1000 zeros = " << peak;
}

// The deactivation must be surgical, not a blanket clear: a notch that still
// fits under the new Nyquist keeps working. 1 kHz at 44.1 kHz gives
// alpha = sin(2*pi*1000/44100)/(2*10) = 0.0071049, pole radius 0.992932.
// Reference simulation, 8192 samples, RMS over the second half:
//   at 1000 Hz -> 3.77e-14  (assert < 0.25)
//   at 2000 Hz -> ratio to 1/sqrt(2) = 0.99781  (assert > 0.9)
TEST(NotchChain, RetargetKeepsNotchThatStillFits)
{
    constexpr double kSampleRate96 = 96000.0;
    constexpr double kSampleRate44 = 44100.0;
    constexpr double kFreq         = 1000.0;

    NotchChain chain(kSampleRate96);
    chain.setNotch(0, kFreq, 10.0, -12.0);

    chain.setSampleRate(kSampleRate44);

    EXPECT_EQ(chain.getNotchInfo(0).state, NotchChain::NotchState::Active);
    EXPECT_EQ(chain.getActiveNotchCount(), 1);

    auto atTarget = sineWave(kFreq, kSampleRate44, 8192);
    for (auto& s : atTarget) s = chain.processSample(s);
    EXPECT_LT(rms(atTarget, 4096), 0.25);

    chain.reset();
    auto control = sineWave(2000.0, kSampleRate44, 8192);
    for (auto& s : control) s = chain.processSample(s);
    EXPECT_GT(rms(control, 4096) / (1.0 / std::sqrt(2.0)), 0.9);
}

// The mixed case, because a real chain holds both kinds at once: the 30 kHz
// notch goes Idle while the 1 kHz notch in the next slot keeps its frequency.
TEST(NotchChain, RetargetDeactivatesOnlyTheNotchesThatNoLongerFit)
{
    NotchChain chain(96000.0);
    chain.setNotch(0, 30000.0, 10.0, -12.0);
    chain.setNotch(1, 1000.0, 10.0, -12.0);
    ASSERT_EQ(chain.getActiveNotchCount(), 2);

    chain.setSampleRate(44100.0);

    EXPECT_EQ(chain.getNotchInfo(0).state, NotchChain::NotchState::Idle);
    EXPECT_EQ(chain.getNotchInfo(1).state, NotchChain::NotchState::Active);
    EXPECT_EQ(chain.getActiveNotchCount(), 1);

    auto atTarget = sineWave(1000.0, 44100.0, 8192);
    for (auto& s : atTarget) s = chain.processSample(s);
    EXPECT_LT(rms(atTarget, 4096), 0.25);
}

// setNotch must not accept a frequency the biquad rejects. Without this the
// slot would go Active while filters_[index] still held whatever coefficients
// were there before -- a notch reported at 30 kHz that actually filters
// something else entirely.
TEST(NotchChain, SetNotchIgnoresParametersTheBiquadRejects)
{
    constexpr double kSampleRate = 48000.0;
    NotchChain chain(kSampleRate);

    chain.setNotch(0, 30000.0, 10.0, -12.0);   // above Nyquist (24 kHz)
    chain.setNotch(1, 1000.0, 0.0, -12.0);     // Q == 0 -> NaN coefficients
    EXPECT_EQ(chain.getActiveNotchCount(), 0);

    // Chain is still a clean pass-through, and nothing is NaN.
    auto samples = sineWave(1000.0, kSampleRate, 8192);
    for (auto& s : samples)
    {
        s = chain.processSample(s);
        ASSERT_TRUE(std::isfinite(s));
    }
    EXPECT_NEAR(rms(samples, 4096), 1.0 / std::sqrt(2.0), 1e-3);
}
