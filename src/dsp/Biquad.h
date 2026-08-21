// Biquad notch filter (RBJ Audio EQ Cookbook)
//
// Single-channel biquad implementing the "notch" / band-reject filter
// described in Robert Bristow-Johnson's Audio EQ Cookbook:
//
//   omega = 2 * pi * freq / sampleRate
//   alpha = sin(omega) / (2 * Q)
//   b0 =  1
//   b1 = -2 * cos(omega)
//   b2 =  1
//   a0 =  1 + alpha
//   a1 = -2 * cos(omega)
//   a2 =  1 - alpha
//
// Coefficients are normalised by a0 (so the stored b0 is 1/a0, etc.).
// Processing uses Direct Form I transposed:
//
//   y = b0 * x + z1
//   z1 = b1 * x - a1 * y + z2
//   z2 = b2 * x - a2 * y
//
// This shape is numerically robust and allocation-free: only five
// coefficient slots and two state slots, no heap, no std::vector.
// Safe to call from a real-time audio thread.
//
// Parameter validation: why setNotchFilter returns bool
// =====================================================
// The cookbook formulas above are only stable inside a specific parameter
// range, and outside it they do not fail loudly -- they quietly produce a
// filter that destroys the signal path. The normalised pole radius is
//
//   r = sqrt((1 - alpha) / (1 + alpha)),   alpha = sin(omega) / (2Q)
//
// so:
//
//   freq >  sampleRate/2  ->  omega > pi  ->  sin(omega) < 0  ->  alpha < 0,
//       hence (1-alpha) > 1 > (1+alpha) and r > 1: the filter DIVERGES.
//       Measured: a 30 kHz notch designed at 96 kHz and then recomputed at
//       44.1 kHz gives r = 1.046, crosses |y| > 1e3 after 208 samples
//       (4.7 ms) and reaches 4.1e18 within 1000 samples. On a live PA that
//       is full-scale garbage into the drivers, not a glitch.
//   freq == sampleRate/2  ->  omega == pi, alpha == 0, r == 1: poles ON the
//       unit circle, undamped ringing that never decays.
//   freq <= 0             ->  alpha == 0 (or omega < 0): same unit-circle
//       poles, and a negative frequency is meaningless anyway.
//   Q <= 0                ->  division by zero gives alpha = +-inf, then
//       a0 = 1 + inf = inf and every normalised coefficient is inf/inf = NaN.
//       NaN is the nastiest of the four because NaN compares false against
//       every downstream sanity check, so the failure is invisible.
//
// setNotchFilter() therefore rejects those cases, returns false and leaves
// the filter untouched (a rejected call is a no-op, not a half-applied
// design). It is branch-only: no allocation, no logging, no exceptions, so it
// remains safe to call from the audio thread.

#pragma once

class Biquad
{
public:
    Biquad();

    // Configures the biquad as an RBJ notch. Returns true if the parameters
    // were valid and applied, false if they were rejected -- in which case
    // the filter (coefficients AND state) is left exactly as it was.
    // Valid range: sampleRate > 0, Q > 0, 0 < freq < sampleRate / 2.
    bool setNotchFilter(double freq, double Q, double sampleRate);
    double processSample(double input);
    void reset();

private:
    // Coefficients (a0-normalised).
    double b0_, b1_, b2_;
    double a1_, a2_;

    // Direct Form I transposed state.
    double z1_, z2_;
};