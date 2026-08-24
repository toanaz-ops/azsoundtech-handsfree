#include <gtest/gtest.h>
#include "app/SlotConfig.h"

TEST(SlotConfig, DefaultIsEnabledFalseStereoZeroOne)
{
    SlotConfig c;
    EXPECT_FALSE(c.enabled);
    EXPECT_EQ(c.width, 2);
    EXPECT_EQ(c.inputChannels[0], 0);
    EXPECT_EQ(c.outputChannels[1], 1);
}

TEST(SlotConfig, IsValidRejectsOutOfRangeChannels)
{
    SlotConfig c;                       // in/out {0,1}
    EXPECT_TRUE(slotConfigIsValid(c, 2, 2));
    EXPECT_FALSE(slotConfigIsValid(c, 1, 2));   // in ch1 > maxIn-1
    EXPECT_FALSE(slotConfigIsValid(c, 2, 1));   // out ch1 > maxOut-1
    SlotConfig mono; mono.width = 1;    // mono chi dung lan 0
    mono.inputChannels[1] = 99;
    EXPECT_TRUE(slotConfigIsValid(mono, 2, 2)); // lan >= width bi bo qua
}

TEST(SlotConfig, ClampPullsIndicesIntoRange)
{
    SlotConfig c; c.inputChannels[0] = 7; c.outputChannels[1] = 42;
    SlotConfig fixed = slotClampedTo(c, 4, 2);
    EXPECT_EQ(fixed.inputChannels[0], 3);
    EXPECT_EQ(fixed.outputChannels[1], 1);
    EXPECT_TRUE(slotConfigIsValid(fixed, 4, 2));
}
