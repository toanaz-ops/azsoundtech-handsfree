// Lane U (spec 2026-10-18 section 3.5): the setup wizard's state machine.

#include <gtest/gtest.h>

#include "app/SetupWizardModel.h"

TEST (SetupWizardModel, ChoicesAreMonoChannelsThenEvenPairs)
{
    const auto two = SetupWizardModel::choicesFor (2);
    ASSERT_EQ (two.size(), 3u);
    EXPECT_EQ (two[0].width, 1); EXPECT_EQ (two[0].channels[0], 0);
    EXPECT_EQ (two[1].width, 1); EXPECT_EQ (two[1].channels[0], 1);
    EXPECT_EQ (two[2].width, 2); EXPECT_EQ (two[2].channels[0], 0); EXPECT_EQ (two[2].channels[1], 1);

    EXPECT_EQ (SetupWizardModel::choicesFor (3).size(), 4u);   // 3 mono + (1,2)
    EXPECT_TRUE (SetupWizardModel::choicesFor (0).empty());
}

TEST (SetupWizardModel, CannotLeaveDeviceStepWithoutChannels)
{
    SetupWizardModel m;
    m.start (0, 0);   // device not delivering audio yet
    EXPECT_FALSE (m.canAdvance());
    m.next();
    EXPECT_EQ (m.getStep(), SetupWizardModel::Step::Device);

    m.setChannelCounts (2, 2);
    EXPECT_TRUE (m.canAdvance());
}

TEST (SetupWizardModel, OutputWidthFollowsTheMic)
{
    SetupWizardModel m;
    m.start (2, 4);
    m.next();
    m.chooseMic (2);   // stereo 1+2
    m.next();
    ASSERT_EQ (m.getStep(), SetupWizardModel::Step::Output);

    const auto outs = m.outputChoices();
    ASSERT_EQ (outs.size(), 2u);   // 1+2, 3+4
    for (const auto& o : outs)
        EXPECT_EQ (o.width, 2);

    m.chooseOutput (1);
    const auto r = m.result();
    EXPECT_TRUE (r.enabled);
    EXPECT_EQ (r.width, 2);
    EXPECT_EQ (r.inputChannels[1], 1);
    EXPECT_EQ (r.outputChannels[0], 2);
    EXPECT_EQ (r.outputChannels[1], 3);
}

TEST (SetupWizardModel, ChangingMicWidthDropsTheOutputChoice)
{
    SetupWizardModel m;
    m.start (2, 2);
    m.next();
    m.chooseMic (0);       // mono
    m.next();
    m.chooseOutput (1);    // mono out 2
    m.back();
    m.chooseMic (2);       // now stereo
    EXPECT_EQ (m.getOutputIndex(), -1);
    m.next();
    EXPECT_FALSE (m.canAdvance());
}

TEST (SetupWizardModel, ShrinkingDeviceDropsChoicesThatNoLongerExist)
{
    SetupWizardModel m;
    m.start (8, 8);
    m.next();
    m.chooseMic (7);   // mono ch 8
    m.setChannelCounts (2, 2);
    EXPECT_EQ (m.getMicIndex(), -1);
    EXPECT_FALSE (m.result().enabled);
}

TEST (SetupWizardModel, NoChoiceMeansNothingRouted)
{
    SetupWizardModel m;
    m.start (2, 2);
    EXPECT_FALSE (m.result().enabled);
}

TEST (SetupWizardModel, OverwriteNeedsConfirmOnlyWithMoreThanOneSlotOn)
{
    std::array<SlotConfig, kMaxSlots> slots {};
    EXPECT_FALSE (SetupWizardModel::needsOverwriteConfirm (slots));
    slots[0].enabled = true;
    EXPECT_FALSE (SetupWizardModel::needsOverwriteConfirm (slots));
    slots[3].enabled = true;
    EXPECT_TRUE (SetupWizardModel::needsOverwriteConfirm (slots));
}
