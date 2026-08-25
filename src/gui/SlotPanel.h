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
    // One slot's detection-tuning snapshot (brief 2026-08-24): Global follows
    // the DETECTION strip; Custom carries the slot's own five parameters.
    struct SlotTuning
    {
        bool   usesGlobal = true;
        double riseMs     = 250;
        int    persist    = 3;
        double depthDb    = -18;
        double q          = 30;
        double thr        = 10;
    };

    explicit SlotPanel (AudioEngine& engine);

    // Repopulate every row from the providers (or the engine) and select what
    // is ACTUALLY configured. Safe with no device open -- that is the state at
    // startup. Called from the message thread only, never polled.
    void refresh();

    // Every control change lands here as a COMPLETE new config for one slot.
    // The receiver applies it through its own restart cycle and calls
    // refresh() so the panel re-reads reality (the honest-revert pattern).
    std::function<void (int slotIndex, const SlotConfig&)> onSlotConfigChanged;

    // Fired by setVisibleRowCount after the preferred height changes. The
    // direct parent is a Viewport, whose resized() never re-sizes its
    // content -- only the owner's layout pass hands this panel its new
    // size. Null fallback: resized() on the direct parent, then on self.
    std::function<void()> onPreferredHeightChanged;

    // Per-slot tuning (brief 2026-08-24): any Tune-combo or custom-parameter
    // change lands here as a COMPLETE SlotTuning for one slot. The receiver
    // owns what it means (MainComponent applies the five values to that
    // slot's NotchController); this panel never touches a controller.
    std::function<void (int slotIndex, const SlotTuning&)> onSlotTuningChanged;

    // Overridable sources. Tests inject light fakes here; when null each falls
    // back to the engine member below.
    std::function<juce::StringArray()> inputChannelNamesProvider;
    std::function<juce::StringArray()> outputChannelNamesProvider;
    std::function<SlotConfig (int)>    slotConfigProvider;
    // Seeds/refreshes a row's tuning state (the flag plus the values its
    // controller currently holds). Null -> defaults, Global.
    std::function<SlotTuning (int)>    slotTuningProvider;

    struct Row
    {
        juce::Label     number;
        juce::ToggleButton enable;

        // Item ids 1 = Mono, 2 = Stereo.
        juce::ComboBox  width;

        // Item ids 1 = G (global DETECTION strip), 2 = C (custom per-slot set).
        juce::ComboBox  tune;

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

    // The custom-tuning editor shown BELOW a slot's row while its Tune combo
    // reads C: the five mini combos (rise / persist / depth / Q / threshold),
    // same choice lists as TuningPanel's global strip.
    struct DetailRow
    {
        juce::Label riseLabel   { {}, "Rise" };
        juce::Label persistLabel{ {}, "Persist" };
        juce::Label depthLabel  { {}, "Depth" };
        juce::Label qLabel      { {}, "Q" };
        juce::Label thrLabel    { {}, "Thr" };

        juce::ComboBox rise;     // 100/250/500/750/1000 ms, id = position+1
        juce::ComboBox persist;  // 1..6, id == value
        juce::ComboBox depth;    // -6/-12/-18/-24 dB, id = position+1
        juce::ComboBox q;        // 10..50, id = position+1
        juce::ComboBox thr;      // 6/8/10/12/15, id = position+1
    };

    // TEST ACCESSOR ONLY -- row i of the fixed 8-row table. Non-const so a
    // test can drive the controls directly.
    [[nodiscard]] Row& getRowForTest (int slotIndex) { return rows_[(std::size_t) slotIndex]; }

    // TEST ACCESSOR ONLY -- the custom-tuning editor of row i.
    [[nodiscard]] DetailRow& getDetailForTest (int slotIndex)
    {
        return details_[(std::size_t) slotIndex];
    }

    // True while slot i's detail row is the ONE open editor (laid out below
    // its own row). At most one of the eight is ever true.
    [[nodiscard]] bool isDetailOpenForTest (int slotIndex) const
    {
        return openDetailSlot_ == slotIndex;
    }

    // How many of the 8 rows are shown. Starts at 2 -- the stereo In1/In2
    // pair most rigs need. The Add button reveals the next row, up to all 8;
    // revealing a row never enables its slot, it only makes the controls
    // reachable.
    void   setVisibleRowCount (int n);
    [[nodiscard]] int getVisibleRowCount() const { return visibleRows_; }

    // 30 px rows, up from 26: this table is operated in the dark with one hand
    // and the extra 4 px is the difference between hitting a row's combo and
    // hitting the one above it.
    static constexpr int kRowHeight = 30;
    static constexpr int kCaptionHeight = 20;

    // Height the hosting Viewport should give the panel for its CURRENT
    // visible row count: theme margins + caption + rows (+ the Add row while
    // any row is still hidden). A Viewport never sizes its content by itself
    // -- the parent must hand the panel this height or it renders empty.
    [[nodiscard]] int getPreferredHeight() const;

    // Width the whole fixed-column grid needs. The hosting Viewport sizes this
    // panel to at least this, so a narrow window SCROLLS the table sideways
    // instead of clipping its last columns off the edge.
    [[nodiscard]] int getPreferredWidth() const;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void handleRowChanged (int slotIndex);
    void handleTuneChanged (int slotIndex);
    void handleDetailChanged();
    void openDetailFor (int slotIndex);
    void closeDetail();
    void seedDetailFrom (int slotIndex);
    void notifyPreferredHeightChanged();

    [[nodiscard]] SlotTuning currentTuning() const;

    [[nodiscard]] juce::StringArray inputChannelNames() const;
    [[nodiscard]] juce::StringArray outputChannelNames() const;
    [[nodiscard]] SlotConfig        currentConfig (int slotIndex) const;

    AudioEngine& engine_;

    int visibleRows_ = 2;
    std::array<Row, kMaxSlots> rows_;
    std::array<DetailRow, kMaxSlots> details_;

    // The one slot whose detail editor is open; -1 when none is. The slot
    // itself stays Custom when its editor is closed by another slot opening.
    int openDetailSlot_ = -1;

    // Sits in the row slot after the last visible one while any row is
    // hidden; reveals one more row per click.
    // NOTE the empty first argument: TextButton's two-argument constructor is
    // (buttonName, TOOLTIP) -- not (name, text) the way Label's is. Written
    // this way the button had no label at all, which the old flat styling hid
    // and the rebuilt outline made obvious. The text is set in the ctor.
    juce::TextButton addButton_;

    juce::Label widthCaption_ { {}, "Width" };
    juce::Label inACaption_   { {}, "In A" };
    juce::Label inBCaption_   { {}, "In B" };
    juce::Label outACaption_  { {}, "Out A" };
    juce::Label outBCaption_  { {}, "Out B" };
    // "On", not "Active": the LED column is 30 px wide, and a caption that
    // ellipsises to "A..." labels nothing at all.
    juce::Label ledCaption_   { {}, "On" };
    juce::Label tuneCaption_  { {}, "Tune" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlotPanel)
};

} // namespace gui
