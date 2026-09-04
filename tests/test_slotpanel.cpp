// SlotPanel tests -- Task 7 (spec section 7 / G-3).
//
// The panel is driven with light injected fakes (the provider hooks it
// exposes) plus a real AudioEngine with no device open -- the state every
// test machine and every app startup is actually in. sendNotificationSync
// dispatches inline (see test_gui_wiring.cpp's header note), so no message
// loop has to be pumped.

#include <gtest/gtest.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include "app/AudioEngine.h"
#include "gui/SlotPanel.h"
#include "gui/theme/AzTheme.h"

#include <array>

namespace
{
struct FakeChannels
{
    juce::StringArray ins  { "In 1", "In 2" };
    juce::StringArray outs { "Out 1", "Out 2" };
};

// A config store standing in for the engine: the callback writes through it,
// so refresh() reads back exactly what was reported -- the same shape
// MainComponent's real wiring has.
struct ConfigStore
{
    std::array<SlotConfig, kMaxSlots> slots {};

    SlotConfig get (int i) const { return slots[(std::size_t) i]; }
    void set (int i, const SlotConfig& c) { slots[(std::size_t) i] = c; }
};
} // namespace

TEST (SlotPanel, WithNoDeviceTheChannelCombosAreEmptyAndDisabled)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;
    gui::SlotPanel panel (engine);
    panel.setSize (520, 260);

    for (int i = 0; i < kMaxSlots; ++i)
    {
        const auto& row = panel.getRowForTest (i);

        EXPECT_FALSE (row.width.isEnabled()) << "row " << i;
        EXPECT_EQ (0, row.inLanes[0].getNumItems())  << "row " << i;
        EXPECT_FALSE (row.inLanes[0].isEnabled())    << "row " << i;
        EXPECT_EQ (0, row.outLanes[0].getNumItems()) << "row " << i;
        EXPECT_FALSE (row.outLanes[0].isEnabled())   << "row " << i;
    }
}

TEST (SlotPanel, TogglingAReportsTheRightSlotAndConfigThroughTheCallback)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;
    gui::SlotPanel panel (engine);

    ConfigStore store;

    // Default SlotConfig is stereo (width 2), channels {0,1}.
    panel.slotConfigProvider       = [&store] (int i) { return store.get (i); };
    panel.inputChannelNamesProvider = [] { return juce::StringArray { "In 1", "In 2" }; };
    panel.outputChannelNamesProvider = [] { return juce::StringArray { "Out 1", "Out 2" }; };

    int reportedSlot = -1;
    SlotConfig reported;

    panel.onSlotConfigChanged = [&] (int slotIndex, const SlotConfig& config)
    {
        reportedSlot = slotIndex;
        reported     = config;
        store.set (slotIndex, config);   // MainComponent's half of the contract
        panel.refresh();
    };

    panel.refresh();

    auto& row = panel.getRowForTest (3);
    row.enable.setToggleState (true, juce::sendNotificationSync);

    EXPECT_EQ (3, reportedSlot);
    EXPECT_TRUE  (reported.enabled);
    EXPECT_EQ (2, reported.width);
}

TEST (SlotPanel, StereoShowsBothLaneCombosMonoHidesTheSecond)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;
    gui::SlotPanel panel (engine);

    ConfigStore store;
    store.slots[0].width = 1;   // mono row

    panel.slotConfigProvider         = [&store] (int i) { return store.get (i); };
    panel.inputChannelNamesProvider  = [] { return juce::StringArray { "In 1", "In 2" }; };
    panel.outputChannelNamesProvider = [] { return juce::StringArray { "Out 1", "Out 2" }; };

    panel.setSize (520, 260);
    panel.refresh();

    EXPECT_FALSE (panel.getRowForTest (0).inLanes[1].isVisible());
    EXPECT_FALSE (panel.getRowForTest (0).outLanes[1].isVisible());

    EXPECT_TRUE (panel.getRowForTest (1).inLanes[1].isVisible());
    EXPECT_TRUE (panel.getRowForTest (1).outLanes[1].isVisible());
}

//==============================================================================
// LINK / INDEP control (Task 7 / spec section 7 continuation): a per-row
// segmented control, shown only on stereo rows, that reports which ring-risk
// policy applies to that slot -- LINK cuts both channels when either rings,
// INDEP cuts only the ringing side.

TEST (SlotPanel, LinkControlFollowsWidthAndReportsClicks)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    gui::SlotPanel panel (engine);
    panel.inputChannelNamesProvider  = [] { return juce::StringArray { "In 1", "In 2" }; };
    panel.outputChannelNamesProvider = [] { return juce::StringArray { "Out 1", "Out 2" }; };
    panel.slotConfigProvider = [] (int slot)
    {
        SlotConfig c; c.enabled = true; c.width = (slot == 0) ? 1 : 2; return c;
    };
    panel.slotLinkedProvider = [] (int slot) { return slot == 1; };
    panel.refresh();
    panel.setSize (700, 400);

    EXPECT_FALSE (panel.getRowForTest (0).link.isVisible());
    EXPECT_TRUE  (panel.getRowForTest (1).link.isVisible());
    EXPECT_EQ (panel.getRowForTest (1).link.getSelectedIndex(), 0);   // LINK
    EXPECT_EQ (panel.getRowForTest (2).link.getSelectedIndex(), 1);   // INDEP

    int reportedSlot = -1; bool reportedLinked = true;
    panel.onSlotLinkChanged = [&] (int s, bool l) { reportedSlot = s; reportedLinked = l; };

    // triggerClick() posts an async command message this headless suite never
    // pumps (juce_Button.cpp, Button::triggerClick -> postCommandMessage; see
    // test_moderail.cpp's header note). setToggleState(..., sendNotificationSync)
    // is the file's established inline-dispatch click, same as every other
    // click in this suite.
    panel.getRowForTest (2).link.getSegmentForTest (0)
        .setToggleState (true, juce::sendNotificationSync);
    EXPECT_EQ (reportedSlot, 2);
    EXPECT_TRUE (reportedLinked);
}

//==============================================================================
// Per-slot tuning (brief 2026-08-24): each row ends in a Tune combo -- G follows
// the global DETECTION strip, C gives the slot its own parameter set edited in
// a detail row below the slot's row.

namespace
{
gui::SlotPanel::SlotTuning makeSeededTuning()
{
    gui::SlotPanel::SlotTuning t;
    t.usesGlobal = true;
    t.riseMs   = 750.0;   // combo id 4
    t.persist  = 5;
    t.depthDb  = -12.0;   // combo id 2
    t.q        = 20.0;    // combo id 2
    t.thr      = 8.0;     // combo id 2
    return t;
}
} // namespace

TEST (SlotPanel, TuneComboDefaultsToGlobalOnEveryRow)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;
    gui::SlotPanel panel (engine);

    for (int i = 0; i < kMaxSlots; ++i)
        EXPECT_EQ (1, panel.getRowForTest (i).tune.getSelectedId()) << "row " << i;

    EXPECT_FALSE (panel.isDetailOpenForTest (0));
}

TEST (SlotPanel, ChoosingCustomFiresCallbackAndOpensTheSeededDetailRow)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;
    gui::SlotPanel panel (engine);
    panel.setSize (520, 260);

    panel.slotTuningProvider = [] (int) { return makeSeededTuning(); };

    int reportedSlot = -1;
    gui::SlotPanel::SlotTuning reported;

    panel.onSlotTuningChanged = [&reportedSlot, &reported] (
                                    int slotIndex, const gui::SlotPanel::SlotTuning& tuning)
    {
        reportedSlot = slotIndex;
        reported     = tuning;
    };

    // A user can only reach the Tune combo of a REVEALED row.
    panel.setVisibleRowCount (3);

    const int heightBefore = panel.getPreferredHeight();

    panel.getRowForTest (2).tune.setSelectedId (2 /* C */, juce::sendNotificationSync);

    EXPECT_EQ (2, reportedSlot);
    EXPECT_FALSE (reported.usesGlobal);
    // Seeded FROM the provider on first switch, not from the struct defaults.
    EXPECT_DOUBLE_EQ (reported.riseMs,  750.0);
    EXPECT_EQ (reported.persist, 5);
    EXPECT_DOUBLE_EQ (reported.depthDb, -12.0);
    EXPECT_DOUBLE_EQ (reported.q,       20.0);
    EXPECT_DOUBLE_EQ (reported.thr,      8.0);

    // The detail row opened BELOW slot 2's row and got real rects.
    EXPECT_TRUE (panel.isDetailOpenForTest (2));
    const auto& detail = panel.getDetailForTest (2);
    EXPECT_FALSE (detail.rise.getBounds().isEmpty());
    EXPECT_FALSE (detail.persist.getBounds().isEmpty());
    EXPECT_FALSE (detail.depth.getBounds().isEmpty());
    EXPECT_FALSE (detail.q.getBounds().isEmpty());
    EXPECT_FALSE (detail.thr.getBounds().isEmpty());

    // Combos show the seeded values (rise 750 -> id 4, persist 5, depth -12 ->
    // id 2, Q 20 -> id 2, Thr 8 -> id 2).
    EXPECT_EQ (4, detail.rise.getSelectedId());
    EXPECT_EQ (5, detail.persist.getSelectedId());
    EXPECT_EQ (2, detail.depth.getSelectedId());
    EXPECT_EQ (2, detail.q.getSelectedId());
    EXPECT_EQ (2, detail.thr.getSelectedId());

    // Preferred height grew by exactly one detail row.
    EXPECT_EQ (heightBefore + gui::SlotPanel::kRowHeight,
               panel.getPreferredHeight());
}

TEST (SlotPanel, EditingACustomComboReportsTheCompleteNewTuning)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;
    gui::SlotPanel panel (engine);
    panel.setSize (520, 260);

    panel.slotTuningProvider = [] (int) { return makeSeededTuning(); };

    int reportedSlot = -1;
    gui::SlotPanel::SlotTuning reported;

    panel.onSlotTuningChanged = [&reportedSlot, &reported] (
                                    int slotIndex, const gui::SlotPanel::SlotTuning& tuning)
    {
        reportedSlot = slotIndex;
        reported     = tuning;
    };

    panel.getRowForTest (1).tune.setSelectedId (2 /* C */, juce::sendNotificationSync);

    // Depth: -12 dB (id 2) -> -24 dB (id 4).
    panel.getDetailForTest (1).depth.setSelectedId (4, juce::sendNotificationSync);

    EXPECT_EQ (1, reportedSlot);
    EXPECT_FALSE (reported.usesGlobal);
    EXPECT_DOUBLE_EQ (reported.depthDb, -24.0);
    // Everything else rides along unchanged from the seed.
    EXPECT_DOUBLE_EQ (reported.riseMs,  750.0);
    EXPECT_EQ (reported.persist, 5);
    EXPECT_DOUBLE_EQ (reported.q,       20.0);
    EXPECT_DOUBLE_EQ (reported.thr,      8.0);
}

TEST (SlotPanel, ChoosingGlobalCollapsesTheDetailRow)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;
    gui::SlotPanel panel (engine);
    panel.setSize (520, 260);

    panel.slotTuningProvider = [] (int) { return makeSeededTuning(); };

    gui::SlotPanel::SlotTuning reported;
    panel.onSlotTuningChanged = [&reported] (int, const gui::SlotPanel::SlotTuning& tuning)
    { reported = tuning; };

    panel.getRowForTest (0).tune.setSelectedId (2 /* C */, juce::sendNotificationSync);
    EXPECT_TRUE (panel.isDetailOpenForTest (0));
    EXPECT_FALSE (panel.getDetailForTest (0).rise.getBounds().isEmpty());

    panel.getRowForTest (0).tune.setSelectedId (1 /* G */, juce::sendNotificationSync);

    EXPECT_TRUE (reported.usesGlobal);
    EXPECT_FALSE (panel.isDetailOpenForTest (0));
    // Laid out nowhere: every detail control has an EMPTY rect again.
    EXPECT_TRUE (panel.getDetailForTest (0).rise.getBounds().isEmpty());
    EXPECT_TRUE (panel.getDetailForTest (0).thr.getBounds().isEmpty());
    EXPECT_EQ (panel.getPreferredHeight(),
               gui::SlotPanel::kCaptionHeight + 3 * gui::SlotPanel::kRowHeight
                   + 3 * az::theme::spacing);   // back to the 2-row + Add baseline
}

TEST (SlotPanel, OnlyOneDetailRowIsOpenAtATime)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;
    gui::SlotPanel panel (engine);
    panel.setSize (520, 260);

    panel.slotTuningProvider = [] (int) { return makeSeededTuning(); };

    int reports = 0;
    panel.onSlotTuningChanged = [&reports] (int, const gui::SlotPanel::SlotTuning&)
    { ++reports; };

    panel.setVisibleRowCount (4);

    panel.getRowForTest (0).tune.setSelectedId (2, juce::sendNotificationSync);
    panel.getRowForTest (3).tune.setSelectedId (2, juce::sendNotificationSync);

    // Slot 3 took over the single detail row; slot 0's collapsed. Both stay
    // CUSTOM (their tune combos keep showing C) -- only the editor moved.
    EXPECT_TRUE  (panel.isDetailOpenForTest (3));
    EXPECT_FALSE (panel.isDetailOpenForTest (0));
    EXPECT_TRUE  (panel.getDetailForTest (0).rise.getBounds().isEmpty());
    EXPECT_FALSE (panel.getDetailForTest (3).rise.getBounds().isEmpty());
    // Slot 0 stays Custom even though its editor collapsed.
    EXPECT_EQ (2, panel.getRowForTest (0).tune.getSelectedId());
    EXPECT_EQ (2, panel.getRowForTest (3).tune.getSelectedId());
}
