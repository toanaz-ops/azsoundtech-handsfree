// StatusBadge -- the one thing on screen that answers "is it protecting?"
// without being read, only glanced at.
//
// Drawn as a real indicator in a bezel: a recessed well, an LED with a genuine
// gaussian halo behind it, and a tracked uppercase legend. The halo is
// melatonin_blur rather than a radial gradient because a gradient halo on a
// small bright dot bands visibly on a dark ground -- and because melatonin
// caches, so the blur is computed once and blitted after that.
//
// Every value comes from az::theme.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <melatonin_blur/melatonin_blur.h>

namespace gui
{

enum class ProtectionState
{
    Protecting,
    Idle,
    Bypassed
};

class StatusBadge : public juce::Component
{
public:
    StatusBadge();

    // Reflects a state; paints on the next pass. Does not drive anything.
    void setState (ProtectionState newState);
    [[nodiscard]] ProtectionState getState() const { return state; }

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    // The state's colour and legend, in one place so paint() reads as layout
    // rather than as a switch statement wrapped around drawing calls.
    [[nodiscard]] juce::Colour stateColour() const;
    [[nodiscard]] juce::String stateLabel() const;

    ProtectionState state = ProtectionState::Idle;

    // Rebuilt in resized(), NOT in paint(): melatonin caches its blur against
    // the path it was handed, so a path rebuilt every frame would recompute
    // the gaussian every frame and throw the cache away.
    juce::Path ledPath_;

    melatonin::DropShadow ledGlow_ { juce::Colours::transparentBlack, 9 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StatusBadge)
};

} // namespace gui
