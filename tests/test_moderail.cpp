// ModeRail + StatusBadge headless tests -- GUI console redesign Task 2
// (spec 2026-08-23 section 6 rows "ModeRail phát đúng lệnh" and "CLEAR ALL
// xác nhận").
//
// Same headless pattern as test_gui_wiring.cpp: ScopedJuceInitialiser_GUI
// brings up the MessageManager; components are never added to the desktop;
// clicks are driven with setToggleState(..., sendNotificationSync), which
// JUCE dispatches inline -- triggerClick() POSTS a command message
// (juce_Button.cpp:359-362) and would need a pumped message loop this suite
// deliberately never runs. The CLEAR ALL guard is exercised through an
// injected fake confirm hook (R-3) -- the default native box is never opened
// here, so driving its non-toggling button means invoking its onClick
// directly, which is where the whole guard lives.

#include <gtest/gtest.h>

#include "gui/ModeRail.h"
#include "gui/StatusBadge.h"

#include <vector>

//==============================================================================

TEST (ModeRail, ClickingAutoFiresOnAutoExactlyOnce)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    gui::ModeRail rail (gui::ModeRail::Orientation::Vertical);
    int fired = 0;
    rail.onAuto = [&fired] { ++fired; };

    rail.autoButton.setToggleState (true, juce::sendNotificationSync);

    EXPECT_EQ (fired, 1);
}

TEST (ModeRail, ClickingSoundcheckAndBypassFireTheirCallbacks)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    gui::ModeRail rail (gui::ModeRail::Orientation::Horizontal);
    int soundchecks = 0, bypasses = 0;
    rail.onSoundcheck = [&soundchecks] { ++soundchecks; };
    rail.onBypass = [&bypasses] { ++bypasses; };

    rail.soundcheckButton.setToggleState (true, juce::sendNotificationSync);
    rail.bypassButton.setToggleState (true, juce::sendNotificationSync);

    EXPECT_EQ (soundchecks, 1);
    EXPECT_EQ (bypasses, 1);
}

TEST (ModeRail, ClearAllFiresNoCallbackBeforeConfirmation)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    gui::ModeRail rail;
    int confirmationsAsked = 0;
    std::function<void (bool)> pending;

    // Fake hook: captures the decision callback instead of showing a box.
    rail.confirmHook = [&confirmationsAsked, &pending] (std::function<void (bool)> decide)
    {
        ++confirmationsAsked;
        pending = std::move (decide);
    };

    int cleared = 0;
    rail.onClearAllConfirmed = [&cleared] { ++cleared; };

    // CLEAR ALL is not a toggling button, so the sendNotificationSync trick
    // does not apply: invoke its onClick directly. triggerClick() would post
    // a command message no headless loop ever dispatches. Everything under
    // test -- the guard, the hook, the single confirmed callback -- lives in
    // that handler.
    rail.clearAllButton.onClick();

    // Nothing cleared yet -- the hook was asked, nothing more.
    EXPECT_EQ (confirmationsAsked, 1);
    EXPECT_EQ (cleared, 0);

    // The user says no.
    ASSERT_TRUE (pending != nullptr);
    pending (false);
    EXPECT_EQ (cleared, 0);

    // A second click asks again and a yes clears exactly once.
    rail.clearAllButton.onClick();
    EXPECT_EQ (confirmationsAsked, 2);
    EXPECT_EQ (cleared, 0);

    ASSERT_TRUE (pending != nullptr);
    pending (true);
    EXPECT_EQ (cleared, 1);
}

TEST (ModeRail, DoubleClickWhileConfirmPendingAsksAndFiresOnlyOnce)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    gui::ModeRail rail;
    int confirmationsAsked = 0;
    std::function<void (bool)> pending;

    rail.confirmHook = [&confirmationsAsked, &pending] (std::function<void (bool)> decide)
    {
        ++confirmationsAsked;
        pending = std::move (decide);
    };

    int cleared = 0;
    rail.onClearAllConfirmed = [&cleared] { ++cleared; };

    // Two clicks while the first dialog is still unanswered: the second must
    // be dropped instead of stacking a second confirm.
    rail.clearAllButton.onClick();
    rail.clearAllButton.onClick();

    EXPECT_EQ (confirmationsAsked, 1);
    EXPECT_EQ (cleared, 0);

    ASSERT_TRUE (pending != nullptr);
    pending (true);

    // Exactly one unanswered ask -> exactly one confirmed callback.
    EXPECT_EQ (cleared, 1);
}

TEST (ModeRail, ConfirmCallbackArrivingAfterTheRailWasDestroyedIsHarmless)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    int confirmationsAsked = 0;
    std::function<void (bool)> pending;

    {
        gui::ModeRail rail;
        rail.confirmHook = [&confirmationsAsked, &pending] (std::function<void (bool)> decide)
        {
            ++confirmationsAsked;
            pending = std::move (decide);
        };
        int cleared = 0;
        rail.onClearAllConfirmed = [&cleared] { ++cleared; };

        rail.clearAllButton.onClick();
        EXPECT_EQ (cleared, 0);
    }
    // The rail is gone -- the "dialog" it opened is still pending.

    ASSERT_TRUE (pending != nullptr);
    // Must not resurrect the rail or touch freed memory: with a raw `this`
    // capture this line is use-after-free; with the SafePointer fix it is a
    // silent no-op. Either way the callback's own captured state (cleared)
    // is never reached.
    pending (true);
    SUCCEED();
}

TEST (ModeRail, CountdownLabelShowsRemainingSecondsWhileAboveZero)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    gui::ModeRail rail;
    double remainingMs = 7000.0;
    rail.getSoundcheckRemainingMs = [&remainingMs] { return remainingMs; };

    rail.updateCountdown();
    EXPECT_EQ (rail.countdownLabel.getText(), juce::String ("SOUNDCHECK 7s"));

    remainingMs = 0.0;
    rail.updateCountdown();
    EXPECT_TRUE (rail.countdownLabel.getText().isEmpty());
}

TEST (ModeRail, ListToggleFiresOnToggleNotchListWithItsNewState)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    gui::ModeRail rail;
    std::vector<bool> reported;
    rail.onToggleNotchList = [&reported] (bool open) { reported.push_back (open); };

    // Same inline-dispatch trick as the mode cells: setToggleState with
    // sendNotificationSync runs onClick synchronously, no message pump.
    rail.listToggleButton.setToggleState (true, juce::sendNotificationSync);
    rail.listToggleButton.setToggleState (false, juce::sendNotificationSync);

    EXPECT_EQ (reported, (std::vector<bool> { true, false }));
}

TEST (ModeRail, SetListToggleVisibleRemovesTheCellAndRelayouts)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    gui::ModeRail rail;
    ASSERT_TRUE (rail.listToggleButton.isVisible());

    rail.setListToggleVisible (false);

    EXPECT_FALSE (rail.listToggleButton.isVisible());
    // The parent re-ran resized(): a hidden-but-positioned cell would leave
    // a ghost rectangle in the rail's flex flow.
    EXPECT_TRUE (rail.listToggleButton.getBounds().isEmpty());

    rail.setListToggleVisible (true);
    EXPECT_TRUE (rail.listToggleButton.isVisible());
}

//==============================================================================

TEST (StatusBadge, SetStateIsReflectedInGetState)
{
    gui::StatusBadge badge;

    EXPECT_EQ (badge.getState(), gui::ProtectionState::Idle);

    badge.setState (gui::ProtectionState::Protecting);
    EXPECT_EQ (badge.getState(), gui::ProtectionState::Protecting);

    badge.setState (gui::ProtectionState::Bypassed);
    EXPECT_EQ (badge.getState(), gui::ProtectionState::Bypassed);
}
