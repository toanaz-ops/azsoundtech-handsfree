// Lane U (spec 2026-10-18 section 3.3): the Dumb-mode status table, one test
// per row plus the priority order between rows.

#include <gtest/gtest.h>

#include "gui/DumbStatus.h"

using gui::DumbInputs;
using gui::DumbStatus;
using gui::dumbStatusFor;

namespace
{
juce::String u (const char* s) { return juce::String::fromUTF8 (s); }

DumbInputs protecting()
{
    DumbInputs in;
    in.configured     = true;
    in.autoMode       = true;
    in.measureAllowed = true;
    in.risk           = DumbInputs::Risk::Low;
    return in;
}

bool hasDigitsWithUnit (const juce::String& s)
{
    return s.containsIgnoreCase ("dB") || s.containsIgnoreCase ("Hz");
}
}

TEST (DumbStatus, Row1_NotConfiguredOpensWizardAndOnlyOffIsLive)
{
    DumbInputs in;   // configured = false
    in.measureAllowed = true;
    const auto s = dumbStatusFor (in);
    EXPECT_EQ (s.lamp, DumbStatus::Lamp::Grey);
    EXPECT_TRUE (s.lampOpensWizard);
    EXPECT_FALSE (s.leftEnabled);
    EXPECT_FALSE (s.middleEnabled);
    EXPECT_TRUE (s.offEnabled);
    EXPECT_EQ (s.headline, u ("C\xe1\xba\xa7n c\xc3\xa0i \xc4\x91\xe1\xba\xb7t"));
}

TEST (DumbStatus, Row2_MeasuringTurnsLeftIntoStopAndLocksOffLikeGeek)
{
    auto in  = protecting();
    in.phase = DumbInputs::Phase::Measuring;
    in.measureAllowed = false;   // the console lock is held while running
    const auto s = dumbStatusFor (in);
    EXPECT_EQ (s.left, DumbStatus::Left::Stop);
    EXPECT_TRUE (s.leftEnabled);      // DỪNG must always work
    EXPECT_FALSE (s.middleEnabled);
    EXPECT_FALSE (s.offEnabled);      // Q10: same as Geek's BYPASS
    EXPECT_TRUE (s.pulse);
    EXPECT_FALSE (s.lampOpensWizard);
}

TEST (DumbStatus, Row3_ResultsOfferApplyAndDismiss)
{
    auto in        = protecting();
    in.phase       = DumbInputs::Phase::Results;
    in.resultCount = 3;
    const auto s = dumbStatusFor (in);
    EXPECT_EQ (s.left, DumbStatus::Left::Apply);
    EXPECT_TRUE (s.leftEnabled);
    EXPECT_EQ (s.middle, DumbStatus::Middle::Dismiss);
    EXPECT_TRUE (s.middleEnabled);
    EXPECT_TRUE (s.offEnabled);
    EXPECT_TRUE (s.headline.contains ("3"));
}

TEST (DumbStatus, Row3_NoResultsCannotBeApplied)
{
    auto in  = protecting();
    in.phase = DumbInputs::Phase::Results;
    const auto s = dumbStatusFor (in);
    EXPECT_FALSE (s.leftEnabled);
    EXPECT_EQ (s.middle, DumbStatus::Middle::Dismiss);
}

TEST (DumbStatus, Row4_AppliedKeepsTheReportUntilDismissed)
{
    auto in        = protecting();
    in.phase       = DumbInputs::Phase::Applied;
    in.resultCount = 2;
    const auto s = dumbStatusFor (in);
    EXPECT_FALSE (s.leftEnabled);
    EXPECT_EQ (s.middle, DumbStatus::Middle::Dismiss);
    EXPECT_TRUE (s.offEnabled);
    EXPECT_TRUE (s.headline.contains ("2"));
}

TEST (DumbStatus, Row5_BypassSaysNothingIsProtecting)
{
    auto in     = protecting();
    in.bypassed = true;
    in.autoMode = false;
    const auto s = dumbStatusFor (in);
    EXPECT_EQ (s.lamp, DumbStatus::Lamp::Grey);
    EXPECT_TRUE (s.offOn);
    EXPECT_FALSE (s.middleOn);
    EXPECT_TRUE (s.middleEnabled);
    EXPECT_TRUE (s.leftEnabled);
}

TEST (DumbStatus, Rows6To9_RiskAndNotchCount)
{
    auto in = protecting();

    in.risk = DumbInputs::Risk::Critical;
    EXPECT_EQ (dumbStatusFor (in).lamp, DumbStatus::Lamp::Red);

    in.risk = DumbInputs::Risk::Rising;
    EXPECT_EQ (dumbStatusFor (in).lamp, DumbStatus::Lamp::Amber);

    in.risk = DumbInputs::Risk::Low;
    in.notchCount = 4;
    auto s = dumbStatusFor (in);
    EXPECT_EQ (s.lamp, DumbStatus::Lamp::Green);
    EXPECT_TRUE (s.headline.contains ("4"));

    in.notchCount = 0;
    s = dumbStatusFor (in);
    EXPECT_EQ (s.headline, u ("An to\xc3\xa0n"));
    EXPECT_TRUE (s.middleOn);
    EXPECT_FALSE (s.offOn);
}

TEST (DumbStatus, Row10_UnavailableIsNeverCalledSafe)
{
    auto in = protecting();
    in.risk = DumbInputs::Risk::Unavailable;
    const auto s = dumbStatusFor (in);
    EXPECT_EQ (s.lamp, DumbStatus::Lamp::DimGreen);
    EXPECT_NE (s.headline, u ("An to\xc3\xa0n"));
}

TEST (DumbStatus, PriorityResultsBeatCriticalAndBypassBeatsCritical)
{
    auto in        = protecting();
    in.risk        = DumbInputs::Risk::Critical;
    in.phase       = DumbInputs::Phase::Results;
    in.resultCount = 1;
    EXPECT_EQ (dumbStatusFor (in).lamp, DumbStatus::Lamp::Cyan);

    in.phase    = DumbInputs::Phase::Idle;
    in.bypassed = true;
    in.autoMode = false;
    EXPECT_EQ (dumbStatusFor (in).lamp, DumbStatus::Lamp::Grey);
}

TEST (DumbStatus, PassiveSoundcheckReadsAsProtectingWithEnableNotSelected)
{
    auto in     = protecting();
    in.autoMode = false;   // Mode::Soundcheck
    const auto s = dumbStatusFor (in);
    EXPECT_EQ (s.lamp, DumbStatus::Lamp::Green);
    EXPECT_FALSE (s.middleOn);
    EXPECT_FALSE (s.offOn);
}

TEST (DumbStatus, MeasureFollowsTheGeekPredicateWhenIdle)
{
    auto in = protecting();
    in.measureAllowed = false;   // a confirmation box is open
    EXPECT_FALSE (dumbStatusFor (in).leftEnabled);
}

TEST (DumbStatus, NoRowSpeaksInDecibelsOrHertz)
{
    for (int phase = 0; phase < 4; ++phase)
        for (int risk = 0; risk < 4; ++risk)
            for (int flags = 0; flags < 4; ++flags)
            {
                DumbInputs in;
                in.configured  = (flags & 1) != 0;
                in.bypassed    = (flags & 2) != 0;
                in.autoMode    = ! in.bypassed;
                in.phase       = (DumbInputs::Phase) phase;
                in.risk        = (DumbInputs::Risk) risk;
                in.resultCount = 2;
                in.notchCount  = 2;
                const auto s = dumbStatusFor (in);
                EXPECT_FALSE (hasDigitsWithUnit (s.headline)) << s.headline;
                EXPECT_FALSE (hasDigitsWithUnit (s.subline))  << s.subline;
                EXPECT_FALSE (s.headline.isEmpty());
            }
}

TEST (DumbStatus, AMeasurementOutranksNotConfigured)
{
    // Verification B-1: Geek routes slot 1 only, starts ĐO, switches to Dumb.
    DumbInputs in;
    in.configured = false;
    in.phase      = DumbInputs::Phase::Measuring;
    const auto s = dumbStatusFor (in);
    EXPECT_EQ (s.left, DumbStatus::Left::Stop);
    EXPECT_TRUE (s.leftEnabled);
    EXPECT_FALSE (s.offEnabled);
    EXPECT_FALSE (s.lampOpensWizard);

    in.phase = DumbInputs::Phase::Results;
    in.resultCount = 2;
    EXPECT_EQ (dumbStatusFor (in).left, DumbStatus::Left::Apply);
    EXPECT_EQ (dumbStatusFor (in).middle, DumbStatus::Middle::Dismiss);

    in.phase = DumbInputs::Phase::Applied;
    EXPECT_EQ (dumbStatusFor (in).middle, DumbStatus::Middle::Dismiss);
}
