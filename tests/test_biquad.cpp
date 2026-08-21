// Biquad notch filter tests
//
// Exercises the RBJ Audio EQ Cookbook notch biquad: targets a single
// frequency, passes other frequencies through, and resets state cleanly.
// These are functional single-threaded tests -- real-time thread safety
// is a property of the caller, not the filter.

#include <gtest/gtest.h>
#include "dsp/Biquad.h"

// juce::ScopedNoDenormals lives in juce_audio_basics (not juce_core); the
// test target already links juce_dsp, which includes juce_audio_basics.
#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>
#include <vector>

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

    // Charge a 1 kHz / Q=10 notch at 48 kHz with a full-scale tone, then feed
    // it `silenceSamples` of EXACT 0.0 and return the last output sample.
    // Used by the denormal test below; the filter is constructed fresh on
    // every call so the two halves of that test cannot contaminate each other.
    double chargeThenFeedSilence(int silenceSamples)
    {
        Biquad filter;
        filter.setNotchFilter(1000.0, 10.0, 48000.0);

        const auto tone = sineWave(1000.0, 48000.0, 4800);
        for (double s : tone)
        {
            (void)filter.processSample(s);
        }

        double out = 0.0;
        for (int i = 0; i < silenceSamples; ++i)
        {
            out = filter.processSample(0.0);
        }
        return out;
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
//==============================================================================
// A1 -- denormal limit cycle on silence.

// A notch at Q=10 has poles at radius sqrt((1-alpha)/(1+alpha)) with
// alpha = sin(2*pi*1000/48000)/(2*10) = 0.0065263096, i.e. radius 0.99349485.
// On exact silence the Direct-Form state decays by that factor per sample, so
// it enters the subnormal range (< 2.225e-308) after
// ln(2.225e-308)/ln(0.99349485) ~= 108,000 samples -- an independent
// double-precision reference run of this exact difference equation puts the
// first subnormal output at silence sample 107,557.
//
// It then NEVER reaches zero. The smallest positive double is 4.94e-324, and
// 0.99349485 * 4.94e-324 = 4.907e-324, which is nearer to 4.94e-324 than to 0
// and so rounds straight back to it. The reference run confirms this: after
// 240,000 silent samples (5 s at 48 kHz) the output is still 1.93e-322.
//
// Subnormal SSE arithmetic traps to microcode. The reviewer measured 16 active
// notches under this project's Release flags going from 1.7 ms of CPU per
// second of audio to 94 ms per second, permanently, once the state fell into
// this range. juce::ScopedNoDenormals sets FTZ/DAZ in MXCSR for its scope,
// which flushes the state to a true zero and ends the cycle -- that is the fix
// applied at the top of AudioEngine::audioDeviceIOCallbackWithContext().
TEST(Biquad, SilenceDoesNotLeaveDenormalState)
{
    constexpr int kFiveSecondsAt48k = 240000;

    {
        const juce::ScopedNoDenormals noDenormals;

        const double guarded = chargeThenFeedSilence(kFiveSecondsAt48k);

        // Exact equality on purpose: EXPECT_DOUBLE_EQ tolerates 4 ULP, and
        // 4 ULP of 0.0 is ~2e-323, which would happily accept the very
        // subnormal this test exists to rule out.
        EXPECT_EQ(guarded, 0.0) << "tail sample under ScopedNoDenormals = " << guarded;
        EXPECT_NE(std::fpclassify(guarded), FP_SUBNORMAL);
    }

    // Second half: the same run with denormals ENABLED must still be sitting
    // on a subnormal. This half documents why the fix is needed. If a future
    // toolchain or compiler flag turns FTZ on globally this assertion starts
    // failing, at which point the hazard can be retired deliberately instead
    // of being rediscovered from scratch.
    const double unguarded = chargeThenFeedSilence(kFiveSecondsAt48k);

    EXPECT_EQ(std::fpclassify(unguarded), FP_SUBNORMAL)
        << "tail sample without ScopedNoDenormals = " << unguarded
        << " (FP_SUBNORMAL=" << FP_SUBNORMAL << ", FP_ZERO=" << FP_ZERO
        << ", FP_NORMAL=" << FP_NORMAL << ")";
    EXPECT_NE(unguarded, 0.0);
}

//==============================================================================
// A2 -- setNotchFilter parameter validation.

TEST(Biquad, RejectsFrequencyAtOrAboveNyquist)
{
    // A default-constructed Biquad is a pass-through: b0 = 1 and every other
    // coefficient is 0, so its impulse response is exactly {1, 0, 0, ...}.
    // A rejected setNotchFilter() must leave it in precisely that state.
    //
    // Why these two are illegal (pole radius = sqrt((1-alpha)/(1+alpha)),
    // alpha = sin(omega)/(2Q), omega = 2*pi*freq/sampleRate):
    //
    //   24000 Hz @ 48 kHz: omega = pi exactly. sin(pi) evaluates to 1.22e-16
    //     in double, so alpha = 6.1e-18 and the pole radius is 1.0 to within
    //     an ulp -- poles ON the unit circle, undamped ringing forever.
    //   30000 Hz @ 48 kHz: omega = 3.92699 > pi, so sin(omega) = -0.70711 and
    //     alpha = -0.035355. Then (1-alpha) = 1.035355 > (1+alpha) = 0.964645
    //     and the pole radius is sqrt(1.035355/0.964645) = 1.03600 > 1 --
    //     the filter is a divergent oscillator.
    constexpr double kSampleRate = 48000.0;
    Biquad filter;

    EXPECT_FALSE(filter.setNotchFilter(24000.0, 10.0, kSampleRate));
    EXPECT_FALSE(filter.setNotchFilter(30000.0, 10.0, kSampleRate));

    // Impulse response unchanged from before the calls: {1, 0, 0, ...}.
    EXPECT_DOUBLE_EQ(filter.processSample(1.0), 1.0);
    for (int i = 0; i < 64; ++i)
    {
        EXPECT_DOUBLE_EQ(filter.processSample(0.0), 0.0);
    }
}

TEST(Biquad, RejectsNonPositiveQ)
{
    // Q == 0 divides by zero in alpha = sin(omega)/(2Q), giving alpha = +inf,
    // so a0 = 1 + inf = inf and every normalised coefficient becomes inf/inf
    // = NaN. That is the worse of the two failures, because NaN compares
    // false against every sanity check downstream -- nothing would notice.
    // Q < 0 flips alpha's sign and pushes the pole radius above 1 in exactly
    // the way an above-Nyquist frequency does.
    constexpr double kSampleRate = 48000.0;
    Biquad filter;

    EXPECT_FALSE(filter.setNotchFilter(1000.0, 0.0, kSampleRate));
    EXPECT_FALSE(filter.setNotchFilter(1000.0, -1.0, kSampleRate));

    // Still the untouched pass-through, and above all still finite.
    double out = filter.processSample(1.0);
    EXPECT_TRUE(std::isfinite(out));
    EXPECT_DOUBLE_EQ(out, 1.0);

    for (int i = 0; i < 64; ++i)
    {
        out = filter.processSample(0.0);
        EXPECT_TRUE(std::isfinite(out));
        EXPECT_DOUBLE_EQ(out, 0.0);
    }
}

TEST(Biquad, RejectsNonPositiveFrequencyAndSampleRate)
{
    // The other two halves of the same guard. freq <= 0 is meaningless (and
    // freq == 0 gives alpha == 0: poles on the unit circle again), and a
    // sampleRate <= 0 makes omega infinite or negative.
    constexpr double kSampleRate = 48000.0;
    Biquad filter;

    EXPECT_FALSE(filter.setNotchFilter(0.0, 10.0, kSampleRate));
    EXPECT_FALSE(filter.setNotchFilter(-1000.0, 10.0, kSampleRate));
    EXPECT_FALSE(filter.setNotchFilter(1000.0, 10.0, 0.0));
    EXPECT_FALSE(filter.setNotchFilter(1000.0, 10.0, -48000.0));

    EXPECT_DOUBLE_EQ(filter.processSample(1.0), 1.0);
    for (int i = 0; i < 64; ++i)
    {
        EXPECT_DOUBLE_EQ(filter.processSample(0.0), 0.0);
    }
}

TEST(Biquad, AcceptsFrequencyJustBelowNyquist)
{
    // Pin the guard from the accepting side too, or it is untested: an
    // over-eager guard that rejected everything would satisfy the two
    // rejection tests above.
    //
    // 0.49 * 48000 = 23520 Hz:
    //   omega = 2*pi*0.49        = 3.078761
    //   sin(omega)               = 0.0627905
    //   alpha = sin(omega)/(2*10)= 0.00313953
    //   pole radius = sqrt((1 - 0.00313953)/(1 + 0.00313953)) = 0.996865 < 1
    //
    // Independent double-precision simulation of this exact difference
    // equation, 8192 samples, RMS over the second half:
    //   at 23520 Hz (the notch)      -> 3.55e-07   (assert < 0.25)
    //   at 11760 Hz (an octave down) -> 0.707099   (ratio to 1/sqrt(2)
    //                                               = 0.99999, assert > 0.9)
    // The control tone is an octave DOWN, not up: an octave up would be
    // 47040 Hz, above Nyquist and therefore not a signal at all.
    constexpr double kSampleRate = 48000.0;
    constexpr double kFreq       = 0.49 * kSampleRate;   // 23520 Hz

    Biquad filter;
    EXPECT_TRUE(filter.setNotchFilter(kFreq, 10.0, kSampleRate));

    auto atTarget = sineWave(kFreq, kSampleRate, 8192);
    for (auto& s : atTarget)
    {
        s = filter.processSample(s);
    }
    EXPECT_LT(rms(atTarget, 4096), 0.25);

    filter.reset();
    auto control = sineWave(11760.0, kSampleRate, 8192);
    for (auto& s : control)
    {
        s = filter.processSample(s);
    }
    EXPECT_GT(rms(control, 4096) / (1.0 / std::sqrt(2.0)), 0.9);
}

//==============================================================================
// Finite-depth notch (spec 5.1: depth 6-24 dB).
//
// The three-argument setNotchFilter implements the RBJ pure notch, which is an
// INFINITE-depth null: it has no depth parameter at all, so NotchChain could
// store depthDB and had nothing to pass it to. Every notch this app placed was
// therefore a full null regardless of the depth requested, and spec 5.1's
// 6-24 dB range -- along with Task 26's "Speech" -18 dB and "Music" -10 dB
// presets -- was not representable.
//
// The four-argument form is the RBJ peaking filter driven with negative gain.
// At the centre frequency its magnitude response is exactly A^2 where
// A = 10^(dB/40), i.e. 10^(dB/20) -- the requested depth, by construction. The
// pure notch is its dB -> -inf limit, which is why the three-argument form is
// kept: it is a real filter, it is well covered by the tests above, and it is
// the mathematical parent of the new one.

TEST(Biquad, DepthAttenuatesByExactlyTheRequestedDecibels)
{
    // A -12 dB notch must attenuate its target by 12 dB -- NOT to silence.
    // This is the whole defect: before the four-argument form existed, this
    // filter drove the target to a full null and -12.0 was ignored.
    constexpr double kSampleRate = 48000.0;
    constexpr double kFreq       = 1000.0;
    constexpr double kDepthDB    = -12.0;

    Biquad filter;
    ASSERT_TRUE(filter.setNotchFilter(kFreq, 10.0, kSampleRate, kDepthDB));

    auto samples = sineWave(kFreq, kSampleRate, 16384);
    for (auto& s : samples)
    {
        s = filter.processSample(s);
    }

    const double inputRms = 1.0 / std::sqrt(2.0);
    const double expected = inputRms * std::pow(10.0, kDepthDB / 20.0);

    // 2% tolerance: the residual is a steady-state amplitude measured over a
    // finite window, so edge effects and the tail of the transient both show up.
    EXPECT_NEAR(rms(samples, 8192), expected, expected * 0.02);
}

TEST(Biquad, DepthIsHonouredAcrossTheSpecifiedRange)
{
    // Spec 5.1 allows 6-24 dB. Each endpoint must produce its own depth, so a
    // shallower setting is audibly gentler than a deeper one. A single-depth
    // test would pass even if the depth were quietly clamped to one constant.
    constexpr double kSampleRate = 48000.0;
    constexpr double kFreq       = 1000.0;
    const double     inputRms    = 1.0 / std::sqrt(2.0);

    for (const double depthDB : { -6.0, -18.0, -24.0 })
    {
        Biquad filter;
        ASSERT_TRUE(filter.setNotchFilter(kFreq, 10.0, kSampleRate, depthDB))
            << "depth " << depthDB;

        auto samples = sineWave(kFreq, kSampleRate, 16384);
        for (auto& s : samples)
        {
            s = filter.processSample(s);
        }

        const double expected = inputRms * std::pow(10.0, depthDB / 20.0);
        EXPECT_NEAR(rms(samples, 8192), expected, expected * 0.02)
            << "depth " << depthDB;
    }
}

TEST(Biquad, DepthLeavesOffTargetContentAlone)
{
    // A finite-depth notch is WIDER in effect than a null at the same Q is
    // deep, so the off-target guarantee has to be re-established rather than
    // inherited from the pure-notch tests.
    constexpr double kSampleRate = 48000.0;

    Biquad filter;
    ASSERT_TRUE(filter.setNotchFilter(1000.0, 10.0, kSampleRate, -12.0));

    auto control = sineWave(2000.0, kSampleRate, 8192);
    for (auto& s : control)
    {
        s = filter.processSample(s);
    }

    EXPECT_GT(rms(control, 4096) / (1.0 / std::sqrt(2.0)), 0.9);
}

TEST(Biquad, RejectsPositiveDepthBecauseItWouldBoostTheRingingFrequency)
{
    // THE dangerous input for this product. The peaking form is symmetric:
    // the same coefficients with a POSITIVE gain amplify the centre frequency
    // by that many dB. A sign error anywhere upstream -- a GUI field, a preset
    // file, a detector that forgets to negate -- would turn the feedback
    // killer into a feedback AMPLIFIER, boosting precisely the frequency that
    // is already ringing.
    //
    // Rejected the same way every other invalid parameter is: return false and
    // leave the filter exactly as it was, rather than half-writing it.
    constexpr double kSampleRate = 48000.0;

    Biquad filter;
    ASSERT_TRUE(filter.setNotchFilter(1000.0, 10.0, kSampleRate, -12.0));

    EXPECT_FALSE(filter.setNotchFilter(1000.0, 10.0, kSampleRate, +12.0));

    // Still the -12 dB filter it was before the rejected call.
    auto samples = sineWave(1000.0, kSampleRate, 16384);
    for (auto& s : samples)
    {
        s = filter.processSample(s);
    }

    const double expected = (1.0 / std::sqrt(2.0)) * std::pow(10.0, -12.0 / 20.0);
    EXPECT_NEAR(rms(samples, 8192), expected, expected * 0.02);
}

TEST(Biquad, DepthFormAppliesTheSameParameterGuardsAsThePureNotch)
{
    // The stability derivation in Biquad.h applies unchanged: the pole radius
    // is sqrt((1 - alpha/A)/(1 + alpha/A)), which is the pure-notch expression
    // with alpha replaced by alpha/A. Every input that made the pure notch
    // diverge does the same here and must be refused identically.
    constexpr double kSampleRate = 48000.0;

    Biquad filter;
    EXPECT_FALSE(filter.setNotchFilter(1000.0, 10.0, 0.0,   -12.0));  // rate <= 0
    EXPECT_FALSE(filter.setNotchFilter(1000.0, 0.0,  kSampleRate, -12.0));  // Q <= 0
    EXPECT_FALSE(filter.setNotchFilter(0.0,    10.0, kSampleRate, -12.0));  // freq <= 0
    EXPECT_FALSE(filter.setNotchFilter(24000.0, 10.0, kSampleRate, -12.0)); // >= Nyquist
    EXPECT_FALSE(filter.setNotchFilter(30000.0, 10.0, kSampleRate, -12.0)); // above Nyquist
}
