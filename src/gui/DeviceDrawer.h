// DeviceDrawer -- GUI console redesign Task 3 (spec section 2).
//
// Wraps the EXISTING DevicePanel (re-used, never rewritten) and hosts the
// Settings row: the L1/L2 layout toggle plus a placeholder where presets
// will live later (spec G-2 / section 2).
//
// Two behaviours, one component:
//   L2 Performance -- collapsible drawer. The gear button slides the device
//     content in/out; the header (settings + gear) stays visible.
//   L1 Classic     -- plain top bar, exactly like the old layout. The gear is
//     hidden and the content is always shown.
//
// OWNERSHIP (memory/bridge-lifecycle-devicepanel lesson): the wrapped
// DevicePanel is NOT owned here. It is re-parented under this drawer once in
// the constructor and stays the same instance forever -- the real-device
// restart path lives inside it, and destroying/recreating it on a layout
// switch would drop an open audio device mid-show.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "gui/DevicePanel.h"

#include <functional>

namespace gui
{

enum class ScreenLayout
{
    Classic,      // L1: vertical spec layout, device bar on top
    Performance   // L2: default; big spectrum + 96 px rail on the right
};

class DeviceDrawer : public juce::Component
{
public:
    // Re-parents wrappedPanel under this drawer. The panel must outlive it.
    explicit DeviceDrawer (DevicePanel& wrappedPanel);

    // Switches behaviour between the collapsible L2 drawer and the plain L1
    // top bar. Entering L2 starts closed.
    void applyLayoutMode (ScreenLayout layout);

    void setOpen (bool shouldOpen);
    void toggleOpen();
    [[nodiscard]] bool isOpen() const { return open_; }

    // Fired by the settings-row toggle buttons (only when a button ends up ON).
    std::function<void (ScreenLayout)> onLayoutSelected;

    // Reflects a choice made elsewhere without firing onLayoutSelected.
    void setSelectedLayout (ScreenLayout layout);

    // Hosts the StatusBadge in the header row (ownership stays with the caller;
    // resized() tolerates null).
    void setStatusBadge (juce::Component* badgeOrNull);

    // Height the parent should give this drawer right now: header always,
    // content only while open. Queried by MainComponent::resized().
    [[nodiscard]] int getPreferredHeight() const;

    // TEST ACCESSORS -- let headless tests see whether the wrapped panel
    // actually got a usable rect when the drawer opened.
    [[nodiscard]] juce::Rectangle<int> wrappedBoundsForTest() const { return wrapped_.getBounds(); }

    void resized() override;

    static constexpr int kHeaderHeight  = 48;   // >= the 44 px touch target
    static constexpr int kContentHeight = 64;   // matches DevicePanel's two combo rows

    // Public for the same reason ModeBar/ModeRail expose their buttons.
    juce::TextButton gearButton_;
    juce::TextButton classicButton_     { "L1 CLASSIC" };
    juce::TextButton performanceButton_ { "L2 PERFORMANCE" };

private:
    DevicePanel& wrapped_;                 // NOT owned
    juce::Component* badge_ = nullptr;     // NOT owned
    juce::Label presetsPlaceholder_ { {}, "(presets)" };

    bool collapsible_ = true;
    bool open_        = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeviceDrawer)
};

} // namespace gui
