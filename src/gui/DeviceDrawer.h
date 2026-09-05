// DeviceDrawer -- the INTERFACE section of the rig column.
//
// Wraps the EXISTING DevicePanel (re-used, never rewritten) under a section
// caption. It is ALWAYS OPEN and has no collapse gear (2026-08-25 owner
// decision): an invisible device row reads as "the app lost my interface".
//
// The 2026-08-25 rebuild took two things OUT of this header row. The status
// badge moved to the masthead, where the question it answers belongs; and the
// "(presets)" placeholder went altogether, because a control that names a
// feature and then does nothing is worse than no control.
//
// OWNERSHIP (memory/bridge-lifecycle-devicepanel lesson): the wrapped
// DevicePanel is NOT owned here. It is re-parented under this drawer once in
// the constructor and stays the same instance forever -- the real-device
// restart path lives inside it, and destroying/recreating it would drop an
// open audio device mid-show.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "gui/DevicePanel.h"

#include <functional>

namespace gui
{

class DeviceDrawer : public juce::Component
{
public:
    // Re-parents wrappedPanel under this drawer. The panel must outlive it.
    explicit DeviceDrawer (DevicePanel& wrappedPanel);

    // Height the parent should give this drawer: caption plus content, always.
    // Queried by MainComponent::resized().
    [[nodiscard]] int getPreferredHeight() const;

    // TEST ACCESSORS -- let headless tests see whether the wrapped panel
    // actually got a usable rect.
    [[nodiscard]] juce::Rectangle<int> wrappedBoundsForTest() const { return wrapped_.getBounds(); }

    // The PRESET row (Task P3): two buttons under the wrapped DevicePanel.
    // PUBLIC because they ARE this drawer's interface -- MainComponent wires
    // their requests and headless tests drive their onClick directly (the same
    // reason ModeRail's buttons are public). Named-ONLY construction: the
    // 2-arg brace form treats the second string as a tooltip (JUCE 9 trap,
    // memory juce9-api-traps).
    juce::TextButton loadButton { "LOAD..." };
    juce::TextButton saveButton { "SAVE..." };

    // Fired when the matching button is clicked. The drawer knows nothing about
    // presets or files -- it just reports the request, exactly as ModeRail
    // reports a mode. MainComponent turns these into a file chooser + load/save.
    std::function<void()> onLoadRequested;
    std::function<void()> onSaveRequested;

    void paint (juce::Graphics& g) override;
    void resized() override;

    // A caption band, not a header row: it holds one tracked word and the
    // groove under it. 48 px was sized for a badge that no longer lives here.
    static constexpr int kHeaderHeight  = 26;
    static constexpr int kContentHeight = 64;   // DevicePanel's two combo rows

    // The PRESET section below the device combos: its own caption band plus a
    // button row. getPreferredHeight() grows by exactly these two.
    static constexpr int kPresetCaptionHeight = 26;
    static constexpr int kPresetRowHeight     = 28;

private:
    DevicePanel& wrapped_;                 // NOT owned

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeviceDrawer)
};

} // namespace gui
