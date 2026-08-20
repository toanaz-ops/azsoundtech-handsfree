// NotchChain: per-channel cascade of up to 16 biquad notch filters.
//
// Each slot in the chain holds:
//   - a Biquad (configured for a notch at the requested frequency/Q), and
//   - a NotchInfo record (frequency, Q, depth, Idle/Active state).
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
    void   setNotch(int index, double freq, double Q, double depthDB);
    void   clearNotch(int index);
    void   reset();

    const NotchInfo& getNotchInfo(int index) const;
    int              getActiveNotchCount() const;

private:
    double                       sampleRate_;
    std::array<Biquad,    MAX_NOTCHES> filters_;
    std::array<NotchInfo, MAX_NOTCHES> notchInfo_;
};