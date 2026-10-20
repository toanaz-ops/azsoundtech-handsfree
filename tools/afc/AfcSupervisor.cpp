#include "AfcSupervisor.h"

#include <algorithm>
#include <cmath>

namespace
{
    std::int64_t samples (double seconds, double sr) noexcept
    {
        return (std::int64_t) std::llround (seconds * sr);
    }
}

void AfcSupervisor::prepare (const double sampleRate) noexcept
{
    sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    mixStep_ = (float) (1.0 / std::max (1.0, kFadeSeconds * sr_));
    reset();
}

void AfcSupervisor::reset() noexcept
{
    now_ = 0;
    status_ = Status::Off;
    lastTrip_ = TripKind::None;
    mix_ = 0.0f;
    wasEnabled_ = false;
    lockedUntilCycled_ = false;
    runningSince_ = 0;
    restUntil_ = -1;
    for (auto& t : tripTimes_) t = -1;
    for (auto& t : softTimes_) t = -1;
    duckStart_ = -1;
    needReset_ = true;
}

void AfcSupervisor::beginDuck() noexcept
{
    // A new event inside a duck extends the HOLD: restart from the bottom
    // rather than ramping down again from wherever the release had got to.
    const auto attack = samples (kFadeSeconds, sr_);
    if (duckStart_ >= 0 && now_ - duckStart_ >= attack)
        duckStart_ = now_ - attack;
    else if (duckStart_ < 0)
        duckStart_ = now_;
}

float AfcSupervisor::duckGain() noexcept
{
    if (duckStart_ < 0)
        return 1.0f;

    const auto t       = now_ - duckStart_;
    const auto attack  = samples (kFadeSeconds, sr_);
    const auto hold    = samples (kDuckHoldSeconds, sr_);
    const auto release = samples (kDuckReleaseSeconds, sr_);

    double db;
    if (t < attack)
        db = kDuckDb * (double) t / (double) attack;
    else if (t < attack + hold)
        db = kDuckDb;
    else if (t < attack + hold + release)
        db = kDuckDb * (1.0 - (double) (t - attack - hold) / (double) release);
    else
    {
        duckStart_ = -1;
        return 1.0f;
    }

    return (float) std::pow (10.0, db / 20.0);
}

void AfcSupervisor::registerTrip (const TripKind kind) noexcept
{
    lastTrip_ = kind;
    ++tripCount_;

    // Sliding window of the last kLockTrips trip times.
    for (int i = kLockTrips - 1; i > 0; --i)
        tripTimes_[i] = tripTimes_[i - 1];
    tripTimes_[0] = now_;

    const bool lock = tripTimes_[kLockTrips - 1] >= 0
                   && now_ - tripTimes_[kLockTrips - 1] <= samples (kLockWindowSeconds, sr_);

    if (kind == TripKind::NonFinite)
        mix_ = 0.0f;                                   // same sample, no crossfade

    beginDuck();
    restUntil_ = now_ + samples (kRestSeconds, sr_);
    needReset_ = true;

    if (lock)
    {
        lockedUntilCycled_ = true;
        status_ = Status::Locked;
    }
    else
    {
        status_ = Status::Resting;
    }
}

AfcSupervisor::Step AfcSupervisor::tick (const bool enabled, const bool frozen,
                                         const TripKind trip, const bool notchPlaced) noexcept
{
    Step s;

    // Switch edges. Off -> a crossfade out AND a duck (B2): the operator may be
    // using the headroom AFC gave them.
    if (wasEnabled_ && ! enabled && (status_ == Status::Learning || status_ == Status::Running))
        beginDuck();
    if (! enabled)
        lockedUntilCycled_ = false;          // cycling the switch clears the lock
    wasEnabled_ = enabled;

    const bool active = status_ == Status::Learning || status_ == Status::Running;

    // Trips only count while the canceller's output is in use.
    if (active && ! frozen)
    {
        TripKind t = trip;

        if (t == TripKind::None && notchPlaced)
        {
            softTimes_[1] = softTimes_[0];
            softTimes_[0] = now_;
            if (softTimes_[1] >= 0 && now_ - softTimes_[1] <= samples (kSoftWindowSeconds, sr_))
            {
                t = TripKind::Soft;
                softTimes_[0] = softTimes_[1] = -1;
            }
        }

        if (t != TripKind::None)
            registerTrip (t);
    }

    // Where should we be?
    if (frozen)
    {
        // Lane M: out, from zero afterwards, no duck.
        if (status_ != Status::Frozen && status_ != Status::Off)
            needReset_ = true;
        status_ = enabled ? Status::Frozen : Status::Off;
    }
    else if (! enabled)
    {
        status_ = Status::Off;
    }
    else if (lockedUntilCycled_)
    {
        status_ = Status::Locked;
    }
    else if (status_ == Status::Resting && now_ < restUntil_)
    {
        // stay resting
    }
    else if (status_ != Status::Learning && status_ != Status::Running)
    {
        status_ = Status::Learning;
        runningSince_ = now_;
        needReset_ = true;                   // always start from zero
    }

    if (status_ == Status::Learning && now_ - runningSince_ >= samples (kLearningSeconds, sr_))
        status_ = Status::Running;

    const bool wantOn = status_ == Status::Learning || status_ == Status::Running;

    // The canceller runs while its output is (fading) in use; once the fade
    // out has finished it stops and is reset.
    if (wantOn)
        mix_ = std::min (1.0f, mix_ + mixStep_);
    else
        mix_ = std::max (0.0f, mix_ - mixStep_);

    s.mix  = mix_;
    s.runCanceller = wantOn || mix_ > 0.0f;

    if (needReset_ && (wantOn || mix_ == 0.0f))
    {
        // Reset only when nothing of the old filter is still being mixed in:
        // either we are starting fresh, or the fade out is complete.
        if (! wantOn || mix_ <= mixStep_)
        {
            s.resetCanceller = true;
            needReset_ = false;
        }
    }

    s.duck = duckGain();
    ++now_;
    return s;
}
