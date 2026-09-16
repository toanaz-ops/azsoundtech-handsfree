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
#include "gui/theme/AzTheme.h"

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

    // The label holds the NUMBER only; the word SOUNDCHECK is painted above it
    // as a caption, because one Label cannot be two type sizes and the number
    // is the part read across a room.
    EXPECT_EQ (rail.countdownLabel.getText(), juce::String ("7 s"));

    remainingMs = 0.0;
    rail.updateCountdown();

    // Not empty: an em dash. A readout cell that blanks itself reads as a
    // control that disappeared, rather than as one with nothing to report.
    EXPECT_EQ (rail.countdownLabel.getText(),
               juce::String::charToString ((juce::juce_wchar) 0x2014));
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

//==============================================================================
// Lane M Task 9 -- the DO button.
//
// EVERY Vietnamese literal below is written as EXPLICIT UTF-8 BYTES, never as
// a source-file literal. This build passes no /utf-8 to MSVC and only ONE of
// the files involved carries a BOM, so a raw "\u0110O" in a source file is
// decoded with whatever the machine's active codepage happens to be. That is
// exactly the mojibake that shipped the middle-dot bug
// (memory/ui-rebuild-sodium-rack-2026-08-25.md); src/gui/DeviceViewModel.cpp:13
// is the precedent this follows.
//
// The bytes here are written out INDEPENDENTLY of the ones in src/. If the two
// were a shared helper these tests could not fail on a wrong label, which is
// the whole point of them.

TEST (ModeRail, MeasureButtonHasItsLabel)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::ModeRail rail (gui::ModeRail::Orientation::Vertical);

    // "DO" with a crossed D: U+0110 U+004F.
    EXPECT_EQ (rail.measureButton.getButtonText(), juce::String::fromUTF8 ("\xc4\x90O"));
    EXPECT_NE (rail.measureButton.getButtonText(), rail.soundcheckButton.getButtonText());

    // RED IF the button is constructed as TextButton(name, tooltip). In JUCE 9
    // the SECOND argument of that two-argument constructor is NOT the tooltip,
    // so the button renders with NO TEXT AT ALL -- and the build stays green.
    // This is memory/juce9-api-traps-2026-08-25.md, and it shipped once
    // already. The explicit emptiness check says so out loud: the EXPECT_EQ
    // above already covers it, but a future edit that loosens the comparison
    // must still trip over this one.
    EXPECT_FALSE (rail.measureButton.getButtonText().isEmpty());
}

TEST (ModeRail, MeasureIsItsOwnButtonAndItsOwnCallback)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::ModeRail rail (gui::ModeRail::Orientation::Vertical);

    int measures = 0, soundchecks = 0;
    rail.onMeasure    = [&measures]    { ++measures; };
    rail.onSoundcheck = [&soundchecks] { ++soundchecks; };

    // onClick(), not triggerClick(): triggerClick is async and the headless
    // suite pumps no message loop (memory/data-loop-lessons-2026-09-05.md).
    rail.measureButton.onClick();
    EXPECT_EQ (measures, 1);
    EXPECT_EQ (soundchecks, 0);

    // Q16: DO is a MOMENTARY action, not a fourth latching mode. If it ever
    // joined the radio group, pressing it would silently un-light whichever
    // mode the engine is actually in -- the lamp would lie about the engine.
    EXPECT_FALSE (rail.measureButton.getClickingTogglesState());
    EXPECT_EQ    (rail.measureButton.getRadioGroupId(), 0);
}

TEST (ModeRail, MeasureButtonIsInsideTheRail)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::ModeRail rail (gui::ModeRail::Orientation::Vertical);
    rail.setSize (140, 620);
    rail.resized();   // JUCE headless setSize() has no peer, so resized() must be
                      // called by hand (memory/gui-console-lessons-2026-08-24.md)

    EXPECT_TRUE (rail.getLocalBounds().contains (rail.measureButton.getBounds()));
    EXPECT_GT (rail.measureButton.getWidth(), 0);
    EXPECT_GT (rail.measureButton.getHeight(), 0);
    EXPECT_FALSE (rail.measureButton.getBounds().intersects (rail.soundcheckButton.getBounds()));
    EXPECT_FALSE (rail.measureButton.getBounds().intersects (rail.clearAllButton.getBounds()));
}

TEST (ModeRail, MeasureButtonIsWideEnoughForItsOwnLegendInTheHorizontalRail)
{
    // RED IF the cell is sized by eyeballing instead of by measuring. The rail
    // is horizontal in the only shipping layout, and a cell narrower than its
    // legend does not fail a test -- it ships a truncated button
    // (memory/stereo-lane-lessons-2026-09-05.md: measure with the REAL font
    // and the widest string the formatter can print).
    const juce::ScopedJuceInitialiser_GUI juceInit;
    az::theme::AzLookAndFeel lnf;
    juce::LookAndFeel::setDefaultLookAndFeel (&lnf);

    gui::ModeRail rail (gui::ModeRail::Orientation::Horizontal);
    rail.setSize (1360, az::theme::transportHeight);
    rail.resized();

    const auto legend = rail.measureButton.getButtonText().toUpperCase();
    const auto font   = az::theme::legendFont (az::theme::switchFontSize, true,
                                               az::theme::trackingSwitch);

    EXPECT_GE ((float) rail.measureButton.getWidth(),
               az::theme::stringWidth (font, legend) + 2.0f * (float) az::theme::gap);

    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
}

TEST (ModeRail, LockingTheRailDisablesEveryControlThatCouldDisturbARun)
{
    // F10: while a measurement is in flight, SOUNDCHECK / AUTO / BYPASS and
    // CLEAR ALL must not be reachable -- each of them changes what the filter
    // chain is doing underneath a run that is measuring it. DO itself is
    // locked through its own setter, because it is also the control that must
    // stay lit-but-dead in Results.
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::ModeRail rail (gui::ModeRail::Orientation::Horizontal);

    rail.setModeControlsEnabled (false);
    EXPECT_FALSE (rail.soundcheckButton.isEnabled());
    EXPECT_FALSE (rail.autoButton.isEnabled());
    EXPECT_FALSE (rail.bypassButton.isEnabled());
    EXPECT_FALSE (rail.clearAllButton.isEnabled());
    EXPECT_TRUE  (rail.measureButton.isEnabled());   // its own switch, not this one

    rail.setMeasureEnabled (false);
    EXPECT_FALSE (rail.measureButton.isEnabled());

    rail.setModeControlsEnabled (true);
    rail.setMeasureEnabled (true);
    EXPECT_TRUE (rail.soundcheckButton.isEnabled());
    EXPECT_TRUE (rail.clearAllButton.isEnabled());
    EXPECT_TRUE (rail.measureButton.isEnabled());
}
