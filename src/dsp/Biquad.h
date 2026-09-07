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

// Depth retune without a click: rampNotchDepth()
// ==============================================
// setNotchFilter() calls reset() because a new DESIGN owns no state from the
// old one. That is right when the frequency or Q moves, and wrong when only
// the depth does: clearing z1/z2 mid-signal is a step discontinuity straight
// into a PA. rampNotchDepth() therefore keeps the state and walks the five
// normalised coefficients linearly to the new design over `rampSamples`.
//
// Why an interpolated coefficient set is safe (spec 4.7, M-4)
// -----------------------------------------------------------
// Fix freq/Q/sampleRate, write u_i = alpha/A_i and d_i = 1 + u_i. Every convex
// combination (weights w_i) of the a0-normalised peaking designs keeps
// b1 == a1 and satisfies P + alpha*K == 1, with P = sum(w_i/d_i) and
// K = sum(w_i/(A_i*d_i)). Substituting, the interpolated set IS a peaking RBJ
// filter with numerator gain A_n = sum(w_i*A_i/d_i)/P and denominator gain
// 1/A_d = sum(w_i/(A_i*d_i))/P:
//
//   |H(w)|^2 = [(cos w - cos w0)^2 + alpha^2 * A_n^2 * sin^2 w]
//            / [(cos w - cos w0)^2 + alpha^2 * A_d^-2 * sin^2 w]
//
// Every A_i <= 1 (depth <= 0), so A_n <= 1 <= 1/A_d and |H| <= 1 at EVERY
// frequency -- the ramp cannot boost anything, including the frequency it is
// pointed at. The pole radius is sqrt((1 - alpha/A_d)/(1 + alpha/A_d)) < 1, so
// it cannot diverge either. A ramp restarted mid-flight is the three-point
// case of the same argument, and so is a ramp started from a never-configured
// identity biquad (b0=1, all else 0 -- A == 1, i.e. depth == 0): the stability
// triangle these designs live in is convex and contains that origin point, so
// interpolating from it is still a valid convex combination (measured: worst
// case -3.5e-11 dB, i.e. still non-boosting to within floating-point noise).
// Measured 2026-09-06: max gain 1.9e-15 dB over
// 3 rates x 5 frequencies x 3 Qs x 7 depth pairs x 101 probes, and over 20 000
// random convex combinations x 400 frequencies. Coefficient deltas are ~1e-6
// per sample -- nowhere near the subnormal range.
//
// A ramp is only ever entered between two designs sharing freq, Q and
// sampleRate (NotchChain::setNotch enforces that). setNotchFilter() and
// reset() CANCEL an in-flight ramp: the newer instruction wins.
//
// NotchChain::clearNotch does NOT cancel a ramp, so an Idle slot can carry
// rampRemaining_ > 0 that nobody ticks. Harmless: setNotch on an Idle slot
// takes the reset path, which cancels it. And a ramp cut short by a device
// stop leaves the filter at an intermediate depth while NotchInfo.depthDB
// already reads the target -- 10 ms of disagreement, accepted (spec 4.7, m-5).
//
// reset() also fires by accident, per sample: AudioEngine.cpp:617 calls
// chain.reset() from inside the per-sample audio loop as a NaN self-heal
// whenever the chain's output goes non-finite, and NotchChain::reset() loops
// every filter, so one bad sample cancels every in-flight ramp in that
// chain -- not just the one that produced it. Unlike a device-restart reset,
// this path is NOT followed by setSampleRate()'s coefficient replay
// (NotchChain.cpp:110), so the filter is left frozen at whatever intermediate
// depth the ramp had reached, indefinitely, while NotchInfo.depthDB keeps
// reading the target the ramp never finished walking to. This class does not
// re-arm the ramp on its own; whether a NaN heal should re-apply the target
// is a decision for the caller that wires the ramp (NotchChain::setNotch),
// not for Biquad.

#pragma once

class Biquad
{
public:
    Biquad();

    // Configures the biquad as an RBJ notch. Returns true if the parameters
    // were valid and applied, false if they were rejected -- in which case
    // the filter (coefficients AND state) is left exactly as it was.
    // Valid range: sampleRate > 0, Q > 0, 0 < freq < sampleRate / 2.
    //
    // This is the INFINITE-depth null: the response at `freq` is zero. It has
    // no depth parameter, which is why depthDB used to be storable in
    // NotchChain::NotchInfo and appliable nowhere. Product code wants the
    // four-argument form below; this one is retained because it is a real
    // filter, it is the mathematical parent of the finite-depth form (its
    // dB -> -inf limit), and the tests above cover it thoroughly.
    bool setNotchFilter(double freq, double Q, double sampleRate);

    // Configures the biquad as a finite-depth notch: an RBJ peaking filter
    // driven with negative gain. `depthDB` is the attenuation at `freq`,
    // expressed as a NEGATIVE number of decibels (-12.0 means 12 dB down),
    // matching the sign convention already used by NotchChain::setNotch, the
    // preset JSON in plan Task 25, and the GUI mock-up in spec 6.1.
    //
    // Magnitude at the centre frequency is exactly A^2, where
    // A = 10^(depthDB/40) -- that is, 10^(depthDB/20), the requested depth by
    // construction. Derivation: at omega == omega0 the numerator collapses to
    // 2j*alpha*A*sin(omega0) and the denominator to 2j*(alpha/A)*sin(omega0),
    // so H = A / (1/A) = A^2.
    //
    // Rejected inputs, all leaving the filter untouched:
    //   - the same four the pure notch rejects. The pole radius here is
    //     sqrt((1 - alpha/A) / (1 + alpha/A)), which is the pure-notch
    //     expression with alpha replaced by alpha/A, so every input that
    //     made that one diverge does the same to this one.
    //   - depthDB > 0. The peaking form is symmetric: a positive gain BOOSTS
    //     the centre frequency by that many dB. In a feedback eliminator a
    //     sign error upstream would amplify precisely the frequency that is
    //     already ringing, so a boost is refused rather than trusted.
    //     depthDB == 0 is accepted and is a mathematical no-op (A == 1 makes
    //     the numerator and denominator identical, H == 1).
    //
    // Branch-only and allocation-free, like its sibling: reachable from the
    // audio thread via NotchChain::setNotch.
    bool setNotchFilter(double freq, double Q, double sampleRate, double depthDB);

    // Retunes a RUNNING notch to `depthDB` while keeping the filter state,
    // interpolating all five coefficients over `rampSamples` calls to
    // processSample(). Applies the SAME four rejections as the four-argument
    // setNotchFilter (sampleRate > 0, Q > 0, 0 < freq < sampleRate/2,
    // depthDB <= 0); on rejection it returns false and leaves the
    // coefficients, the state AND any in-flight ramp exactly as they were.
    // rampSamples <= 0 installs the target immediately, still without reset().
    // Branch-only and allocation-free: reachable from the audio thread via
    // NotchChain::setNotch.
    bool rampNotchDepth(double freq, double Q, double sampleRate, double depthDB,
                        int rampSamples);

    // TEST ACCESSORS ONLY -- the ramp's whole point is that state SURVIVES it,
    // and no black-box measurement can tell a preserved state from a cleared
    // one (spec 5.1, M-7).
    struct State  { double z1, z2; };
    struct Coeffs { double b0, b1, b2, a1, a2; };
    State  stateForTest()  const { return { z1_, z2_ }; }
    Coeffs coeffsForTest() const { return { b0_, b1_, b2_, a1_, a2_ }; }
    int    rampRemainingForTest() const { return rampRemaining_; }
    double processSample(double input);
    void reset();

private:
    // The four-argument peaking design, in ONE place: setNotchFilter and
    // rampNotchDepth must never be able to drift apart on either the formula
    // or the four rejections. Writes the a0-normalised b0,b1,b2,a1,a2 into
    // `out` and returns true; returns false and writes nothing on rejection.
    static bool designPeaking(double freq, double Q, double sampleRate,
                              double depthDB, double out[5]);

    // Coefficients (a0-normalised).
    double b0_, b1_, b2_;
    double a1_, a2_;

    // Direct Form I transposed state.
    double z1_, z2_;

    // Ramp state. rampRemaining_ == 0 means "not ramping" and is the only
    // thing processSample() branches on.
    double target_[5];
    double delta_[5];
    int    rampRemaining_;
};