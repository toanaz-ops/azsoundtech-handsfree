// src/dsp/ClockSource.h
#pragma once
#include <juce_core/juce_core.h>

// Injectable wall clock (bridge design §4). Deliberately NOT
// std::function<double()>: it may heap-allocate, and this is called in the
// detector's hot loop.
class ClockSource
{
public:
    virtual ~ClockSource() = default;
    virtual double nowMs() const = 0;
};

// Production impl: JUCE's high-res monotonic counter (not wall UTC -- immune
// to system clock changes mid-show).
class JuceMonotonicClock final : public ClockSource
{
public:
    double nowMs() const override { return juce::Time::getMillisecondCounterHiRes(); }
};
