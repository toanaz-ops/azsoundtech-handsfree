// StatusBadge -- GUI console redesign Task 2 (spec 2026-08-23 section 2).
// Answers in one glance "is the app protecting right now?" -- a coloured dot
// plus a short word, every value taken from az::theme.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

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
    StatusBadge() = default;

    // Reflects a state; paints on the next pass. Does not drive anything.
    void setState (ProtectionState newState);
    [[nodiscard]] ProtectionState getState() const { return state; }

    void paint (juce::Graphics& g) override;

private:
    ProtectionState state = ProtectionState::Idle;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StatusBadge)
};

} // namespace gui
