// tests/test_loopgainestimator.cpp
//
// A synthetic room: delay + gain, optionally one resonance. The estimator sees
// only sample buffers, so everything here is arithmetic.
#include <gtest/gtest.h>

#include "dsp/LoopGainEstimator.h"
#include "dsp/SoundcheckSignal.h"

#include <algorithm>   // std::max, for the Box-Muller floor in noise()
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

// BOX-MULLER, NOT std::normal_distribution (final review M-1). mt19937's
// sequence is standardised; normal_distribution's mapping of it is not, so the
// same seed draws a different noise floor under libc++ than under MSVC and the
// SNR tolerances below were only ever measured on one of them. Same statistics
// (mean 0, s.d. sigma, independent), same seed, same numbers everywhere.
std::vector<float> noise (std::size_t n, float sigma, unsigned seed)
{
    constexpr float kTwoPi = 6.28318530717958647692f;

    std::mt19937 rng { seed };
    std::uniform_real_distribution<float> u { 0.0f, 1.0f };
    std::vector<float> out (n);

    for (std::size_t i = 0; i < n; i += 2)
    {
        // Floored off zero: log(0) is -inf, and a single inf here would reach
        // every bin of the estimator's noise-floor spectrum.
        const float u1  = std::max (u (rng), 1.0e-12f);
        const float mag = sigma * std::sqrt (-2.0f * std::log (u1));
        const float th  = kTwoPi * u (rng);

        out[i] = mag * std::cos (th);
        if (i + 1 < n)
            out[i + 1] = mag * std::sin (th);
    }

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

// Mean hDb over [7.4 kHz, 10 kHz] -- the TOP of the swept range, above
// kTrustedHighHz, so these bins are DRAWN but never trusted. This is the band a
// delay pushes off the end of the capture window: a 3 s log sweep from 100 Hz
// reaches 7.35 kHz at 3.0 * ln(73.5)/ln(100) = 2.80 s, so the last 0.2 s of the
// sweep IS this band -- and it is exactly the 0.2 s that a 900 ms delay loses
// from a 3.7 s buffer.
double meanHighBandDb (const LoopGainEstimator::Result& r)
{
    const int lo = LoopGainEstimator::hzToBin ( 7400.0, kSr);
    const int hi = LoopGainEstimator::hzToBin (10000.0, kSr);
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
// genuinely pushes the last part of the sweep past the capture window -- but
// WHICH PART matters, and rev 2 of this comment did not say. `flatRoom` renders
// sweep + 0.7 s = 3.7 s; delayed by 0.9 s the sweep ends at 3.9 s, so the 0.2 s
// that falls off the end is the sweep's LAST 0.2 s, which is 7.35-10 kHz. None
// of it is inside meanMidBandDb's [200 Hz, 4 kHz]. So asserting only on the
// mid-band with a 3.0 dB tolerance was decorative: the same F18/I-11 defect
// surviving one layer further down again.
//
// The test therefore reads BOTH bands, and they carry opposite obligations:
//
//   mid  [200 Hz, 4 kHz]  nothing was truncated here, so the delay must change
//                         NOTHING. Measured: -6.0208 quick, -6.0205 slow -- a
//                         swing of 0.0003 dB. The tolerance is 0.5 dB against
//                         the true -6.02, which a method that tracked delay
//                         could not meet.
//   high [7.4, 10 kHz]    everything WAS truncated here, so the delayed case
//                         must read materially lower. Measured: -4.42 dB quick,
//                         -134.84 dB slow -- a drop of 130.4 dB, because the
//                         delayed sweep put NONE of this band inside the capture
//                         window and all that survives the noise subtraction is
//                         the eps floor.
//
// Why the quick case reads -4.42 up here rather than the room's -6.02: the
// reference stream is 3.0 s and the capture stream is 3.7 s, and a frame is
// emitted only from a FULL 2048-sample window, so the reference's frame grid
// stops 128 samples short of the end of the sweep while the capture's does not.
// Those last 128 samples are ~10 kHz, so EY holds a sliver of top-octave energy
// EX never counted and the ratio reads high. That is a property of this
// FIXTURE's stream lengths, not of the room and not of the estimator; it is
// confined to the top of the sweep (the mid band is unaffected, see above), and
// it is asserted rather than ignored so it cannot drift unnoticed.
TEST (LoopGainEstimator, DelayDoesNotChangeTheAnswer)
{
    const auto x = renderSweep();

    const auto quick = runRoom (x, flatRoom (x,   5.0, 0.5, 0.7), 1.0e-4f);
    const auto slow  = runRoom (x, flatRoom (x, 900.0, 0.5, 0.7), 1.0e-4f);

    ASSERT_TRUE (quick.measured);
    ASSERT_TRUE (slow.measured);

    // Untouched band: the per-bin energy ratio must not TRACK the delay.
    EXPECT_NEAR (meanMidBandDb (quick), -6.02, 0.5);
    EXPECT_NEAR (meanMidBandDb (slow),  -6.02, 0.5);

    // Truncated band: the energy really is gone, and the estimator must show it
    // rather than quietly reconstruct it. 130.4 dB measured, so 40 dB is a
    // threshold with room to spare that still cannot be met by accident.
    EXPECT_NEAR (meanHighBandDb (quick), -4.42, 1.0)
        << "the reference/capture stream-length asymmetry described above has "
           "moved; re-measure before changing this number";
    EXPECT_LT (meanHighBandDb (slow), meanHighBandDb (quick) - 40.0)
        << "the sweep's top 0.2 s fell outside the capture window; an estimator "
           "that still reads a room-like level up here is reconstructing energy "
           "it never received"
        << " (quick=" << meanHighBandDb (quick) << " slow=" << meanHighBandDb (slow) << ")";
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
// 1 - 10^(-6 * 2.2 / t60), and the shortfall is -10*log10 of it. Evaluated with
// d = 2.2 s, this is THE authoritative table:
//
//     t60 = 2.0 s -> 10^(-6.6)  = 2.51e-07 -> fraction 0.9999997 -> 0.0000 dB
//     t60 = 8.0 s -> 10^(-1.65) = 0.022387 -> fraction 0.977613  -> 0.0983 dB
//
// Plan rev 1 shipped t60 = 2.0 expecting 3.0 dB, and the brief that replaced it
// shipped t60 = 8.0 expecting 2.62 dB. BOTH are misevaluations of the formula
// printed directly above them -- the brief's own pairs, 0.99921/0.0035 dB and
// 0.5477/2.62 dB, are WRONG and are not reproduced here as authority. Each pair
// is internally consistent (-10*log10(0.5477) really is 2.61) but neither
// follows from 1 - 10^(-6d/t60): a captured fraction of 0.5477 needs
// t60 = 38.3 s, not 8.0. The dB figure was chosen first and the fraction
// back-solved from it; the exponential was never evaluated. The brief's own rule
// -- "re-derive it from the block comment, which is the authority" -- is what
// resolves it, and 0.0983 dB is what t60 = 8.0 actually predicts, against
// 0.104 dB measured: agreement to 0.005 dB.
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
//
// RE-MEASURE THIS TEST IF ANY OF THESE MOVE, because each one shifts the 14.4 dB
// band SNR that the sigma above was chosen to produce, and a drifting band SNR
// silently returns the test to the decorative state it started in:
//   - SoundcheckSignal::kSoundcheckMaxPeak (the sweep's level, hence EY)
//   - SoundcheckSignal::kSweepSeconds / kSweepLowHz / kSweepHighHz (energy per bin)
//   - Detector::kFftSize / kHopSize (frame count, hence the noise sum)
//   - LoopGainEstimator::kMinBandSnrDb (the gate the 14.4 dB sits above)
// The bandSnrDb assertion below is there so such a drift fails LOUDLY here
// rather than by this test quietly passing for the wrong reason.
TEST (LoopGainEstimator, NoiseFloorIsSubtracted)
{
    const auto x = renderSweep();
    const auto y = flatRoom (x, 15.0, 0.5, 0.7);

    const auto quiet = runRoom (x, y, 1.0e-5f);
    const auto noisy = runRoom (x, y, 1.2e-2f);

    ASSERT_TRUE (quiet.measured);
    ASSERT_TRUE (noisy.measured) << "band SNR " << noisy.bandSnrDb
                                 << " dB must stay above kMinBandSnrDb";

    // The fixture's own calibration, not a property of the estimator: 14.4 dB
    // measured, and below ~13 dB the noise term stops being small enough for the
    // 0.15 dB tolerance to discriminate the sign mutations. See the list above.
    EXPECT_GT (noisy.bandSnrDb, 13.0)
        << "the noise level this test depends on has drifted; re-measure the "
           "table in the comment above before trusting this test again";

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

// RED IF: the band gate and the bin gate are applied independently, so a run the
// estimator has ALREADY declared unusable can still hand Task 6 a bin to notch.
//
// The two gates answer different questions -- the band gate asks whether the
// measurement happened at all, the bin gate only ranks bins inside a measurement
// that did -- so `measured` must OVERRIDE `trusted`, not sit beside it.
//
// The fixture separates them on purpose: a room whose BAND SNR is under
// kMinBandSnrDb (12 dB) but which carries a resonance whose own bins are well
// above kMinBinSnrDb (6 dB) inside the trusted band. Checking `trustedCount == 0`
// in a room where every bin is quiet (BinBelowSnrIsMarkedUntrusted) cannot tell
// the two gates apart, because there the bin gate alone already closes every bin.
//
// CALIBRATION, measured with the band gate temporarily removed from finish() so
// the bin gate's verdict could be read on its own (broadband gain 0.001, noise
// sigma 3.0e-3, resonance at 1 kHz):
//
//     resonance gain 1.0  ->  bandSnr  7.10 dB  measured 0  bins passing bin gate  9
//     resonance gain 1.4  ->  bandSnr  9.57 dB  measured 0  bins passing bin gate 14   <-- used
//     resonance gain 1.8  ->  bandSnr 11.55 dB  measured 0  bins passing bin gate 14
//     resonance gain 2.0  ->  bandSnr 12.41 dB  measured 1  bins passing bin gate 15
//
// 1.4 sits 2.4 dB below the band gate -- clear of it, unlike 1.8 -- while still
// leaving 14 bins that the bin gate alone would have trusted. Those 14 bins are
// exactly what this test requires finish() to throw away.
TEST (LoopGainEstimator, NotMeasuredClearsEveryTrustedBin)
{
    const auto x = renderSweep();

    // Broadband room 60 dB down so the BAND drowns, plus one loud 1 kHz
    // resonance so a handful of BINS do not.
    auto y = flatRoom (x, 15.0, 0.001, 0.7);
    const auto res = resonantRoom (x, 1000.0, 0.35, 1.4, 0.7);
    for (std::size_t i = 0; i < y.size() && i < res.size(); ++i)
        y[i] += res[i];

    const auto r = runRoom (x, y, 3.0e-3f);

    // The fixture must actually straddle the two thresholds, or it proves
    // nothing -- so assert the SETUP, not just the outcome.
    ASSERT_LT (r.bandSnrDb, LoopGainEstimator::kMinBandSnrDb)
        << "fixture no longer has a failing BAND SNR: " << r.bandSnrDb;
    ASSERT_FALSE (r.measured);

    // ... and the resonance must still be loud enough that the bin gate WOULD
    // have passed it. hDb at 1 kHz reads -3.8 dB here against roughly -60 dB for
    // the broadband floor around it, so the resonance is unambiguously present
    // and this is not silently the all-quiet fixture again.
    const int k1k = LoopGainEstimator::hzToBin (1000.0, kSr);
    ASSERT_GT (r.hDb[(std::size_t) k1k], -20.0f)
        << "the 1 kHz resonance has gone quiet; this fixture no longer separates "
           "the band gate from the bin gate";

    int trustedCount = 0;
    for (int k = 0; k < LoopGainEstimator::kNumBins; ++k)
        if (r.trusted[(std::size_t) k]) ++trustedCount;

    EXPECT_EQ (trustedCount, 0)
        << "a run with measured == false must expose no trusted bin at all; "
           "14 bins around bin " << k1k << " (1 kHz) survive the bin gate and "
           "must be cleared by the band gate";
}

// RED IF: reset() forgets a stream or an accumulator. Task 6 re-runs a soundcheck
// on the same instance, and a stale accumulator there reads as a hot room -- a
// notch proposed from a measurement that is half the previous run.
TEST (LoopGainEstimator, ResetClearsEveryStream)
{
    const auto x = renderSweep();
    const auto y = flatRoom (x, 15.0, 0.5, 0.7);

    LoopGainEstimator est { kSr };
    const auto floorNoise = noise ((std::size_t) (0.5 * kSr), 1.0e-4f, 7);
    est.pushNoiseFloor (floorNoise.data(), (int) floorNoise.size());
    est.pushReference  (x.data(), (int) x.size());
    est.pushCapture    (y.data(), (int) y.size());

    const auto before = est.finish();
    ASSERT_TRUE (before.measured);
    ASSERT_GT (before.noiseFrames, 0);
    ASSERT_GT (before.referenceFrames, 0);
    ASSERT_GT (before.captureFrames, 0);

    est.reset (kSr);
    const auto after = est.finish();

    EXPECT_EQ (after.noiseFrames, 0);
    EXPECT_EQ (after.referenceFrames, 0);
    EXPECT_EQ (after.captureFrames, 0);
    EXPECT_FALSE (after.measured);

    // Not just the counters: the ACCUMULATORS. With every bin back to EY = EX = 0
    // the ratio is eps/eps, so hDb is exactly 0.0 -- and no bin may be trusted,
    // which is also the silent-bin contract on Result (0.0 dB is not a howl).
    for (int k = 0; k < LoopGainEstimator::kNumBins; ++k)
    {
        ASSERT_FLOAT_EQ (after.hDb[(std::size_t) k], 0.0f) << "bin " << k;
        ASSERT_FALSE (after.trusted[(std::size_t) k]) << "bin " << k;
    }

    // And a re-run on the reset instance must reproduce the original answer
    // rather than a doubled one.
    est.pushNoiseFloor (floorNoise.data(), (int) floorNoise.size());
    est.pushReference  (x.data(), (int) x.size());
    est.pushCapture    (y.data(), (int) y.size());

    const auto again = est.finish();
    ASSERT_TRUE (again.measured);
    EXPECT_EQ (again.captureFrames, before.captureFrames);
    EXPECT_NEAR (meanMidBandDb (again), meanMidBandDb (before), 1.0e-4);
}
