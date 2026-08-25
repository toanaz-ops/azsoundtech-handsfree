// SlotTabs -- which routing slot the analyser and the notch table are showing.
//
// The app has always run one NotchController PER SLOT: eight independent
// detectors, each with its own spectrum and its own set of placed notches.
// Until now the GUI only ever showed slot 0's, because SpectrumView and
// NotchListPanel were handed notchControllers_[0] at construction and had no
// way to be pointed anywhere else. A rig with a vocal mic on slot 1 and a
// lectern on slot 2 could see only one of them.
//
// This is that selector. It reflects and reports; it owns nothing and it does
// not know what a NotchController is. MainComponent decides what a selection
// MEANS -- re-pointing both display panels -- exactly the way it owns what a
// routing change or a tuning change means.
//
// PLACEMENT (owner, 2026-08-25): it sits in the ACTIVE NOTCHES caption row,
// not in the masthead. It governs the analyser too, so the masthead was the
// semantically tidier home -- and nobody found it there. A control nobody
// finds is a control that does not exist. It now sits directly above the
// table whose contents it changes, and the table's caption names the slot.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "app/SlotConfig.h"

#include <array>
#include <functional>

namespace gui
{

class SlotTabs : public juce::Component
{
public:
    SlotTabs();

    // How many chips are reachable. Tracks the routing table's visible row
    // count: a slot with no row in that table has no meaning to select.
    // Clamped to [1, kMaxSlots].
    void setSlotCount (int count);
    [[nodiscard]] int getSlotCount() const { return count_; }

    // Lights one chip and clears the rest WITHOUT invoking onSlotSelected.
    // Out-of-range indices are ignored rather than clamped: a caller asking
    // for a slot that is not reachable has a bug, and silently showing it a
    // different slot's notches would hide it.
    void setSelected (int slotIndex);
    [[nodiscard]] int getSelected() const { return selected_; }

    // Fired only by a user click, with the newly selected slot index.
    std::function<void (int)> onSlotSelected;

    // Width this strip needs for its current slot count. The masthead asks,
    // rather than guessing, because the count changes at run time.
    [[nodiscard]] int getPreferredWidth() const;

    // TEST ACCESSOR ONLY -- drive a chip directly, like ModeRail's buttons.
    [[nodiscard]] juce::TextButton& getChipForTest (int slotIndex)
    {
        return chips_[(std::size_t) slotIndex];
    }

    void paint (juce::Graphics& g) override;
    void resized() override;

    static constexpr int kChipWidth   = 30;
    static constexpr int kLegendWidth = 62;

private:
    void reflect();

    std::array<juce::TextButton, kMaxSlots> chips_;
    int count_    = 2;   // the routing table's own starting row count
    int selected_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlotTabs)
};

} // namespace gui
