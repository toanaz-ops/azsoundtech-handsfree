// AfcSupervisor: what happens AROUND one lane's AfcCanceller (lane A1, spec
// 2026-10-19 sections 3.5-3.7). Pure state machine, ticked once per sample on
// the audio thread; no allocation, no JUCE.
//
// It owns four things the canceller must not decide for itself:
//
//   mix   0..1, how much of the cancellation reaches the output:
//         out = y + mix * (e - y). Ramps over 10 ms in both directions, so
//         switching AFC on, off, or tripping it is never a step. A NON-FINITE
//         trip drops it to 0 on the same sample (a crossfade through NaN is a
//         NaN, which the guard turns into 0 -- a hard cut).
//   duck  the Q7 safety gain: -6 dB in 10 ms on a trip or a switch-off, held
//         5 s, released over 5 s, linear in dB. Not on a lane M freeze -- the
//         operator has just pulled the master down for the measurement.
//   rest  5 s after a trip before the canceller is allowed back, from zero;
//         3 trips inside 60 s latch it off until the switch is cycled.
//   soft  a notch placed by the detector while AFC runs is a sign the loop is
//         not held; 2 of them inside 10 s count as a trip.

#pragma once

#include <cstdint>

class AfcSupervisor
{
public:
    enum class Status { Off, Learning, Running, Resting, Locked, Frozen };
    enum class TripKind { None, NonFinite, Inflation, Coefficients, Soft };

    struct Step
    {
        float mix  = 0.0f;
        float duck = 1.0f;          // linear gain applied after the notch chain
        bool  runCanceller   = false;
        bool  resetCanceller = false;
    };

    void prepare (double sampleRate) noexcept;
    void reset() noexcept;          // device restart: everything back to Off, no duck

    // enabled: the operator's switch (snapshotted per callback).
    // frozen : lane M is running (scSuspendTaps || scCaptureActive).
    // trip   : the canceller's hard trip for this sample, if any.
    // notchPlaced: the detector placed a notch on this slot this sample.
    Step tick (bool enabled, bool frozen, TripKind trip, bool notchPlaced) noexcept;

    [[nodiscard]] Status status() const noexcept { return status_; }
    [[nodiscard]] TripKind lastTrip() const noexcept { return lastTrip_; }
    [[nodiscard]] std::uint32_t tripCount() const noexcept { return tripCount_; }

    // Numbers the spec fixes, exposed for tests.
    static constexpr double kFadeSeconds     = 0.010;
    static constexpr double kDuckDb          = -6.0;
    static constexpr double kDuckHoldSeconds = 5.0;
    static constexpr double kDuckReleaseSeconds = 5.0;
    static constexpr double kRestSeconds     = 5.0;
    static constexpr double kLearningSeconds = 20.0;
    static constexpr int    kLockTrips       = 3;
    static constexpr double kLockWindowSeconds = 60.0;
    static constexpr int    kSoftTrips       = 2;
    static constexpr double kSoftWindowSeconds = 10.0;

private:
    void beginDuck() noexcept;
    void registerTrip (TripKind kind) noexcept;
    [[nodiscard]] float duckGain() noexcept;

    double sr_ = 48000.0;
    std::int64_t now_ = 0;              // samples since prepare/reset

    Status status_ = Status::Off;
    TripKind lastTrip_ = TripKind::None;
    std::uint32_t tripCount_ = 0;

    float mix_ = 0.0f;
    float mixStep_ = 0.0f;              // per sample, set in prepare
    bool  wasEnabled_ = false;
    bool  lockedUntilCycled_ = false;

    std::int64_t runningSince_ = 0;
    std::int64_t restUntil_ = -1;
    std::int64_t tripTimes_[kLockTrips] = { -1, -1, -1 };
    std::int64_t softTimes_[kSoftTrips] = { -1, -1 };

    std::int64_t duckStart_ = -1;       // -1 = no duck in progress
    bool needReset_ = false;
};
