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

#pragma once

class Biquad
{
public:
    Biquad();

    void setNotchFilter(double freq, double Q, double sampleRate);
    double processSample(double input);
    void reset();

private:
    // Coefficients (a0-normalised).
    double b0_, b1_, b2_;
    double a1_, a2_;

    // Direct Form I transposed state.
    double z1_, z2_;
};