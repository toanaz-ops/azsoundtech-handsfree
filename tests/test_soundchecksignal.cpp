// tests/test_soundchecksignal.cpp
//
// SoundcheckSignal is the ONLY thing between "the app decided to measure" and a
// loudspeaker. Every test here is a safety test; none of them needs a device,
// a thread or a clock.
#include <gtest/gtest.h>

#include "dsp/SoundcheckSignal.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace
{
constexpr double kSr = 48000.0;

SoundcheckSignal::Params defaultParams (float peak = SoundcheckSignal::kSoundcheckMaxPeak)
{
    SoundcheckSignal::Params p;
    p.sampleRate = kSr;
    p.peak       = peak;
    return p;
}

// Zero crossings of the sweep over a short span, converted back to a frequency.
// Counting crossings is deliberately independent of instantaneousHz(): a test
// that asked the class for the answer it is checking proves nothing.
double measuredHzAround (const SoundcheckSignal& s, std::int64_t centre, std::int64_t span)
{
    int crossings = 0;
    float prev = s.sampleAt (centre - span / 2);
    for (std::int64_t n = centre - span / 2 + 1; n < centre + span / 2; ++n)
    {
        const float cur = s.sampleAt (n);
        if ((prev < 0.0f && cur >= 0.0f) || (prev >= 0.0f && cur < 0.0f))
            ++crossings;
        prev = cur;
    }
    return (crossings / 2.0) * kSr / (double) span;
}
} // namespace

// RED IF: clampPeak() stops clamping, or the clamp is applied only at the
// setter and not at the point of use (spec §3 safety table: "a value with two
// routes to becoming wrong needs BOTH clamped -- one clamp is half a clamp",
// lane G lesson M-B). inv 1.
TEST (SoundcheckSignal, PeakIsClampedWhateverIsAsked)
{
    for (float asked : { 1.0f, 10.0f, -5.0f, std::numeric_limits<float>::quiet_NaN(),
                         std::numeric_limits<float>::infinity() })
    {
        const SoundcheckSignal s { defaultParams (asked) };

        EXPECT_LE (s.clampedPeak(), SoundcheckSignal::kSoundcheckMaxPeak) << "asked " << asked;
        EXPECT_GE (s.clampedPeak(), 0.0f) << "asked " << asked;

        for (std::int64_t n = 0; n < s.totalSamples(); n += 97)
            ASSERT_LE (std::abs (s.sampleAt (n)), SoundcheckSignal::kSoundcheckMaxPeak)
                << "asked " << asked << " at n=" << n;
    }
}

// RED IF: the raised-cosine window is dropped, or it is applied as w(n)+something
// that does not vanish at the ends. A non-zero first sample IS a click into a PA.
// inv 2.
TEST (SoundcheckSignal, FirstAndLastSampleAreExactlyZero)
{
    const SoundcheckSignal s { defaultParams() };

    EXPECT_FLOAT_EQ (s.sampleAt (0), 0.0f);
    EXPECT_FLOAT_EQ (s.sampleAt (s.totalSamples() - 1), 0.0f);
}

// RED IF: the noise-floor phase is reintroduced as a separate "emitting" flag
// instead of a negative sample index. inv 2 / inv 6.
TEST (SoundcheckSignal, NegativeIndexReturnsZero)
{
    const SoundcheckSignal s { defaultParams() };

    for (std::int64_t n : { -1LL, -100LL, -24000LL, -(std::int64_t) (0.5 * kSr) })
        EXPECT_FLOAT_EQ (s.sampleAt (n), 0.0f) << "n=" << n;

    EXPECT_FLOAT_EQ (s.sampleAt (s.totalSamples()), 0.0f);
    EXPECT_FLOAT_EQ (s.sampleAt (s.totalSamples() + 4096), 0.0f);
}

// RED IF: the exponential phase term is written with the wrong sign or the wrong
// K, which produces a sweep that runs backwards or stops short of 10 kHz.
//
// B-8: an earlier draft asserted 100 Hz +- 5 and 10000 Hz +- 500 at the probe
// CENTRES, and the sweep cannot meet that. f(n) = 100 * 100^(n/T) with
// T = 3.0 * 48000 = 144000, so a probe centred at 2400 samples is already at
// 100 * 100^(2400/144000) = 107.98 Hz, and one centred near the end reads about
// 9263 Hz. Both are CORRECT for a log sweep; the tolerances were wrong.
//
// So the test compares each measurement against the closed form evaluated at
// THAT SAME centre, which pins the shape without pretending the endpoints are
// reachable by a finite probe. The endpoints themselves are pinned by
// instantaneousHz(0) and instantaneousHz(total), where no window is needed.
TEST (SoundcheckSignal, InstantaneousFrequencyIsMonotoneAndHitsBothEnds)
{
    const SoundcheckSignal s { defaultParams() };
    const std::int64_t total = s.totalSamples();

    // The closed form, written out here rather than asked of the class, so the
    // two are independent.
    auto closedForm = [total] (std::int64_t n)
    {
        return 100.0 * std::pow (100.0, (double) n / (double) total);
    };

    // 20 ms holds ~2 cycles at 100 Hz; 4 ms holds ~37 at 9 kHz.
    const std::int64_t spanA = (std::int64_t) (0.020 * kSr);
    const std::int64_t spanB = (std::int64_t) (0.004 * kSr);
    const std::int64_t cA    = spanA / 2 + 1;
    const std::int64_t cB    = total - spanB / 2 - 1;

    const double atStart = measuredHzAround (s, cA, spanA);
    const double atEnd   = measuredHzAround (s, cB, spanB);

    EXPECT_NEAR (atStart, closedForm (cA), closedForm (cA) * 0.05);
    EXPECT_NEAR (atEnd,   closedForm (cB), closedForm (cB) * 0.05);

    // The ends themselves, where no probe window is involved.
    EXPECT_NEAR (s.instantaneousHz (0),     100.0,   0.01);
    EXPECT_NEAR (s.instantaneousHz (total), 10000.0, 1.0);

    double previous = 0.0;
    for (std::int64_t n = 0; n < total; n += total / 64)
    {
        const double hz = s.instantaneousHz (n);
        ASSERT_GT (hz, previous) << "not monotone at n=" << n;
        previous = hz;
    }
}

// RED IF: the fade-in is made linear or shortened. The ramp must rise from 0 to
// the full peak envelope over exactly kRampMs and never overshoot.
TEST (SoundcheckSignal, RampIsMonotoneOverThirtyMilliseconds)
{
    const SoundcheckSignal s { defaultParams() };
    const std::int64_t ramp = s.rampSamples();

    EXPECT_EQ (ramp, (std::int64_t) (SoundcheckSignal::kRampMs * kSr / 1000.0));

    // The envelope is not directly observable, so probe it by the running peak
    // over one period of the (100 Hz, 480-sample) tone at each point.
    auto envelopeNear = [&s] (std::int64_t centre)
    {
        float m = 0.0f;
        for (std::int64_t n = centre; n < centre + 480; ++n)
            m = std::max (m, std::abs (s.sampleAt (n)));
        return m;
    };

    float previous = -1.0f;
    for (std::int64_t n = 0; n + 480 < ramp; n += 480)
    {
        const float e = envelopeNear (n);
        ASSERT_GE (e, previous) << "envelope fell during the fade-in at n=" << n;
        previous = e;
    }
}

// RED IF: the ramp-out reuses the fade-in window instead of being its own
// function (F8), or its last sample is "very small" rather than EXACTLY zero.
// The callback relies on reaching exactly 0 to know the ramp is over and to
// clear scOutChannel_ itself. inv 9.
TEST (SoundcheckSignal, RampOutIsMonotoneAndReachesExactlyZero)
{
    const std::int64_t r = SoundcheckSignal::rampOutSamples (kSr);
    EXPECT_EQ (r, (std::int64_t) (SoundcheckSignal::kRampOutMs * kSr / 1000.0));

    const std::int64_t anchor = 12345;

    EXPECT_FLOAT_EQ (SoundcheckSignal::rampOut (anchor - 1, anchor, r), 1.0f);
    EXPECT_FLOAT_EQ (SoundcheckSignal::rampOut (anchor,     anchor, r), 1.0f);

    float previous = 1.0f;
    for (std::int64_t n = anchor; n < anchor + r; ++n)
    {
        const float v = SoundcheckSignal::rampOut (n, anchor, r);
        ASSERT_LE (v, previous + 1.0e-6f) << "ramp-out rose at n=" << n;
        ASSERT_GE (v, 0.0f);
        previous = v;
    }

    EXPECT_FLOAT_EQ (SoundcheckSignal::rampOut (anchor + r,       anchor, r), 0.0f);
    EXPECT_FLOAT_EQ (SoundcheckSignal::rampOut (anchor + r + 999, anchor, r), 0.0f);
}

// RED IF: the two envelopes are multiplied in the wrong order or the ramp-out is
// applied to the raw sine instead of the already-windowed sample. This is the
// composition the callback performs, asserted here once so Task 5 only has to
// wire it.
TEST (SoundcheckSignal, RampOutMultipliesTheWindowedSampleAndNeverExceedsThePeak)
{
    const SoundcheckSignal s { defaultParams() };
    const std::int64_t r      = SoundcheckSignal::rampOutSamples (kSr);
    const std::int64_t anchor = s.totalSamples() / 2;

    for (std::int64_t n = anchor; n < anchor + r + 64; ++n)
    {
        const float v = s.sampleAt (n) * SoundcheckSignal::rampOut (n, anchor, r);
        ASSERT_LE (std::abs (v), SoundcheckSignal::kSoundcheckMaxPeak);
    }

    EXPECT_FLOAT_EQ (s.sampleAt (anchor + r + 10)
                         * SoundcheckSignal::rampOut (anchor + r + 10, anchor, r), 0.0f);
}
