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
// Plan rev 1 shipped t60 = 2.0 with an expectation of 3.0 dB, and the brief that
// replaced it shipped t60 = 8.0 with an expectation of 2.62 dB. BOTH expectations
// are misevaluations of the formula printed directly above them, and the brief's
// own rule -- "re-derive it from the block comment, which is the authority" --
// resolves it. Evaluated honestly, with d = 2.2 s:
//
//     t60 = 2.0 s -> 10^(-6.6)  = 2.51e-07 -> fraction 0.9999997 -> 0.0000 dB
//     t60 = 8.0 s -> 10^(-1.65) = 0.022387 -> fraction 0.977613  -> 0.0983 dB
//
// The brief's table instead printed 0.99921/0.0035 dB and 0.5477/2.62 dB. Each
// pair is self-consistent (-10*log10(0.5477) really is 2.61) but neither follows
// from 1 - 10^(-6d/t60): a captured fraction of 0.5477 needs t60 = 38.3 s, not
// 8.0. The number was back-solved from a desired dB figure and the exponential
// was never evaluated. 0.0983 dB is what t60 = 8.0 actually predicts, and
// 0.104 dB is what the fixture measures -- agreement to 0.005 dB.
//
// 0.0983 dB is small, so `slight` below is the CONTROL that keeps the test
// honest: the same room with t60 = 0.35 s rings out long before the tail ends
// (10^(-6*2.2/0.35) = 1e-38), so its truncation loss must be ~0. An estimator
// that read low for any reason OTHER than truncation would move both, and the
// control would catch it.
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
    EXPECT_NEAR (lost, 0.0983, 0.04)
        << "truncation error moved; re-derive it from the block comment above "
           "before editing this number";

    // Control: a decay that FITS inside the tail loses nothing.
    const auto fullQuick  = runRoom (x, resonantRoom (x, 1000.0, 0.35, 1.0, 12.0), 1.0e-4f);
    const auto shortQuick = runRoom (x, resonantRoom (x, 1000.0, 0.35, 1.0,  0.7), 1.0e-4f);

    ASSERT_TRUE (fullQuick.measured);
    ASSERT_TRUE (shortQuick.measured);

    const double lostQuick = fullQuick.hDb[(std::size_t) k] - shortQuick.hDb[(std::size_t) k];
    EXPECT_NEAR (lostQuick, 0.0, 0.02)
        << "a decay that fits inside the 0.7 s tail must not be under-read; "
           "if this moves, `lost` above is measuring something other than truncation";
}

// RED IF: Nbar is added instead of subtracted, or is not scaled by framesY.
//
// THE NOISE LEVEL AND THE TOLERANCE ARE BOTH LOAD-BEARING, and the brief's
// sigma = 3.0e-4 with a 1.5 dB tolerance was neither. Measured: at 3.0e-4 the
// band SNR is 46 dB, so Nbar*framesY is 2.4e-5 of EY and flipping the sign of a
// term that small moves the answer by 0.0003 dB. The mutation this test names in
// its own RED IF -- `eY + noiseInY` for `eY - noiseInY` -- passed all eight tests
// unchanged. A test that cannot go red for its stated reason is not a test.
//
// sigma = 1.2e-2 puts the band SNR at 14.4 dB: high enough that `measured` still
// holds (kMinBandSnrDb is 12, so there is 2.4 dB of headroom and the test does
// not sit on the gate), low enough that the noise term is a real fraction of EY.
// Measured mid-band answers at that sigma, quiet reading -6.0207 dB:
//
//     subtracted (correct)      -6.0080  ->  delta 0.013 dB
//     added instead             -5.5704  ->  delta 0.450 dB
//     not subtracted at all     -5.7819  ->  delta 0.239 dB   (also what dropping
//                                                              *framesY degrades to)
//
// A 0.15 dB tolerance passes the correct code with 12x margin and fails both
// mutations. Everything here is deterministic -- fixed mt19937 seeds, fixed FFT
// -- so there is no flakiness budget to spend on a looser number.
TEST (LoopGainEstimator, NoiseFloorIsSubtracted)
{
    const auto x = renderSweep();
    const auto y = flatRoom (x, 15.0, 0.5, 0.7);

    const auto quiet = runRoom (x, y, 1.0e-5f);
    const auto noisy = runRoom (x, y, 1.2e-2f);

    ASSERT_TRUE (quiet.measured);
    ASSERT_TRUE (noisy.measured) << "band SNR " << noisy.bandSnrDb
                                 << " dB must stay above kMinBandSnrDb";

    // Same room, 1200x the noise: after subtraction the mid-band answer must
    // still be the room's, not the noise's.
    EXPECT_NEAR (meanMidBandDb (noisy), meanMidBandDb (quiet), 0.15);
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
