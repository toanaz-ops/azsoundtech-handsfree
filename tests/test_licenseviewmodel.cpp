// Tests for LicenseViewModel: the licence-state-to-UI-decision mapping.
//
// Per this ledger's own audit rule, each test names the production change
// that makes it fail -- a green suite total is not evidence.

#include <gtest/gtest.h>

#include <app/LicenseManager.h>
#include <app/LicenseViewModel.h>

namespace
{

// The production change that breaks screenFor() is any edit that sends a
// state to the wrong screen; every test below pins exactly one edge.

TEST (LicenseViewModel, ActiveStateRunsNormallyWithNoBanner)
{
    // Breaks if Active is routed to ShowActivation (locked-out paying customer)
    // or to Locked.
    const auto screen = license::screenFor (LicenseState::Active);
    EXPECT_EQ (screen.first, license::LicenseScreen::RunNormally);
    EXPECT_TRUE (screen.second.isEmpty());
}

TEST (LicenseViewModel, GraceWarningStillRunsButShowsTheBanner)
{
    // Breaks if GraceWarning is treated as Expired: a day-8 customer must
    // still be able to run a show (that question is D-pending for the owner,
    // but the IMPLEMENTED plan semantics are warn-at-7, block-at-10).
    const auto screen = license::screenFor (LicenseState::GraceWarning);
    EXPECT_EQ (screen.first, license::LicenseScreen::RunWithWarning);
}

TEST (LicenseViewModel, NotActivatedShowsTheActivationScreen)
{
    // Breaks if a fresh install runs unlocked forever.
    EXPECT_EQ (license::screenFor (LicenseState::NotActivated).first,
               license::LicenseScreen::ShowActivation);
}

TEST (LicenseViewModel, ExpiredLocksTheProcessing)
{
    // Breaks if Expired keeps running: plan Task 29's stop-processing edge.
    EXPECT_EQ (license::screenFor (LicenseState::Expired).first,
               license::LicenseScreen::Locked);
}

TEST (LicenseViewModel, ClockTamperingLocksToo)
{
    // Breaks if ClockTampered is merged into an ordinary failure: winding the
    // clock back must never be a way to keep using an expired licence.
    EXPECT_EQ (license::screenFor (LicenseState::ClockTampered).first,
               license::LicenseScreen::Locked);
}

TEST (LicenseViewModel, GraceBannerNamesDaysUsedAndTheLimit)
{
    // Breaks if the banner drops either number: "reactivate soon" without a
    // count cannot tell a soundman whether tonight's show is inside the window.
    const auto banner = license::graceBannerText (8);
    EXPECT_TRUE (banner.contains ("8"));
    EXPECT_TRUE (banner.contains ("10"));
}

TEST (LicenseViewModel, EveryActivationFailureHasItsOwnDistinctMessage)
{
    // Breaks if any two outcomes collapse into one generic string: the whole
    // point of ActivationResult's seven values is that the dialog can say WHICH
    // thing happened instead of "activation failed".
    std::vector<juce::String> messages;
    for (const auto r : { ActivationResult::InvalidKeyFormat,
                          ActivationResult::TransportFailure,
                          ActivationResult::ServerRejected,
                          ActivationResult::MalformedResponse,
                          ActivationResult::MalformedToken,
                          ActivationResult::ExpiredToken })
    {
        const auto msg = license::activationFailureMessage (r);
        EXPECT_FALSE (msg.isEmpty()) << "empty message for outcome index "
                                     << static_cast<int> (r);
        for (const auto& earlier : messages)
            EXPECT_NE (msg, earlier) << "duplicate message for outcome index "
                                     << static_cast<int> (r);
        messages.push_back (msg);
    }
}

} // namespace
