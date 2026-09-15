// src/dsp/SoundcheckSignal.cpp
#include "dsp/SoundcheckSignal.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr double kPi = 3.14159265358979323846;
}

float SoundcheckSignal::clampPeak (float requested) noexcept
{
    // Decided explicitly rather than by jlimit/std::clamp: both are undefined
    // for NaN inputs, and this value drives a loudspeaker.
    if (! std::isfinite (requested))
        return 0.0f;
    return std::min (std::max (requested, 0.0f), kSoundcheckMaxPeak);
}

SoundcheckSignal::SoundcheckSignal (const Params& p) noexcept
{
    sampleRate_ = (p.sampleRate > 0.0) ? p.sampleRate : 48000.0;
    lowHz_      = (p.lowHz > 0.0)      ? p.lowHz      : kSweepLowHz;

    const double highHz = (p.highHz > lowHz_) ? p.highHz : (lowHz_ * 2.0);
    const double tS     = (p.sweepSeconds > 0.0) ? p.sweepSeconds : kSweepSeconds;

    k_            = std::log (highHz / lowHz_);
    phaseScale_   = 2.0 * kPi * lowHz_ * tS / k_;
    totalSamples_ = (std::int64_t) (tS * sampleRate_);
    rampSamples_  = (std::int64_t) (std::max (0.0, p.rampMs) * sampleRate_ / 1000.0);

    // Never more than half the sweep at each end, or the two windows overlap
    // and the "last sample is exactly zero" property stops holding.
    rampSamples_ = std::min (rampSamples_, totalSamples_ / 2);

    peak_ = clampPeak (p.peak);
}

float SoundcheckSignal::sampleAt (std::int64_t n) const noexcept
{
    if (n < 0 || n >= totalSamples_)
        return 0.0f;

    const double t   = (double) n / (double) totalSamples_;
    const double phi = phaseScale_ * (std::exp (k_ * t) - 1.0);

    // Raised cosine at BOTH ends. w(0) == 0 and w(totalSamples_-1) == 0 by
    // construction: the denominator is rampSamples_, and the index that reaches
    // it is exactly the first sample of the flat middle.
    double w = 1.0;
    if (rampSamples_ > 0)
    {
        const std::int64_t fromStart = n;
        const std::int64_t fromEnd   = totalSamples_ - 1 - n;
        const std::int64_t edge      = std::min (fromStart, fromEnd);

        if (edge < rampSamples_)
            w = 0.5 * (1.0 - std::cos (kPi * (double) edge / (double) rampSamples_));
    }

    const double v = (double) peak_ * w * std::sin (phi);

    // The peak is already clamped, w is in [0,1] and |sin| <= 1, so this cannot
    // exceed kSoundcheckMaxPeak. The clamp is here anyway because it costs one
    // instruction and this is the sample that leaves for a loudspeaker.
    return (float) std::min (std::max (v, -(double) kSoundcheckMaxPeak),
                             (double) kSoundcheckMaxPeak);
}

double SoundcheckSignal::instantaneousHz (std::int64_t n) const noexcept
{
    if (totalSamples_ <= 0)
        return lowHz_;
    const double t = (double) n / (double) totalSamples_;
    return lowHz_ * std::exp (k_ * t);
}

std::int64_t SoundcheckSignal::rampOutSamples (double sampleRate) noexcept
{
    if (! (sampleRate > 0.0))
        return 0;
    return (std::int64_t) (kRampOutMs * sampleRate / 1000.0);
}

float SoundcheckSignal::rampOut (std::int64_t n, std::int64_t anchor,
                                 std::int64_t rampLengthSamples) noexcept
{
    if (n < anchor)
        return 1.0f;
    if (rampLengthSamples <= 0 || n >= anchor + rampLengthSamples)
        return 0.0f;   // EXACTLY zero -- the callback uses this to end the run

    const double x = (double) (n - anchor) / (double) rampLengthSamples;
    return (float) (0.5 * (1.0 + std::cos (kPi * x)));
}
