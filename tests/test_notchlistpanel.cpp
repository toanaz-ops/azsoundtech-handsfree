// NotchListPanel tests -- Task 4 of the GUI Console redesign (spec sections
// 2, 5, 6).
//
// Same patterns as the sibling GUI tests:
//   - test_gui_wiring.cpp: juce::ScopedJuceInitialiser_GUI brings up the
//     MessageManager; components are built headless, never added to a
//     desktop; no message loop is pumped (JUCE_MODAL_LOOPS_PERMITTED off),
//     which also means the panel's internal Timer NEVER fires here -- every
//     state below is produced by an explicit refreshFromSnapshot().
//   - test_spectrumview.cpp: the snapshot comes from a REAL NotchController
//     driven by synchronous runOnce() calls, so what reaches the panel is
//     the genuinely published struct.
//
// Determinism: the panel's clock is injected (ruling R-2 -- age is computed
// GUI-side from a steady_clock first-seen per notch identity). Tests drive a
// controllable fake instead of wall time.

#include <gtest/gtest.h>

#include "app/MainComponent.h"
#include "app/NotchController.h"
#include "dsp/ClockSource.h"
#include "dsp/Detector.h"
#include "dsp/LockFreeRingBuffer.h"
#include "gui/NotchListPanel.h"
#include "gui/theme/AzTheme.h"
#include "test_gui_helpers.h"

using gui_test::paintHeadless;

#include <cmath>
#include <cstdint>
#include <vector>

namespace
{

// Controllable clock: returns ms, advanced by hand.
struct FakeClock
{
    double nowMs = 0.0;
    double operator()() const { return nowMs; }
};

// A controller fed two manual notches (987 Hz on ch0/slot0, 2.4 kHz on
// ch1/slot3) plus audio blocks, pumped until it published real frames.
struct TwoNotchController
{
    LockFreeRingBuffer<float> tap { 8192 };
    LockFreeRingBuffer<NotchCommand> commands { 128 };
    JuceMonotonicClock clock;
    NotchController controller { tap, commands, clock };

    TwoNotchController()
    {
        controller.setNotch (0, 0, 987.0, 4.0, -12.0,
                             NotchController::Origin::Manual);
        controller.setNotch (1, 3, 2400.0, 8.0, -6.0,
                             NotchController::Origin::Manual);

        std::vector<float> hop ((std::size_t) Detector::kHopSize);
        for (std::size_t i = 0; i < hop.size(); ++i)
            hop[i] = 0.1f;

        for (int block = 0; block < 8; ++block)
        {
            tap.write (hop.data(), hop.size());
            controller.runOnce();
        }
    }

    // Publishes a fresh frame reflecting the current model.
    void republish()
    {
        std::vector<float> hop ((std::size_t) Detector::kHopSize, 0.1f);
        tap.write (hop.data(), hop.size());
        controller.runOnce();
    }
};

} // namespace

//==============================================================================
// Formatting contract (binding): "987 Hz" / "2.4 kHz", "-12.0 dB", "4.0",
// ages "12s" / "2m ago".

TEST (NotchListPanelFormatting, FrequencyBelowOneKilohertzUsesHertz)
{
    EXPECT_EQ (gui::NotchListPanel::formatFrequency (987.0f).toStdString(), "987 Hz");
}

TEST (NotchListPanelFormatting, FrequencyAtOrAboveOneKilohertzUsesKilohertz)
{
    EXPECT_EQ (gui::NotchListPanel::formatFrequency (2400.0f).toStdString(), "2.4 kHz");
    EXPECT_EQ (gui::NotchListPanel::formatFrequency (1000.0f).toStdString(), "1.0 kHz");
}

// Boundary: 999.5 Hz ROUNDS to 1000, so it must render through the kHz
// path as "1.0 kHz" -- not as "1000 Hz" from inside the Hz branch.
TEST (NotchListPanelFormatting, FrequencyRoundingUpToOneKilohertzUsesKilohertz)
{
    EXPECT_EQ (gui::NotchListPanel::formatFrequency (999.5f).toStdString(), "1.0 kHz");
    EXPECT_EQ (gui::NotchListPanel::formatFrequency (999.4f).toStdString(), "999 Hz");
}

TEST (NotchListPanelFormatting, DepthUsesTheTypographicMinusAndOneDecimal)
{
    const juce::String expected
        = juce::String::charToString ((juce::juce_wchar) 0x2212) + "12.0 dB";
    EXPECT_EQ (gui::NotchListPanel::formatDepthDb (-12.0f).toStdString(),
               expected.toStdString());
}

TEST (NotchListPanelFormatting, QUsesOneDecimal)
{
    EXPECT_EQ (gui::NotchListPanel::formatQ (4.0f).toStdString(), "4.0");
}

TEST (NotchListPanelFormatting, AgeUnderAMinuteCountsSecondsOverAMinutesMinutesAgo)
{
    EXPECT_EQ (gui::NotchListPanel::formatAgeMs (0.0).toStdString(), "0s");
    EXPECT_EQ (gui::NotchListPanel::formatAgeMs (12000.0).toStdString(), "12s");
    EXPECT_EQ (gui::NotchListPanel::formatAgeMs (59999.0).toStdString(), "59s");
    EXPECT_EQ (gui::NotchListPanel::formatAgeMs (60000.0).toStdString(), "1m ago");
    EXPECT_EQ (gui::NotchListPanel::formatAgeMs (120000.0).toStdString(), "2m ago");
}

//==============================================================================
// Model behaviour against a REAL published snapshot.

// Spec test 19. Red if the LANE column stops reading L for channel 0 and R
// for channel 1.
TEST (NotchListPanel, LaneColumnReadsLOrRPerChannel)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    LockFreeRingBuffer<float> tapL { 8192 }, tapR { 8192 };
    LockFreeRingBuffer<NotchCommand> commands { 128 };
    JuceMonotonicClock clock;
    NotchController controller { tapL, &tapR, commands, clock };
    ASSERT_TRUE (controller.setNotch (0, 0, 1000.0, 30.0, -12.0, NotchController::Origin::Manual));
    ASSERT_TRUE (controller.setNotch (1, 3, 2000.0, 30.0, -12.0, NotchController::Origin::Manual));
    std::vector<float> hop (Detector::kHopSize, 0.01f);
    tapL.write (hop.data(), hop.size()); tapR.write (hop.data(), hop.size());
    for (int i = 0; i < 5; ++i) { tapL.write (hop.data(), hop.size()); tapR.write (hop.data(), hop.size()); controller.runOnce(); }

    gui::NotchListPanel panel (controller, [] { return 0.0; });
    panel.refreshFromSnapshot();
    ASSERT_EQ (panel.rowCountForTest(), 2);

    bool sawL = false, sawR = false;
    for (int i = 0; i < panel.rowCountForTest(); ++i)
    {
        const auto row = panel.rowForTest (i);
        if (row.freq == "1000 Hz" || row.freq == "1.0 kHz")
            EXPECT_EQ (row.lane.toStdString(), "L");
        if (row.freq == "2.0 kHz")
            EXPECT_EQ (row.lane.toStdString(), "R");
        if (row.lane.toStdString() == "L") sawL = true;
        if (row.lane.toStdString() == "R") sawR = true;
    }
    EXPECT_TRUE (sawL);
    EXPECT_TRUE (sawR);
}

TEST (NotchListPanel, TwoPublishedNotchesProduceExactlyTwoRows)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    TwoNotchController fed;
    NotchController::SnapshotBuffer snap {};
    fed.controller.copySnapshot (snap);
    ASSERT_EQ (snap.notchCount, 2u);   // the source really has two

    FakeClock clock;
    gui::NotchListPanel panel (fed.controller, clock);

    panel.refreshFromSnapshot();

    ASSERT_EQ (panel.rowCountForTest(), 2);

    bool saw987 = false;
    bool saw2400 = false;
    for (int i = 0; i < panel.rowCountForTest(); ++i)
    {
        const auto row = panel.rowForTest (i);
        if (row.freq == "987 Hz")
        {
            saw987 = true;
            EXPECT_EQ (row.depth.toStdString(),
                       (juce::String::charToString ((juce::juce_wchar) 0x2212)
                        + "12.0 dB").toStdString());
            EXPECT_EQ (row.q.toStdString(), "4.0");
        }
        if (row.freq == "2.4 kHz")
            saw2400 = true;
    }
    EXPECT_TRUE (saw987);
    EXPECT_TRUE (saw2400);
}

TEST (NotchListPanel, AgeIsComputedFromFirstSightingNotWallTime)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    TwoNotchController fed;
    FakeClock clock;
    clock.nowMs = 10000.0;

    // Capture BY REFERENCE: ClockFn copies its callable, so passing `clock`
    // by value would freeze the panel on a snapshot of nowMs.
    gui::NotchListPanel panel (fed.controller,
                               [&clock] { return clock.nowMs; });
    panel.refreshFromSnapshot();
    ASSERT_EQ (panel.rowCountForTest(), 2);
    EXPECT_EQ (panel.rowForTest (0).status.toStdString(), "0s");

    // Twelve seconds of LIVE time later, same notch identities: the age must
    // read from the panel's own first-seen record.
    clock.nowMs = 22000.0;
    panel.refreshFromSnapshot();

    ASSERT_EQ (panel.rowCountForTest(), 2);
    EXPECT_EQ (panel.rowForTest (0).status.toStdString(), "12s");
    EXPECT_EQ (panel.rowForTest (1).status.toStdString(), "12s");

    paintHeadless (panel, 420, 120);
}

TEST (NotchListPanel, IdentityUnseenPastTheTimeoutLosesItsAgeHistory)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    TwoNotchController fed;
    FakeClock clock;
    clock.nowMs = 1000.0;

    gui::NotchListPanel panel (fed.controller,
                               [&clock] { return clock.nowMs; });
    panel.refreshFromSnapshot();
    ASSERT_EQ (panel.rowCountForTest(), 2);

    // Notch A disappears...
    fed.controller.clearNotch (0, 0);
    clock.nowMs = 1000.0 + gui::NotchListPanel::kTrackingTimeoutMs + 30000.0;
    fed.republish();
    panel.refreshFromSnapshot();
    ASSERT_EQ (panel.rowCountForTest(), 1);   // only the 2.4 kHz one left

    // ...and the SAME identity returns long after the tracking timeout: its
    // age must restart, not resume from the pre-drop first-seen.
    fed.controller.setNotch (0, 0, 987.0, 4.0, -12.0,
                             NotchController::Origin::Manual);
    fed.republish();
    panel.refreshFromSnapshot();

    ASSERT_EQ (panel.rowCountForTest(), 2);
    EXPECT_EQ (panel.rowForTest (0).status.toStdString(), "0s");
}

TEST (NotchListPanel, EmptyStatePaintsWithoutCrashing)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    LockFreeRingBuffer<float> tap { 8192 };
    LockFreeRingBuffer<NotchCommand> commands { 128 };
    JuceMonotonicClock clock;
    NotchController idle { tap, commands, clock };

    gui::NotchListPanel panel (idle);
    panel.refreshFromSnapshot();

    EXPECT_EQ (panel.rowCountForTest(), 0);
    paintHeadless (panel, 420, 120);
}

//==============================================================================
// Parent wiring. REWRITTEN 2026-08-25 with the console rebuild: the notch
// table used to be a full-width 120 px strip pinned across the window bottom.
// It is now the LEFT COLUMN of the bottom floor, sitting beside the rig
// controls, because the analyser needed the vertical space the old stack of
// seven equal-weight full-width bands was spending on chrome.
//
// The contract that survived the move is asserted structurally rather than by
// pixel count, so the next layout change fails only if it breaks something
// that matters: the table is always visible, it never overlaps the analyser,
// it stays inside the window, and laying out twice is idempotent.

TEST (NotchListPanelWiring, TableIsTheFloorsLeftColumnAndNeverOverlapsTheAnalyser)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    MainComponent app;

    // Same driver as MinimumSizeKeepsRailAndSpectrumDisjoint: an explicit
    // size guarantees resized() has produced real geometry to reason about.
    // Headless quirk (verified against the whole suite): setSize() alone does
    // NOT deliver the resized() callback to a component with no desktop peer
    // -- drive the layout pass directly.
    app.setSize (MainComponent::kMinimumWidth, MainComponent::kMinimumHeight);
    app.resized();

    const auto strip    = app.notchListBoundsForTest();
    const auto spectrum = app.spectrumBoundsForTest();

    EXPECT_FALSE (strip.isEmpty());

    // A COLUMN, not a band: the rig controls occupy the rest of the floor
    // beside it, so it never reaches the window's right edge.
    EXPECT_LT (strip.getRight(), MainComponent::kMinimumWidth);

    // NEITHER column dominates. The design study gave the notch table the
    // wider share, but in the running app that column is only ever as tall as
    // the number of notches -- extra WIDTH there buys nothing, while the rig
    // column holds a routing table that was scrolling sideways beside it. The
    // width went where the controls are (owner, 2026-08-25).
    //
    // Asserted as a BAND rather than a figure: the exact split is a judgement
    // that may move again, but a floor where one column has swallowed the
    // other is a defect either way.
    EXPECT_GT (strip.getWidth(), juce::roundToInt (MainComponent::kMinimumWidth * 0.34));
    EXPECT_LT (strip.getWidth(), juce::roundToInt (MainComponent::kMinimumWidth * 0.56));

    // Wholly inside the window, at its bottom.
    EXPECT_GE (strip.getX(), 0);
    EXPECT_LE (strip.getBottom(), MainComponent::kMinimumHeight);

    // It sits BELOW the analyser and the two never overlap -- the claim the
    // old pixel-exact assertion was really making.
    EXPECT_FALSE (strip.intersects (spectrum));
    EXPECT_GE (strip.getY(), spectrum.getBottom());

    // And the analyser is the thing that got the space: at the minimum window
    // size it is still taller than the floor column beside the rig.
    EXPECT_GT (spectrum.getHeight(), 0);

    // A second pass (the old toggle path used to re-run resized()) leaves the
    // same geometry: there is no open/closed state left to change it.
    app.resized();
    EXPECT_EQ (app.notchListBoundsForTest(), strip);
}

//==============================================================================
// Column budget (review finding on Task 8): the header's guarantee that
// every column fits its widest realistic cell at the narrowest sensible
// panel (~360 px), measured against the REAL faces the panel paints with --
// not guessed, and not the 13 px the header comment used to (wrongly) claim.
//
// Red if any column constant is narrowed below its widest cell, or if a
// formatter starts producing a wider string, at the panel's real font.
TEST (NotchListPanel, EveryColumnFitsItsWidestCellAtTheNarrowestPanel)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    // Same call NotchListPanel's tableFont_ is built with -- one definition
    // of "the real face", shared rather than re-derived here.
    const auto tableFont = az::theme::monoFont();

    auto widthOf = [&] (const juce::String& s)
    {
        return juce::GlyphArrangement::getStringWidth (tableFont, s);
    };

    constexpr float kInset = 4.0f;   // the right-margin every column paints with
    const juce::String minus = juce::String::charToString ((juce::juce_wchar) 0x2212);

    // # / LANE: a 2-digit index, a single lane glyph.
    EXPECT_LE (widthOf ("32") + kInset, gui::NotchListPanel::kColIdW);
    EXPECT_LE (widthOf ("L") + kInset, gui::NotchListPanel::kColLaneW);
    EXPECT_LE (widthOf ("R") + kInset, gui::NotchListPanel::kColLaneW);

    // FREQ: detection runs to Nyquist (24 kHz at 48 kHz sample rate, 48 kHz
    // at 96 kHz), so a two-digit-kHz reading is reachable, not just the
    // "2.4 kHz" design-plan example this test used to pin. Assert against
    // the ACTUAL formatter output, not a literal, so a format change that
    // widens the string fails this test too.
    const auto widestFreq = gui::NotchListPanel::formatFrequency (23900.0f);
    ASSERT_EQ (widestFreq.toStdString(), "23.9 kHz");   // sanity: this IS the 8-char case
    const float freqPrefix = gui::NotchListPanel::kDotGap + gui::NotchListPanel::kDotSize;
    EXPECT_LE (freqPrefix + widthOf (widestFreq) + kInset, gui::NotchListPanel::kColFreqW);

    // DEPTH: PresetManager only enforces depthDB <= 0 -- no floor -- so a
    // hand-edited preset can carry a three-digit magnitude. Assert against
    // the ACTUAL formatter output for the same reason as FREQ above.
    const auto widestDepth = gui::NotchListPanel::formatDepthDb (-150.0f);
    ASSERT_EQ (widestDepth.toStdString(), (minus + "150.0 dB").toStdString());
    EXPECT_LE (widthOf (widestDepth) + kInset, gui::NotchListPanel::kColDepthW);

    // Q: TuningPanel::kQChoices tops out at 50, but sourced from the
    // formatter (99.9, same 4-char width as the old "50.0" literal under
    // this monospace face) so a format change is caught here too.
    const auto widestQ = gui::NotchListPanel::formatQ (99.9f);
    EXPECT_LE (widthOf (widestQ) + kInset, gui::NotchListPanel::kColQW);

    // STATUS/HELD: formatAgeMs() is uncapped -- age keeps counting across
    // re-sightings, so "127m ago" (8 chars) is reachable in a long show.
    // Assert against the ACTUAL formatter output, not a literal, so a format
    // change that widens the string fails this test too.
    const auto widestAge = gui::NotchListPanel::formatAgeMs (127.0 * 60.0 * 1000.0);
    ASSERT_EQ (widestAge.toStdString(), "127m ago");   // sanity: this IS the 8-char case
    const float statusW360 = gui::NotchListPanel::statusWidthFor (360.0f);
    EXPECT_LE (widthOf (widestAge) + kInset, statusW360);
}
