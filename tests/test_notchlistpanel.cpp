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

void paintHeadless (juce::Component& component, const int width, const int height)
{
    juce::Image image (juce::Image::ARGB, width, height, true);
    juce::Graphics g (image);
    component.setSize (width, height);
    component.paint (g);   // must simply not crash
}

// Same isolation trick as TempLayoutStore in test_gui_wiring.cpp: persistence
// redirected into a scratch directory so a real user's saved layout is never
// read or clobbered.
class TempLayoutStore
{
public:
    TempLayoutStore()
        : directory (juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("HandsFreeNotchListTests_"
                                        + juce::String (juce::Random::getSystemRandom().nextInt())))
    {
        directory.createDirectory();
    }

    ~TempLayoutStore() { directory.deleteRecursively(); }

    juce::PropertiesFile::Options options() const
    {
        juce::PropertiesFile::Options o;
        o.applicationName = "AZ Soundtech Hands-free";
        o.filenameSuffix  = "xml";
        o.folderName      = directory.getFullPathName(); // absolute wins over app-data
        return o;
    }

private:
    juce::File directory;
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
// Parent wiring: L2 hosts the strip as a toggle, L1 pins it to the bottom.
// The panel itself knows nothing about either arrangement (ruling R-1).

TEST (NotchListPanelWiring, PerformanceLayoutHidesTheStripUntilToggled)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    TempLayoutStore store;
    MainComponent app (&store.options());   // L2 default
    ASSERT_EQ (app.getLayout(), gui::ScreenLayout::Performance);

    // Same driver as MinimumSizeKeepsRailAndSpectrumDisjoint: an explicit
    // size guarantees resized() has produced real geometry to reason about.
    // Headless quirk (verified against the whole suite): setSize() alone does
    // NOT deliver the resized() callback to a component with no desktop peer
    // -- every existing MainComponent test gets geometry through setLayout(),
    // which invokes resized() explicitly. Drive it the same way.
    app.setSize (MainComponent::kMinimumWidth, MainComponent::kMinimumHeight);
    app.resized();

    EXPECT_FALSE (app.isNotchListOpen());

    // Visibility is asserted through GEOMETRY rather than an isVisible probe:
    // a JUCE component keeps its last bounds when hidden, so "the strip is
    // out" is proven by the flex row regaining its 120 px.
    const int fullHeight = app.getHeight();

    // Closed: the spectrum row runs to the window bottom -- no strip below.
    EXPECT_EQ (app.spectrumBoundsForTest().getBottom(), fullHeight);

    app.setNotchListOpen (true);
    EXPECT_TRUE (app.isNotchListOpen());
    const auto strip = app.notchListBoundsForTest();
    EXPECT_FALSE (strip.isEmpty());
    EXPECT_EQ (strip.getHeight(), 120);
    EXPECT_EQ (strip.getBottom(), fullHeight);          // pinned at the bottom
    EXPECT_EQ (app.spectrumBoundsForTest().getBottom(), fullHeight - 120);

    app.setNotchListOpen (false);
    EXPECT_FALSE (app.isNotchListOpen());
    EXPECT_EQ (app.spectrumBoundsForTest().getBottom(), fullHeight);
}

TEST (NotchListPanelWiring, ClassicLayoutPinsTheStripToTheWindowBottom)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    TempLayoutStore store;
    MainComponent app (&store.options());
    app.setSize (MainComponent::kMinimumWidth, MainComponent::kMinimumHeight);

    // Even with the L2 toggle CLOSED, Classic must show the fixed strip.
    ASSERT_FALSE (app.isNotchListOpen());
    app.setLayout (gui::ScreenLayout::Classic);
    app.setSize (MainComponent::kMinimumWidth, MainComponent::kMinimumHeight);

    const auto strip = app.notchListBoundsForTest();
    EXPECT_FALSE (strip.isEmpty());
    EXPECT_EQ (strip.getHeight(), 120);
    EXPECT_EQ (strip.getBottom(), MainComponent::kMinimumHeight);
    // The spectrum sits directly above it and nothing runs past the strip.
    EXPECT_EQ (app.spectrumBoundsForTest().getBottom(), strip.getY());

    // Back to Performance with the toggle still closed -> the strip's 120 px
    // return to the spectrum row.
    app.setLayout (gui::ScreenLayout::Performance);
    EXPECT_EQ (app.spectrumBoundsForTest().getBottom(), app.getHeight());
}
