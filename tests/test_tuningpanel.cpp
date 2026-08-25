// TuningPanel tests -- detection tuning brief (2026-08-24).
//
// Same shape as test_slotpanel.cpp: the panel is driven with an injected
// provider/callback pair and sendNotificationSync dispatches inline, so no
// message loop has to be pumped. The panel never touches a controller --
// MainComponent owns what a Params change means (the SlotPanel pattern).

#include <gtest/gtest.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include "gui/TuningPanel.h"

namespace
{
gui::TuningPanel::Params paramsFrom (const juce::ComboBox& rise,
                                     const juce::ComboBox& persist,
                                     const juce::ComboBox& depth,
                                     const juce::ComboBox& q,
                                     const juce::ComboBox& thr)
{
    gui::TuningPanel::Params p;
    p.riseReferenceMs    = gui::TuningPanel::riseMsForId (rise.getSelectedId());
    p.persistenceBlocks  = persist.getSelectedId();
    p.depthDb            = gui::TuningPanel::depthDbForId (depth.getSelectedId());
    p.q                  = gui::TuningPanel::qForId (q.getSelectedId());
    p.peakinessThreshold = gui::TuningPanel::thresholdForId (thr.getSelectedId());
    return p;
}
} // namespace

TEST (TuningPanel, ChangingCombosReportsAllValuesThroughTheCallback)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    gui::TuningPanel panel;
    panel.setSize (720, gui::TuningPanel::kPanelHeight);

    int callbackCount = 0;
    gui::TuningPanel::Params reported;

    panel.onTuningChanged = [&] (const gui::TuningPanel::Params& p)
    {
        ++callbackCount;
        reported = p;
    };

    // Defaults selected without notification must NOT fire the callback.
    panel.getRiseComboForTest().setSelectedId (3, juce::dontSendNotification);   // 250 ms
    EXPECT_EQ (callbackCount, 0);

    // User changes, dispatched synchronously.
    panel.getRiseComboForTest().setSelectedId (4, juce::sendNotificationSync);   // 750 ms
    panel.getPersistComboForTest().setSelectedId (2, juce::sendNotificationSync);
    panel.getDepthComboForTest().setSelectedId (4, juce::sendNotificationSync);  // -24 dB
    panel.getQComboForTest().setSelectedId (2, juce::sendNotificationSync);      // Q 20
    panel.getThrComboForTest().setSelectedId (4, juce::sendNotificationSync);    // 12

    EXPECT_EQ (callbackCount, 5);
    EXPECT_EQ (reported.riseReferenceMs,   750);
    EXPECT_EQ (reported.persistenceBlocks, 2);
    EXPECT_EQ (reported.depthDb,           -24);
    EXPECT_EQ (reported.q,                 20);
    EXPECT_FLOAT_EQ (reported.peakinessThreshold, 12.0f);
}

TEST (TuningPanel, RefreshSelectsProviderValues)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    gui::TuningPanel panel;
    panel.setSize (720, gui::TuningPanel::kPanelHeight);

    panel.paramsProvider = []
    {
        return gui::TuningPanel::Params { 750, 5, -12, 20, 8.0f };
    };
    panel.refresh();

    EXPECT_EQ (panel.getRiseComboForTest().getSelectedId(),    4);   // 750 ms
    EXPECT_EQ (panel.getPersistComboForTest().getSelectedId(), 5);
    EXPECT_EQ (panel.getDepthComboForTest().getSelectedId(),   2);   // -12 dB
    EXPECT_EQ (panel.getQComboForTest().getSelectedId(),       2);   // Q 20
    EXPECT_EQ (panel.getThrComboForTest().getSelectedId(),     2);   // 8
}

TEST (TuningPanel, IdHelpersRoundTripEveryComboItem)
{
    // Every listed choice must survive the id -> value mapping exactly.
    const int rises[] = { 100, 250, 500, 750, 1000 };
    for (int i = 0; i < 5; ++i)
        EXPECT_EQ (gui::TuningPanel::riseMsForId (i + 1), rises[i]) << "rise " << i;

    const int depths[] = { -6, -12, -18, -24 };
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ (gui::TuningPanel::depthDbForId (i + 1), depths[i]) << "depth " << i;

    const int qs[] = { 10, 20, 30, 40, 50 };
    for (int i = 0; i < 5; ++i)
        EXPECT_EQ (gui::TuningPanel::qForId (i + 1), qs[i]) << "q " << i;

    const float thrs[] = { 6.0f, 8.0f, 10.0f, 12.0f, 15.0f };
    for (int i = 0; i < 5; ++i)
        EXPECT_FLOAT_EQ (gui::TuningPanel::thresholdForId (i + 1), thrs[i]) << "thr " << i;
}
