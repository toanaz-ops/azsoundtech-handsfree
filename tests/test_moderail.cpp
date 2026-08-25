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

//==============================================================================
// Reflecting the ENGINE's mode, added 2026-08-25 with the console rebuild.
//
// The lit lamp on a switch is the primary "what mode am I in" signal in this
// design. Before setDisplayedMode existed the rail only ever showed the last
// click it received, so a freshly-launched window read BYPASSED on the status
// badge with no switch lit at all.

TEST (ModeRail, SetDisplayedModeLightsExactlyOneSwitch)
{
    gui::ModeRail rail;

    rail.setDisplayedMode (gui::ModeRail::Mode::Bypass);
    EXPECT_FALSE (rail.soundcheckButton.getToggleState());
    EXPECT_FALSE (rail.autoButton.getToggleState());
    EXPECT_TRUE  (rail.bypassButton.getToggleState());

    rail.setDisplayedMode (gui::ModeRail::Mode::Auto);
    EXPECT_FALSE (rail.soundcheckButton.getToggleState());
    EXPECT_TRUE  (rail.autoButton.getToggleState());
    EXPECT_FALSE (rail.bypassButton.getToggleState());

    rail.setDisplayedMode (gui::ModeRail::Mode::Soundcheck);
    EXPECT_TRUE  (rail.soundcheckButton.getToggleState());
    EXPECT_FALSE (rail.autoButton.getToggleState());
    EXPECT_FALSE (rail.bypassButton.getToggleState());
}

TEST (ModeRail, SetDisplayedModeNeverRequestsTheModeItIsShowing)
{
    // The whole point of a separate reflect path: it must not loop back into
    // the engine. A rail that re-requested what it was told to display would
    // fight anything else that sets the mode.
    gui::ModeRail rail;

    int soundchecks = 0, autos = 0, bypasses = 0;
    rail.onSoundcheck = [&soundchecks] { ++soundchecks; };
    rail.onAuto       = [&autos]       { ++autos; };
    rail.onBypass     = [&bypasses]    { ++bypasses; };

    rail.setDisplayedMode (gui::ModeRail::Mode::Bypass);
    rail.setDisplayedMode (gui::ModeRail::Mode::Auto);
    rail.setDisplayedMode (gui::ModeRail::Mode::Soundcheck);

    EXPECT_EQ (soundchecks, 0);
    EXPECT_EQ (autos,       0);
    EXPECT_EQ (bypasses,    0);
}
