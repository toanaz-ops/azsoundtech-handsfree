// Lane U (spec 2026-10-18 section 3.3): the whole console in Dumb mode -- one
// status lamp spoken as a sentence, and three switches.
//
// Holds NO reference to the engine or any controller. MainComponent pushes
// DumbInputs every tick and wires the callbacks to the SAME functions the
// Geek console calls (invariant 1): this view adds no control path of its own.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "gui/DumbStatus.h"

#include <functional>

namespace gui
{

class DumbView : public juce::Component
{
public:
    DumbView();

    // Repaints only when something visible changed.
    void setInputs (const DumbInputs& inputs, const juce::String& message);

    [[nodiscard]] const DumbStatus& getStatus() const { return status_; }

    // Left switch, by job: ĐO PHÒNG / DỪNG / ÁP DỤNG.
    std::function<void()> onMeasure, onStop, onApply;
    // Middle switch, by job: BẬT CHỐNG HÚ / BỎ.
    std::function<void()> onEnable, onDismiss;
    std::function<void()> onDisable;        // TẮT
    std::function<void()> onOpenWizard;     // the "Cần cài đặt" lamp
    std::function<void()> onSwitchToGeek;   // the corner button

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;

    // The component's interface, public for the same reason ModeRail's are.
    juce::TextButton leftButton, middleButton, offButton;
    juce::TextButton geekButton;

    [[nodiscard]] juce::Rectangle<int> getLampBoundsForTest() const { return lampArea_; }

private:
    void applyStatus();

    DumbStatus   status_;
    juce::String message_;
    bool         hasStatus_ = false;
    juce::Rectangle<int> lampArea_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DumbView)
};

} // namespace gui
