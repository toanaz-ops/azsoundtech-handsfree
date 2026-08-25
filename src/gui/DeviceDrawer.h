// DeviceDrawer -- GUI console redesign Task 3 (spec section 2).
//
// Wraps the EXISTING DevicePanel (re-used, never rewritten) and hosts the
// Settings row header (presets placeholder + status badge).
//
// The drawer is ALWAYS OPEN and has no collapse gear (2026-08-25 owner
// decision: the L1/L2 layouts are gone -- one Classic arrangement, and an
// invisible device row reads as "the app lost my interface").
//
// OWNERSHIP (memory/bridge-lifecycle-devicepanel lesson): the wrapped
// DevicePanel is NOT owned here. It is re-parented under this drawer once in
// the constructor and stays the same instance forever -- the real-device
// restart path lives inside it, and destroying/recreating it would drop an
// open audio device mid-show.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "gui/DevicePanel.h"

namespace gui
{

class DeviceDrawer : public juce::Component
{
public:
    // Re-parents wrappedPanel under this drawer. The panel must outlive it.
    explicit DeviceDrawer (DevicePanel& wrappedPanel);

    // Hosts the StatusBadge in the header row (ownership stays with the caller;
    // resized() tolerates null).
    void setStatusBadge (juce::Component* badgeOrNull);

    // Height the parent should give this drawer: header plus content, always.
    // Queried by MainComponent::resized().
    [[nodiscard]] int getPreferredHeight() const;

    // TEST ACCESSORS -- let headless tests see whether the wrapped panel
    // actually got a usable rect.
    [[nodiscard]] juce::Rectangle<int> wrappedBoundsForTest() const { return wrapped_.getBounds(); }

    void resized() override;

    static constexpr int kHeaderHeight  = 48;   // >= the 44 px touch target
    static constexpr int kContentHeight = 64;   // matches DevicePanel's two combo rows

private:
    DevicePanel& wrapped_;                 // NOT owned
    juce::Component* badge_ = nullptr;     // NOT owned
    juce::Label presetsPlaceholder_ { {}, "(presets)" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeviceDrawer)
};

} // namespace gui
