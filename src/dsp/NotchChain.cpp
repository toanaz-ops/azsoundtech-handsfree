#include "dsp/NotchChain.h"

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

    // Configure the biquad for this notch. We treat `depthDB` as a hint
    // (currently unused at the biquad level -- a future revision can
    // apply it as a post-filter gain), but we still record it so callers
    // and tests can see what was requested.
    //
    // If the biquad rejects the parameters (see Biquad.h) the slot is left
    // COMPLETELY untouched. Activating it anyway would mark the slot Active
    // while filters_[index] still held whatever design was there before, so
    // the chain would report a notch at one frequency and filter another.
    if (! filters_[index].setNotchFilter(freq, Q, sampleRate_))
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

void NotchChain::reset()
{
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
            if (! filters_[i].setNotchFilter(notchInfo_[i].frequency, notchInfo_[i].Q, sampleRate_))
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