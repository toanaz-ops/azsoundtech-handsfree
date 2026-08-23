// tests/test_clocksource.cpp
#include <gtest/gtest.h>
#include <juce_events/juce_events.h>
#include "dsp/ClockSource.h"

namespace {
class FakeClock : public ClockSource {
public:
    double nowMs() const override { return ms_; }
    void advance (double ms) { ms_ += ms; }
private:
    double ms_ = 1000.0;
};
}

TEST (ClockSource, FakeAdvancesOnCommand)
{
    FakeClock clock;
    EXPECT_DOUBLE_EQ (clock.nowMs(), 1000.0);
    clock.advance (30000.0);
    EXPECT_DOUBLE_EQ (clock.nowMs(), 31000.0);
}

TEST (ClockSource, JuceClockIsMonotonic)
{
    JuceMonotonicClock clock;
    const double a = clock.nowMs();
    juce::Thread::sleep (5);
    const double b = clock.nowMs();
    EXPECT_GE (b, a);
}
