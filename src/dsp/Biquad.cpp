#include "dsp/Biquad.h"

#include <cmath>

namespace
{
    constexpr double kPi = 3.14159265358979323846;
}

Biquad::Biquad()
    : b0_(1.0)
    , b1_(0.0)
    , b2_(0.0)
    , a1_(0.0)
    , a2_(0.0)
    , z1_(0.0)
    , z2_(0.0)
{
}

bool Biquad::setNotchFilter(double freq, double Q, double sampleRate)
{
    // Parameter validation FIRST -- see the stability derivation in Biquad.h.
    // Every rejected case produces poles at radius >= 1 or NaN coefficients,
    // and the filter is left exactly as it was rather than half-written.
    //
    // Allocation-free, branch-only, no logging and no exceptions: this may be
    // reached from the audio thread via NotchChain::setNotch().
    if (sampleRate <= 0.0)
    {
        return false;
    }
    if (Q <= 0.0)
    {
        return false;
    }
    if (freq <= 0.0)
    {
        return false;
    }
    if (freq >= 0.5 * sampleRate)
    {
        return false;
    }

    // RBJ Audio EQ Cookbook -- notch / band-reject.
    const double omega = 2.0 * kPi * freq / sampleRate;
    const double alpha = std::sin(omega) / (2.0 * Q);
    const double cosw  = std::cos(omega);

    const double a0 = 1.0 + alpha;

    // Unnormalised coefficients straight from the cookbook.
    const double b0 = 1.0;
    const double b1 = -2.0 * cosw;
    const double b2 = 1.0;
    const double a1 = -2.0 * cosw;
    const double a2 = 1.0 - alpha;

    // Normalise so a0 == 1 -- lets processSample skip one multiply.
    b0_ = b0 / a0;
    b1_ = b1 / a0;
    b2_ = b2 / a0;
    a1_ = a1 / a0;
    a2_ = a2 / a0;

    // Filter coefficients changed -- state belongs to the previous design.
    reset();
    return true;
}

bool Biquad::setNotchFilter(double freq, double Q, double sampleRate, double depthDB)
{
    // Same four guards as the pure notch -- see the derivation in Biquad.h.
    // The pole radius here is sqrt((1 - alpha/A) / (1 + alpha/A)), i.e. the
    // pure-notch expression with alpha replaced by alpha/A, so the same inputs
    // push the poles onto or outside the unit circle.
    if (sampleRate <= 0.0)
    {
        return false;
    }
    if (Q <= 0.0)
    {
        return false;
    }
    if (freq <= 0.0)
    {
        return false;
    }
    if (freq >= 0.5 * sampleRate)
    {
        return false;
    }

    // A positive depth is a BOOST at the centre frequency. This filter sits in
    // a feedback eliminator, so the one frequency it must never amplify is the
    // one it was pointed at. Refuse rather than trust the caller's sign.
    if (depthDB > 0.0)
    {
        return false;
    }

    // RBJ Audio EQ Cookbook -- peakingEQ, driven with negative gain so the
    // "peak" is a cut of exactly depthDB at freq.
    const double A     = std::pow(10.0, depthDB / 40.0);
    const double omega = 2.0 * kPi * freq / sampleRate;
    const double alpha = std::sin(omega) / (2.0 * Q);
    const double cosw  = std::cos(omega);

    const double a0 = 1.0 + alpha / A;

    const double b0 = 1.0 + alpha * A;
    const double b1 = -2.0 * cosw;
    const double b2 = 1.0 - alpha * A;
    const double a1 = -2.0 * cosw;
    const double a2 = 1.0 - alpha / A;

    // Normalise so a0 == 1 -- lets processSample skip one multiply.
    b0_ = b0 / a0;
    b1_ = b1 / a0;
    b2_ = b2 / a0;
    a1_ = a1 / a0;
    a2_ = a2 / a0;

    // Filter coefficients changed -- state belongs to the previous design.
    reset();
    return true;
}

double Biquad::processSample(double input)
{
    // Direct Form I transposed.
    const double output = b0_ * input + z1_;
    z1_ = b1_ * input - a1_ * output + z2_;
    z2_ = b2_ * input - a2_ * output;
    return output;
}

void Biquad::reset()
{
    z1_ = 0.0;
    z2_ = 0.0;
}
