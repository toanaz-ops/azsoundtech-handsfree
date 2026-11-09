#include <gtest/gtest.h>

#include "app/WindowFit.h"

TEST (WindowFit, BigScreenKeepsDefault)
{
    const auto s = clampSizeToArea ({ 1520, 980 }, { 1200, 700 }, { 0, 0, 2560, 1400 });
    EXPECT_EQ (s.x, 1520);
    EXPECT_EQ (s.y, 980);
}

TEST (WindowFit, SmallLaptopClampsToUserArea)
{
    const auto s = clampSizeToArea ({ 1520, 980 }, { 1200, 700 }, { 0, 0, 1366, 728 });
    EXPECT_EQ (s.x, 1366);
    EXPECT_EQ (s.y, 728);
}

TEST (WindowFit, NeverBelowMinimum)
{
    const auto s = clampSizeToArea ({ 1520, 980 }, { 1200, 700 }, { 0, 0, 1024, 600 });
    EXPECT_EQ (s.x, 1200);
    EXPECT_EQ (s.y, 700);
}

TEST (WindowFit, CentredInOffsetUserArea)
{
    const auto b = fitWindowBounds ({ 1520, 980 }, { 1200, 700 }, { 100, 50, 2000, 1200 });
    EXPECT_EQ (b, juce::Rectangle<int> (100 + 240, 50 + 110, 1520, 980));
}

TEST (WindowFit, OversizedMinimumAnchorsTopLeft)
{
    const auto b = fitWindowBounds ({ 1520, 980 }, { 1200, 700 }, { 0, 0, 1024, 600 });
    EXPECT_EQ (b.getX(), 0);
    EXPECT_EQ (b.getY(), 0);
}
