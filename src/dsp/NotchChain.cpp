#include "dsp/NotchChain.h"

#include <cmath>

NotchChain::NotchChain(double sampleRate)
    : sampleRate_(sampleRate)
{
}

double NotchChain::processSample(double input)
{
    // Allocation-free: walk the pre-allocated filter array, processing
    // only the Active slots. Idle slots are skipped with a single branch.
    // The chain is a series cascade, so each active filter takes the
    // previous filter's output as its input.
    double sample = input;
    for (int i = 0; i < MAX_NOTCHES; ++i)
    {
        if (notchInfo_[i].state == NotchState::Active)
        {
            sample = filters_[i].processSample(sample);
        }
    }
    return sample;
}

void NotchChain::setNotch(int index, double freq, double Q, double depthDB)
{
    if (index < 0 || index >= MAX_NOTCHES)
    {
        return;
    }

    // Depth-only retune of a RUNNING notch: keep the state, ramp the
    // coefficients (see the header). rampSamples is rounded from kRampMs
    // against the chain's CURRENT rate, so the ramp is 10 ms of real time at
    // any device rate. A rejected design leaves the slot completely alone,
    // exactly like the path below.
    if (notchInfo_[index].state == NotchState::Active
        && freq == notchInfo_[index].frequency
        && Q    == notchInfo_[index].Q)
    {
        const int rampSamples = static_cast<int>(std::lround(kRampMs * sampleRate_ / 1000.0));
        if (! filters_[index].rampNotchDepth(freq, Q, sampleRate_, depthDB, rampSamples))
        {
            return;
        }
        // The reported depth is the TARGET, from the instant the command is
        // accepted -- the model, the GUI and the preset all describe intent,
        // not the ramp's momentary position (spec 4.7).
        notchInfo_[index].depthDB = depthDB;
        return;
    }

    // Configure the biquad for this notch, DEPTH INCLUDED. `depthDB` used to
    // be recorded here and dropped on the floor -- the three-argument
    // setNotchFilter is an infinite-depth null and has no depth parameter --
    // so every notch this app placed was a full null no matter what was asked
    // for, and spec 5.1's 6-24 dB range was unreachable.
    //
    // If the biquad rejects the parameters (see Biquad.h) the slot is left
    // COMPLETELY untouched. Activating it anyway would mark the slot Active
    // while filters_[index] still held whatever design was there before, so
    // the chain would report a notch at one frequency and filter another.
    // A positive depthDB is among the rejected cases: it would boost the
    // ringing frequency instead of cutting it.
    if (! filters_[index].setNotchFilter(freq, Q, sampleRate_, depthDB))
    {
        return;
    }

    notchInfo_[index].frequency = freq;
    notchInfo_[index].Q         = Q;
    notchInfo_[index].depthDB   = depthDB;
    notchInfo_[index].state     = NotchState::Active;
}

void NotchChain::clearNotch(int index)
{
    if (index < 0 || index >= MAX_NOTCHES)
    {
        return;
    }

    notchInfo_[index].state = NotchState::Idle;
}

void NotchChain::clearState()
{
    // State only -- coefficients and in-flight depth ramps survive. See the
    // header for why this is NOT reset().
    for (int i = 0; i < MAX_NOTCHES; ++i)
    {
        filters_[i].clearState();
    }
}

const Biquad& NotchChain::getFilterForTest(int index) const
{
    const int clamped = (index < 0) ? 0
                      : (index >= MAX_NOTCHES) ? (MAX_NOTCHES - 1) : index;
    return filters_[static_cast<std::size_t>(clamped)];
}

void NotchChain::reset()
{
    // GAP (spec 4.7, m-5): this cancels every in-flight depth ramp and does
    // NOT re-apply the stored design, so a reset that is not followed by
    // setSampleRate()'s coefficient replay -- AudioEngine's per-sample NaN
    // self-heal is the one such caller -- strands the filter at the ramp's
    // intermediate depth while NotchInfo.depthDB already reads the target.
    for (int i = 0; i < MAX_NOTCHES; ++i)
    {
        filters_[i].reset();
    }
}

void NotchChain::setSampleRate(double sampleRate)
{
    // Guard: a non-positive rate is invalid -- leave the chain untouched.
    if (sampleRate <= 0.0)
    {
        return;
    }

    sampleRate_ = sampleRate;

    // Recompute coefficients for every Active notch from its STORED
    // NotchInfo (frequency, Q) against the new rate, so an active notch
    // keeps its intended frequency in Hz across a rate change. Idle notches
    // need no coefficient work; their stored NotchInfo is retained.
    //
    // NYQUIST GUARD. A notch that was legal at the old rate can sit above
    // the new Nyquist -- 30 kHz is fine at 96 kHz and impossible at 44.1 kHz
    // -- and replaying it blind would install poles outside the unit circle
    // and turn the chain into a divergent oscillator feeding the PA (see the
    // derivation in Biquad.h). Biquad::setNotchFilter refuses those
    // parameters, and when it does we DEACTIVATE the slot.
    //
    // We deliberately do NOT clamp the notch to just under the new Nyquist.
    // A notch that silently moves to a frequency that never rang is worse
    // than an absent one: the engineer sees the app filtering something it
    // never heard and stops trusting it. Anything above ~22 kHz was not
    // audible feedback in the first place.
    //
    // The stored NotchInfo is retained on deactivation, so if the device
    // goes back to a higher rate the notch can be reinstated as-is.
    for (int i = 0; i < MAX_NOTCHES; ++i)
    {
        if (notchInfo_[i].state == NotchState::Active)
        {
            // Depth is replayed along with frequency and Q. Retargeting
            // through the three-argument form would silently deepen every
            // notch in the chain to a full null on the first device reopen.
            if (! filters_[i].setNotchFilter(notchInfo_[i].frequency, notchInfo_[i].Q,
                                             sampleRate_, notchInfo_[i].depthDB))
            {
                notchInfo_[i].state = NotchState::Idle;
            }
        }
    }

    // Old filter state is meaningless at a new rate. reset() clears every
    // filter's state, including Idle ones that may hold stale state from a
    // previously cleared notch.
    reset();
}

double NotchChain::getSampleRate() const
{
    return sampleRate_;
}

const NotchChain::NotchInfo& NotchChain::getNotchInfo(int index) const
{
    // Out-of-range callers get a reference to a sentinel so we never
    // dereference past the array. The sentinel is a static so it lives
    // for the program's lifetime; reading it is always safe.
    static const NotchInfo kSentinel{};
    if (index < 0 || index >= MAX_NOTCHES)
    {
        return kSentinel;
    }
    return notchInfo_[index];
}

int NotchChain::getActiveNotchCount() const
{
    int count = 0;
    for (int i = 0; i < MAX_NOTCHES; ++i)
    {
        if (notchInfo_[i].state == NotchState::Active)
        {
            ++count;
        }
    }
    return count;
}