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
    , target_{ 1.0, 0.0, 0.0, 0.0, 0.0 }
    , delta_{ 0.0, 0.0, 0.0, 0.0, 0.0 }
    , rampRemaining_(0)
{
}

bool Biquad::designPeaking(double freq, double Q, double sampleRate,
                           double depthDB, double out[5])
{
    // The same four guards as the pure notch -- see the derivation in
    // Biquad.h. The pole radius here is sqrt((1 - alpha/A) / (1 + alpha/A)),
    // i.e. the pure-notch expression with alpha replaced by alpha/A, so the
    // same inputs push the poles onto or outside the unit circle.
    if (sampleRate <= 0.0)          return false;
    if (Q <= 0.0)                   return false;
    if (freq <= 0.0)                return false;
    if (freq >= 0.5 * sampleRate)   return false;

    // A positive depth is a BOOST at the centre frequency. This filter sits in
    // a feedback eliminator, so the one frequency it must never amplify is the
    // one it was pointed at. Refuse rather than trust the caller's sign.
    if (depthDB > 0.0)              return false;

    const double A     = std::pow(10.0, depthDB / 40.0);
    const double omega = 2.0 * kPi * freq / sampleRate;
    const double alpha = std::sin(omega) / (2.0 * Q);
    const double cosw  = std::cos(omega);

    const double a0 = 1.0 + alpha / A;

    // Normalise so a0 == 1 -- lets processSample skip one multiply.
    out[0] = (1.0 + alpha * A) / a0;   // b0
    out[1] = (-2.0 * cosw)     / a0;   // b1
    out[2] = (1.0 - alpha * A) / a0;   // b2
    out[3] = (-2.0 * cosw)     / a0;   // a1
    out[4] = (1.0 - alpha / A) / a0;   // a2
    return true;
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
    double c[5];
    if (! designPeaking(freq, Q, sampleRate, depthDB, c))
    {
        return false;
    }

    b0_ = c[0]; b1_ = c[1]; b2_ = c[2]; a1_ = c[3]; a2_ = c[4];

    // A new DESIGN wins over an in-flight depth ramp: the ramp interpolates
    // toward a target this call has just superseded.
    rampRemaining_ = 0;

    // Filter coefficients changed -- state belongs to the previous design.
    reset();
    return true;
}

bool Biquad::rampNotchDepth(double freq, double Q, double sampleRate, double depthDB,
                            int rampSamples)
{
    double c[5];
    if (! designPeaking(freq, Q, sampleRate, depthDB, c))
    {
        // Rejected: coefficients, state and any in-flight ramp are untouched.
        return false;
    }

    if (rampSamples <= 0)
    {
        b0_ = c[0]; b1_ = c[1]; b2_ = c[2]; a1_ = c[3]; a2_ = c[4];
        rampRemaining_ = 0;
        return true;   // NO reset(): the state is still valid at this freq/Q.
    }

    // Deltas are measured from the coefficients the filter is running RIGHT
    // NOW, not from the previous target, so a second command arriving
    // mid-ramp starts a fresh straight line from wherever the first one got
    // to instead of snapping back.
    const double current[5] = { b0_, b1_, b2_, a1_, a2_ };
    for (int i = 0; i < 5; ++i)
    {
        target_[i] = c[i];
        delta_[i]  = (c[i] - current[i]) / static_cast<double>(rampSamples);
    }
    rampRemaining_ = rampSamples;
    return true;
}

double Biquad::processSample(double input)
{
    // The ONLY new work on the audio thread: one comparison, and while a ramp
    // is live either five additions or (on the last sample) five assignments.
    // No allocation, no logging.
    if (rampRemaining_ > 0)
    {
        if (--rampRemaining_ == 0)
        {
            // Land ON the target rather than on the accumulated sum of
            // deltas, so a long ramp cannot drift by rounding error.
            b0_ = target_[0]; b1_ = target_[1]; b2_ = target_[2];
            a1_ = target_[3]; a2_ = target_[4];
        }
        else
        {
            b0_ += delta_[0]; b1_ += delta_[1]; b2_ += delta_[2];
            a1_ += delta_[3]; a2_ += delta_[4];
        }
    }

    // Direct Form I transposed.
    const double output = b0_ * input + z1_;
    z1_ = b1_ * input - a1_ * output + z2_;
    z2_ = b2_ * input - a2_ * output;
    return output;
}

void Biquad::clearState()
{
    // State only. rampRemaining_ and the five coefficients are deliberately
    // untouched -- see the header: a muted lane has had its INPUT interrupted,
    // which says nothing about a depth ramp that is still the right thing to
    // finish when the input comes back.
    z1_ = 0.0;
    z2_ = 0.0;
}

void Biquad::reset()
{
    z1_ = 0.0;
    z2_ = 0.0;
    // An explicit reset is a statement that the past is gone; a ramp toward a
    // target designed against that past has nothing left to interpolate.
    rampRemaining_ = 0;
}
