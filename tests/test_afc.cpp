// Lane A1 (spec docs/superpowers/specs/2026-10-19-afc-design.md): the
// canceller and its supervisor, headless.

#include <gtest/gtest.h>

#include "AfcCanceller.h"
#include "AfcSupervisor.h"

#include <cmath>
#include <random>
#include <vector>

namespace
{
std::vector<float> syntheticRoom (int len, int delay, unsigned seed)
{
    std::mt19937 rng (seed);
    std::normal_distribution<double> g (0.0, 1.0);
    std::vector<float> h ((size_t) len, 0.0f);
    double e = 0;
    for (int i = delay; i < len; ++i)
    {
        h[(size_t) i] = (float) (g (rng) * std::exp (-6.9 * (i - delay) / (0.01 * 48000.0)));
        e += (double) h[(size_t) i] * h[(size_t) i];
    }
    for (auto& x : h) x = (float) (x / std::sqrt (e));
    return h;
}

double misalignmentDb (const AfcCanceller& afc, const std::vector<float>& h)
{
    std::vector<float> w;
    afc.copyImpulseResponse (w);
    double num = 0, den = 0;
    for (int k = 0; k < AfcCanceller::L; ++k)
    {
        const int tap = AfcCanceller::P + k;
        const double ht = tap < (int) h.size() ? h[(size_t) tap] : 0.0;
        num += (w[(size_t) k] - ht) * (w[(size_t) k] - ht);
        den += ht * ht;
    }
    return 10.0 * std::log10 (num / den);
}

// Open loop: u is white noise the canceller did not produce, y = h * u.
void runOpenLoop (AfcCanceller& afc, const std::vector<float>& h, int samples, unsigned seed,
                  std::vector<float>* eOut = nullptr)
{
    std::mt19937 rng (seed);
    std::normal_distribution<double> g (0.0, 0.1);
    const int hl = (int) h.size();
    std::vector<float> u ((size_t) (samples + hl), 0.0f);
    for (int i = 0; i < samples; ++i)
    {
        double y = 0;
        for (int k = 0; k < hl; ++k)
            if (h[(size_t) k] != 0.0f)
                y += h[(size_t) k] * u[(size_t) (hl + i - k)];
        const float e = afc.processSample ((float) y);
        if (eOut) eOut->push_back (e);
        const float ui = (float) g (rng);
        u[(size_t) (hl + i)] = ui;
        afc.pushReference (ui);
    }
}
}

TEST (AfcCanceller, JuceInverseFftIsScaledByOneOverN)
{
    // inverse() relies on it; an FFT engine that does not scale would make
    // every filter K*N times too strong.
    juce::dsp::FFT fft (7);
    std::vector<float> d (256, 0.0f);
    d[3] = 1.0f;
    fft.performRealOnlyForwardTransform (d.data(), true);
    fft.performRealOnlyInverseTransform (d.data());
    EXPECT_NEAR (d[3], 1.0f, 1e-5f);
    EXPECT_NEAR (d[4], 0.0f, 1e-5f);
}

TEST (AfcCanceller, IdentifiesAnOpenLoopPathDeeply)
{
    AfcCanceller afc;
    AfcCanceller::Params p;
    p.mu = 0.5f;
    p.pem = false;
    afc.setParams (p);
    const auto h = syntheticRoom (300 + 480, 300, 1);
    runOpenLoop (afc, h, 48000 * 4, 7);
    EXPECT_LT (misalignmentDb (afc, h), -30.0);
}

TEST (AfcCanceller, IdentifiesWithPemOn)
{
    AfcCanceller afc;
    AfcCanceller::Params p;
    p.mu = 0.5f;
    afc.setParams (p);
    const auto h = syntheticRoom (300 + 480, 300, 2);
    runOpenLoop (afc, h, 48000 * 8, 8);
    // Open loop the PEM model is estimated on e = h * u, not on u, so u is not
    // exactly whitened: a floor near -18 dB is the method, not a defect.
    EXPECT_LT (misalignmentDb (afc, h), -15.0);
}

TEST (AfcCanceller, LagsBelowPAreLargelyNotModelled)
{
    // A path with ALL its energy at lag 10 (< P) is outside the model. The
    // rotating gradient constraint lets a little leak through circular wrap
    // in the partitions not yet constrained (spec 3.3), so "zero" is not the
    // bar: the learned energy must stay far below the path's (0.25).
    AfcCanceller afc;
    AfcCanceller::Params p; p.mu = 0.5f; p.pem = false; afc.setParams (p);
    std::vector<float> h (11, 0.0f);
    h[10] = 0.5f;
    runOpenLoop (afc, h, 48000, 9);
    EXPECT_LT (afc.coefficientEnergy(), 0.25 * 0.2);   // >= 7 dB below
}

TEST (AfcCanceller, ZeroFilterPassesInputThroughUnchanged)
{
    AfcCanceller afc;
    for (int i = 0; i < 1000; ++i)
    {
        const float y = std::sin (0.01f * (float) i);
        EXPECT_EQ (afc.processSample (y), y);
        afc.pushReference (0.0f);   // nothing ever leaves: nothing to learn
    }
}

TEST (AfcCanceller, NonFiniteInputTripsAndReturnsYOnTheSameSample)
{
    AfcCanceller afc;
    const float e = afc.processSample (std::numeric_limits<float>::quiet_NaN());
    EXPECT_TRUE (std::isfinite (e));
    EXPECT_EQ (afc.takeTrip(), AfcCanceller::Trip::NonFinite);
    EXPECT_EQ (afc.takeTrip(), AfcCanceller::Trip::None);
}

TEST (AfcCanceller, NonFiniteReferenceIsTreatedAsSilence)
{
    AfcCanceller afc;
    for (int i = 0; i < 4 * AfcCanceller::P; ++i)
    {
        afc.processSample (0.1f);
        afc.pushReference (std::numeric_limits<float>::infinity());
    }
    EXPECT_TRUE (std::isfinite (afc.coefficientEnergy()));
}

TEST (AfcCanceller, ResultDoesNotDependOnHowTheStreamIsChunked)
{
    // Spec invariant 3: the partition counter lives in the object. Feeding the
    // same samples one at a time or in any grouping gives identical output --
    // there is no "block" API, so this pins that two instances fed the same
    // sequence agree bit for bit, with learning on.
    const auto h = syntheticRoom (300 + 480, 300, 3);
    AfcCanceller a, b;
    std::vector<float> ea, eb;
    runOpenLoop (a, h, 20000, 11, &ea);
    runOpenLoop (b, h, 20000, 11, &eb);
    ASSERT_EQ (ea.size(), eb.size());
    for (size_t i = 0; i < ea.size(); ++i)
        ASSERT_EQ (ea[i], eb[i]) << i;
}

TEST (AfcCanceller, SilentReferenceTeachesNothing)
{
    // I3: a reference with no energy (mute, a deep notch everywhere) must not
    // make W drift from its own rounding.
    AfcCanceller afc;
    for (int i = 0; i < 48000; ++i)
    {
        afc.processSample (0.05f * std::sin (0.02f * (float) i));
        afc.pushReference (0.0f);
    }
    EXPECT_EQ (afc.coefficientEnergy(), 0.0);
}

TEST (AfcCanceller, ResetForgetsTheFilter)
{
    AfcCanceller afc;
    AfcCanceller::Params p; p.mu = 0.5f; p.pem = false; afc.setParams (p);
    const auto h = syntheticRoom (300 + 480, 300, 4);
    runOpenLoop (afc, h, 24000, 12);
    ASSERT_GT (afc.coefficientEnergy(), 0.1);
    afc.reset();
    EXPECT_EQ (afc.coefficientEnergy(), 0.0);
}

TEST (AfcCanceller, CoefficientBlowUpTrips)
{
    // A "path" with gain 3 is not a room: ||F^|| > 2 must trip.
    AfcCanceller afc;
    AfcCanceller::Params p; p.mu = 0.5f; p.pem = false; afc.setParams (p);
    std::vector<float> h (301, 0.0f);
    h[300] = 3.0f;
    bool tripped = false;
    std::mt19937 rng (5);
    std::normal_distribution<double> g (0.0, 0.1);
    std::vector<float> u (48000 + 301, 0.0f);
    for (int i = 0; i < 48000 && ! tripped; ++i)
    {
        afc.processSample (3.0f * u[(size_t) (301 + i - 300)]);
        u[(size_t) (301 + i)] = (float) g (rng);
        afc.pushReference (u[(size_t) (301 + i)]);
        tripped = afc.takeTrip() == AfcCanceller::Trip::Coefficients;
    }
    EXPECT_TRUE (tripped);
}

//==============================================================================

namespace
{
using Kind = AfcSupervisor::TripKind;

struct Sup
{
    AfcSupervisor s;
    Sup() { s.prepare (48000.0); }
    AfcSupervisor::Step run (int n, bool en, bool frozen = false, Kind trip = Kind::None)
    {
        AfcSupervisor::Step last;
        for (int i = 0; i < n; ++i)
            last = s.tick (en, frozen, i == 0 ? trip : Kind::None, false);
        return last;
    }
};
constexpr int kFade = 480;   // 10 ms @ 48 kHz
}

TEST (AfcSupervisor, OffIsSilentAndUnducked)
{
    Sup t;
    const auto s = t.run (1000, false);
    EXPECT_EQ (s.mix, 0.0f);
    EXPECT_EQ (s.duck, 1.0f);
    EXPECT_FALSE (s.runCanceller);
    EXPECT_EQ (t.s.status(), AfcSupervisor::Status::Off);
}

TEST (AfcSupervisor, SwitchingOnFadesInFromAFreshFilter)
{
    Sup t;
    const auto first = t.s.tick (true, false, Kind::None, false);
    EXPECT_TRUE (first.resetCanceller);
    EXPECT_GT (first.mix, 0.0f);
    EXPECT_LT (first.mix, 0.01f);
    const auto s = t.run (kFade, true);
    EXPECT_EQ (s.mix, 1.0f);
    EXPECT_EQ (t.s.status(), AfcSupervisor::Status::Learning);
    t.run (48000 * 21, true);
    EXPECT_EQ (t.s.status(), AfcSupervisor::Status::Running);
}

TEST (AfcSupervisor, SwitchingOffFadesOutAndDucksSixDb)
{
    Sup t;
    t.run (48000, true);
    auto s = t.run (kFade / 2, false);
    EXPECT_GT (s.mix, 0.0f);                     // still fading: no step
    s = t.run (kFade, false);
    EXPECT_EQ (s.mix, 0.0f);
    EXPECT_NEAR (20.0 * std::log10 (s.duck), -6.0, 0.05);
    s = t.run (48000 * 5, false);                // inside hold + release
    EXPECT_GT (s.duck, 0.5f);
    s = t.run (48000 * 6, false);
    EXPECT_EQ (s.duck, 1.0f);
}

TEST (AfcSupervisor, InflationTripCrossfadesDucksAndRests)
{
    Sup t;
    t.run (48000, true);
    auto s = t.s.tick (true, false, Kind::Inflation, false);
    EXPECT_GT (s.mix, 0.9f);                     // fading, not cut
    EXPECT_EQ (t.s.status(), AfcSupervisor::Status::Resting);
    s = t.run (kFade, true);
    EXPECT_EQ (s.mix, 0.0f);
    EXPECT_NEAR (20.0 * std::log10 (s.duck), -6.0, 0.05);
    t.run (48000 * 5, true);
    EXPECT_EQ (t.s.status(), AfcSupervisor::Status::Learning);   // back, from zero
}

TEST (AfcSupervisor, NonFiniteTripCutsTheMixOnTheSameSample)
{
    Sup t;
    t.run (48000, true);
    const auto s = t.s.tick (true, false, Kind::NonFinite, false);
    EXPECT_EQ (s.mix, 0.0f);
}

TEST (AfcSupervisor, ThreeTripsInAMinuteLockUntilTheSwitchIsCycled)
{
    Sup t;
    for (int i = 0; i < 3; ++i)
    {
        t.run (48000 * 6, true);                 // learning again after the rest
        t.s.tick (true, false, Kind::Inflation, false);
    }
    EXPECT_EQ (t.s.status(), AfcSupervisor::Status::Locked);
    t.run (48000 * 30, true);
    EXPECT_EQ (t.s.status(), AfcSupervisor::Status::Locked);
    t.run (10, false);
    t.run (10, true);
    EXPECT_EQ (t.s.status(), AfcSupervisor::Status::Learning);
}

TEST (AfcSupervisor, TwoNotchesInTenSecondsAreASoftTrip)
{
    Sup t;
    t.run (48000, true);
    t.s.tick (true, false, Kind::None, true);
    t.run (48000 * 3, true);
    EXPECT_EQ (t.s.status(), AfcSupervisor::Status::Learning);
    t.s.tick (true, false, Kind::None, true);
    EXPECT_EQ (t.s.status(), AfcSupervisor::Status::Resting);
    EXPECT_EQ (t.s.lastTrip(), Kind::Soft);
}

TEST (AfcSupervisor, LaneMFreezeFadesOutWithoutADuckAndRestartsFresh)
{
    Sup t;
    t.run (48000, true);
    auto s = t.run (kFade + 1, true, true);
    EXPECT_EQ (s.mix, 0.0f);
    EXPECT_EQ (s.duck, 1.0f);
    EXPECT_EQ (t.s.status(), AfcSupervisor::Status::Frozen);
    // A trip reported during the freeze does not count.
    t.s.tick (true, true, Kind::Inflation, false);
    EXPECT_EQ (t.s.tripCount(), 0u);
    s = t.s.tick (true, false, Kind::None, false);
    EXPECT_TRUE (s.resetCanceller);
    EXPECT_EQ (t.s.status(), AfcSupervisor::Status::Learning);
}

TEST (AfcCanceller, PemModelWhitensAKnownArProcess)
{
    // Spec 3.4. Feed a strongly coloured AR(2) process as y with no reference:
    // the model estimated on e (= y here) must flatten it. Measured as the
    // prediction gain: residual power well below input power.
    AfcCanceller afc;
    std::mt19937 rng (21);
    std::normal_distribution<double> g (0.0, 0.01);
    const double r = 0.995, th = 2.0 * juce::MathConstants<double>::pi * 1000.0 / 48000.0;
    const double a1 = -2.0 * r * std::cos (th), a2 = r * r;
    double z1 = 0, z2 = 0;
    std::vector<float> x;
    for (int i = 0; i < 48000; ++i)
    {
        const double v = g (rng) - a1 * z1 - a2 * z2;
        z2 = z1; z1 = v;
        x.push_back ((float) v);
        afc.processSample ((float) v);
        afc.pushReference (0.0f);
    }

    const auto& A = afc.pemModel();
    ASSERT_NE (A[1], 0.0f) << "PEM model never updated";

    double in = 0, res = 0;
    for (size_t n = AfcCanceller::kPemOrder; n < x.size(); ++n)
    {
        double acc = 0;
        for (int i = 0; i <= AfcCanceller::kPemOrder; ++i)
            acc += A[(size_t) i] * x[n - (size_t) i];
        in += (double) x[n] * x[n];
        res += acc * acc;
    }
    EXPECT_LT (10.0 * std::log10 (res / in), -20.0);
}
