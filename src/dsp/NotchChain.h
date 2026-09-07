// NotchChain: per-channel cascade of up to 16 biquad notch filters.
//
// Each slot in the chain holds:
//   - a Biquad (configured for a notch at the requested frequency, Q AND
//     depth -- see Biquad.h's four-argument setNotchFilter), and
//   - a NotchInfo record (frequency, Q, depth, Idle/Active state).
//
// Depth is a NEGATIVE number of dB (-12.0 == 12 dB down), matching the preset
// JSON in plan Task 25 and the GUI in spec 6.1. Spec 5.1 calls for 6-24 dB.
// A positive depth is refused by the biquad rather than applied, because in a
// feedback eliminator it would boost the frequency that is already ringing.
//
// Idle slots are bypassed -- processSample passes the sample straight
// through. Active slots are chained in series so a single input is filtered
// by every active notch in turn. The chain pre-allocates every Biquad and
// NotchInfo in its constructor; processSample() never allocates and never
// touches the heap.
//
// Threading: all methods are intended to be called from a single thread
// (the audio thread). The detector thread sends intent via the SPSC ring
// buffer; another layer (Task 8) drains that buffer on the audio thread
// and calls setNotch / clearNotch here.

#pragma once

#include "dsp/Biquad.h"

#include <array>

class NotchChain
{
public:
    static constexpr int MAX_NOTCHES = 16;

    // kRampMs: a depth-only retune walks its coefficients over 10 ms --
    // 441 samples at 44.1 kHz, 480 at 48 kHz, 960 at 96 kHz. The ramp is
    // per-sample, so how it lands on callback boundaries does not matter:
    // at a 1024-sample buffer or larger it starts and finishes inside one
    // callback at every rate above; at a 64-sample buffer it straddles
    // about 7 callbacks at 44.1/48 kHz and 15 at 96 kHz. 10 ms is chosen so
    // a 6 dB rung moves at most 0.6 dB/ms (spec 4.7); nothing in NotchChain
    // enforces that bound -- the controller's ladder must not send a
    // multi-rung step in the deep direction.
    static constexpr double kRampMs = 10.0;

    enum class NotchState
    {
        Idle,
        Active
    };

    struct NotchInfo
    {
        double     frequency = 0.0;
        double     Q         = 0.0;
        double     depthDB   = 0.0;
        NotchState state     = NotchState::Idle;
    };

    explicit NotchChain(double sampleRate);

    double processSample(double input);

    // Installs a notch in `index`. If the biquad rejects the parameters
    // (sampleRate <= 0, Q <= 0, freq <= 0, or freq >= sampleRate/2 -- see
    // Biquad.h) the slot is left untouched and stays whatever it was.
    //
    // DEPTH-ONLY RETUNE (spec 4.7): when the slot is already Active and both
    // `freq` and `Q` compare EQUAL to the stored NotchInfo, only the depth is
    // moving, so the filter state is still meaningful and clearing it would be
    // a step discontinuity into the PA. That case routes to
    // Biquad::rampNotchDepth and interpolates over kRampMs instead. Everything
    // else -- an Idle slot, a new frequency, a new Q -- keeps taking the
    // setNotchFilter + reset path exactly as before.
    //
    // The comparison is a plain `==` on doubles ON PURPOSE: the caller
    // (NotchController::pushRetuneLocked) resends the freq and Q it stored
    // when the notch was placed, so the values are bit-identical by
    // construction. A near-miss falls through to the reset path, which is the
    // SAFE direction to fail in -- an audible click, never a filter running
    // one design while claiming another.
    void   setNotch(int index, double freq, double Q, double depthDB);
    void   clearNotch(int index);
    void   reset();

    // Sample-rate retarget: recomputes coefficients for every Active notch
    // against the new rate so each notch keeps its intended frequency in Hz.
    // Any Active notch whose frequency is no longer below the NEW Nyquist is
    // DEACTIVATED rather than clamped -- its stored NotchInfo is kept so it
    // can be reinstated if the rate goes back up. Retargeting such a notch
    // blind would place its poles outside the unit circle and the chain
    // would diverge (Biquad.h has the derivation and the measured numbers).
    // Idle notches retain their stored NotchInfo. Called from
    // audioDeviceAboutToStart() (UI/device thread) BEFORE the audio callback
    // runs, so it is not real-time critical -- and it never allocates, it
    // only rewrites the pre-allocated coefficient slots.
    void   setSampleRate(double sampleRate);
    double getSampleRate() const;

    const NotchInfo& getNotchInfo(int index) const;
    int              getActiveNotchCount() const;

private:
    double                       sampleRate_;
    std::array<Biquad,    MAX_NOTCHES> filters_;
    std::array<NotchInfo, MAX_NOTCHES> notchInfo_;
};