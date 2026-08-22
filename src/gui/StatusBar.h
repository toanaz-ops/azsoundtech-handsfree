// Status display -- Task 18, spec section 6.1:
//     Status: * 48 kHz / 2 in / 2 out / Latency 5.3 ms
//
// Everything this component decides lives in DeviceViewModel and is tested
// there. What is left here is drawing.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "gui/DeviceViewModel.h"

namespace gui
{

class StatusBar : public juce::Component
{
public:
    // Explicit because JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR declares a
    // deleted copy constructor, and any user-declared constructor suppresses
    // the implicit default one.
    StatusBar() = default;

    // `message` is the raw text of whatever went wrong, or empty when nothing
    // has. Two kinds arrive here: AudioEngine::getLastDeviceError(), and the
    // GUI's own report that the hardware refused a setting.
    void setStatus (const DeviceStatus& status, const juce::String& message);

    void paint (juce::Graphics& g) override;

private:
    DeviceStatus status_;
    juce::String message_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StatusBar)
};

} // namespace gui
