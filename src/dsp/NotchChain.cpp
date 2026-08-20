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
    filters_[index].setNotchFilter(freq, Q, sampleRate_);

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