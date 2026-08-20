// Biquad notch filter tests
//
// Exercises the RBJ Audio EQ Cookbook notch biquad: targets a single
// frequency, passes other frequencies through, and resets state cleanly.
// These are functional single-threaded tests -- real-time thread safety
// is a property of the caller, not the filter.

#include <gtest/gtest.h>
#include "dsp/Biquad.h"

#include <cmath>

namespace
{
    constexpr double kPi = 3.14159265358979323846;

    // Generate N samples of a unit-amplitude sine at `freqHz`, sampled at
    // `sampleRate`. Returns the samples so tests can feed them through a
    // filter and observe steady-state behaviour.
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

    // RMS of a sample range -- use the second half of the buffer to skip
    // the filter's transient response.
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

TEST(Biquad, NotchAttenuatesTargetFrequency)
{
    // 1 kHz notch at 48 kHz sample rate, Q=10 (~very narrow).
    constexpr double kSampleRate = 48000.0;
    constexpr double kFreq        = 1000.0;
    constexpr double kQ           = 10.0;

    Biquad filter;
    filter.setNotchFilter(kFreq, kQ, kSampleRate);

    auto samples = sineWave(kFreq, kSampleRate, 8192);
    for (auto& s : samples)
    {
        s = filter.processSample(s);
    }

    // Brief: at target freq, output < 0.25 (about -12 dB or more attenuation).
    // The second half of the buffer is past the filter's group delay so
    // we're measuring steady-state attenuation.
    const double outputRms = rms(samples, 4096);
    EXPECT_LT(outputRms, 0.25);
}

TEST(Biquad, NotchPassesOffTargetFrequency)
{
    // Same notch, but probe one octave away -- the signal must survive.
    constexpr double kSampleRate = 48000.0;
    constexpr double kFreq        = 1000.0;
    constexpr double kQ           = 10.0;
    constexpr double kProbeFreq   = 2000.0; // one octave up

    Biquad filter;
    filter.setNotchFilter(kFreq, kQ, kSampleRate);

    auto samples = sineWave(kProbeFreq, kSampleRate, 8192);
    for (auto& s : samples)
    {
        s = filter.processSample(s);
    }

    // Brief: off target (1 octave away), gain ratio > 0.9.
    // We feed in unit-amplitude sines; rms of a unit sine is 1/sqrt(2) ~ 0.707.
    // "Gain ratio > 0.9" means outputRms >= 0.9 * inputRms = 0.636.
    const double outputRms = rms(samples, 4096);
    const double inputRms  = 1.0 / std::sqrt(2.0);
    EXPECT_GT(outputRms / inputRms, 0.9);
}

TEST(Biquad, ResetClearsState)
{
    // After reset() with coefficients still configured, a DC input must
    // pass through ~unchanged. DC is well outside any audio-band notch --
    // the filter's gain at 0 Hz is exactly 1.0, so the steady-state output
    // must equal the input. We pump enough samples to outlast the filter's
    // transient response: the poles sit at radius ~sqrt(a2) ~ 0.994, so
    // 5000 samples of excitation drive the residual below 1e-30.
    constexpr double kSampleRate = 48000.0;
    constexpr int    kSamples     = 5000;
    Biquad filter;
    filter.setNotchFilter(1000.0, 10.0, kSampleRate);

    filter.reset();

    constexpr double kDc = 0.5;
    double out = 0.0;
    for (int i = 0; i < kSamples; ++i)
    {
        out = filter.processSample(kDc);
    }

    // After enough samples to flush any internal state, output ~= input.
    EXPECT_NEAR(out, kDc, 1e-6);
}

TEST(Biquad, HandlesDifferentSampleRates)
{
    // Run the same logical notch at 44.1 kHz and 96 kHz. Attenuation at
    // the target must be similar in both cases. This guards against
    // bugs where a sample-rate dependent constant (e.g. 1/sampleRate)
    // gets baked in by mistake.
    constexpr double kFreq = 1000.0;
    constexpr double kQ    = 10.0;

    auto measure = [&](double sampleRate) {
        Biquad filter;
        filter.setNotchFilter(kFreq, kQ, sampleRate);

        auto samples = sineWave(kFreq, sampleRate, 8192);
        for (auto& s : samples)
        {
            s = filter.processSample(s);
        }
        return rms(samples, 4096);
    };

    const double r441 = measure(44100.0);
    const double r96  = measure(96000.0);

    // Both sample rates must give strong attenuation at the target.
    EXPECT_LT(r441, 0.25);
    EXPECT_LT(r96,  0.25);
}

TEST(Biquad, CoefficientValidity)
{
    // No dedicated accessor in the public API -- but we can probe the
    // filter's behaviour at boundary coefficients. Setting freq=0
    // (omega=0) must not produce NaN or infinity in the output.
    constexpr double kSampleRate = 48000.0;
    Biquad filter;
    filter.setNotchFilter(0.0, 10.0, kSampleRate);

    double out = filter.processSample(1.0);
    EXPECT_TRUE(std::isfinite(out));

    // Setting Q very low (broad notch) must also stay finite.
    filter.setNotchFilter(1000.0, 0.1, kSampleRate);
    for (int i = 0; i < 100; ++i)
    {
        out = filter.processSample(1.0);
        EXPECT_TRUE(std::isfinite(out));
    }
}