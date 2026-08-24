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
