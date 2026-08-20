// NotchChain tests
//
// A NotchChain holds up to 16 biquads in series per audio channel.
// Idle filters are bypassed (input passes through); Active filters
// attenuate their target frequencies. The container pre-allocates
// everything in its constructor so processSample() is allocation-free.

#include <gtest/gtest.h>
#include "dsp/NotchChain.h"
#include "dsp/Biquad.h"

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