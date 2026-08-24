// The 8-slot routing table (spec section 7 / G-3): enable, mono/stereo width
// and per-lane channel mapping for each engine slot.
//
// Pattern: this mirrors DevicePanel exactly. The panel READS the engine
// (refresh()) and REPORTS changes through a callback -- it never calls a
// mutating engine entry point itself. MainComponent owns what a change means:
// the §6.5 detector stop/restart cycle around engine_.setSlotConfig().
//
// Channel combos list the OPEN device's real channel names via
// getInput/OutputChannelNames(). With no device open both arrays are empty;
// every combo is then cleared AND disabled rather than filled with guessed
// "1".."n" placeholders -- showing channels that do not exist is precisely the
// lie DevicePanel refuses to tell about sample rates.
//
// v1 scope: the LED column shows enabled-state only. A live notch count would
// need all eight NotchControllers handed in; the brief allows deferring that.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "app/AudioEngine.h"
#include "app/SlotConfig.h"

#include <functional>

namespace gui
{

class SlotPanel : public juce::Component
{
public:
    explicit SlotPanel (AudioEngine& engine);

    // Repopulate every row from the providers (or the engine) and select what
    // is ACTUALLY configured. Safe with no device open -- that is the state at
    // startup. Called from the message thread only, never polled.
    void refresh();

    // Every control change lands here as a COMPLETE new config for one slot.
    // The receiver applies it through its own restart cycle and calls
    // refresh() so the panel re-reads reality (the honest-revert pattern).
    std::function<void (int slotIndex, const SlotConfig&)> onSlotConfigChanged;

    // Overridable sources. Tests inject light fakes here; when null each falls
    // back to the engine member below.
    std::function<juce::StringArray()> inputChannelNamesProvider;
    std::function<juce::StringArray()> outputChannelNamesProvider;
    std::function<SlotConfig (int)>    slotConfigProvider;

    struct Row
    {
        juce::Label     number;
        juce::ToggleButton enable;

        // Item ids 1 = Mono, 2 = Stereo.
        juce::ComboBox  width;

        juce::ComboBox  inLanes [kMaxSlotLanes];
        juce::ComboBox  outLanes[kMaxSlotLanes];

        // Enabled-state LED. Colours come from az::theme (see .cpp).
        struct Led : public juce::Component
        {
            bool on = false;
            void paint (juce::Graphics& g) override;
        };
        Led led;
    };

    // TEST ACCESSOR ONLY -- row i of the fixed 8-row table. Non-const so a
    // test can drive the controls directly.
    [[nodiscard]] Row& getRowForTest (int slotIndex) { return rows_[(std::size_t) slotIndex]; }

    // How many of the 8 rows are shown. Starts at 2 -- the stereo In1/In2
    // pair most rigs need. The Add button reveals the next row, up to all 8;
    // revealing a row never enables its slot, it only makes the controls
    // reachable.
    void   setVisibleRowCount (int n);
    [[nodiscard]] int getVisibleRowCount() const { return visibleRows_; }

    static constexpr int kRowHeight = 26;
    static constexpr int kCaptionHeight = 18;

    // Height the hosting Viewport should give the panel for its CURRENT
    // visible row count: theme margins + caption + rows (+ the Add row while
    // any row is still hidden). A Viewport never sizes its content by itself
    // -- the parent must hand the panel this height or it renders empty.
    [[nodiscard]] int getPreferredHeight() const;

    void resized() override;

private:
    void handleRowChanged (int slotIndex);

    [[nodiscard]] juce::StringArray inputChannelNames() const;
    [[nodiscard]] juce::StringArray outputChannelNames() const;
    [[nodiscard]] SlotConfig        currentConfig (int slotIndex) const;

    AudioEngine& engine_;

    int visibleRows_ = 2;
    std::array<Row, kMaxSlots> rows_;

    // Sits in the row slot after the last visible one while any row is
    // hidden; reveals one more row per click.
    juce::TextButton addButton_ { {}, "+ Add slot" };

    juce::Label widthCaption_ { {}, "Width" };
    juce::Label inACaption_   { {}, "In A" };
    juce::Label inBCaption_   { {}, "In B" };
    juce::Label outACaption_  { {}, "Out A" };
    juce::Label outBCaption_  { {}, "Out B" };
    juce::Label ledCaption_   { {}, "Active" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlotPanel)
};

} // namespace gui
