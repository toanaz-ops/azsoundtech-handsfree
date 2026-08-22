// Device, sample rate and buffer size selection -- Tasks 16 and 17.
//
// Why the two tasks share one component
// =====================================
// getAvailableSampleRates() and getAvailableBufferSizes() are BOTH EMPTY until
// a device is open, so the rate and buffer combos have to be repopulated by the
// very act of opening a device. Splitting them into a separate component would
// mean inventing a callback contract between the two purely to say "I just
// restarted the device, refill yourself". One refresh() over all four combos is
// the whole of it.
//
// (Plan Task 16 names the file DeviceSelector. Renamed because this owns the
// buffer size too, which "selector" does not describe.)

#pragma once

// Module headers rather than <JuceHeader.h>, for the same reason AudioEngine.h
// is written this way: JuceHeader.h exists only for juce_add_* targets, and
// this component has to compile into the plain add_executable test target.
#include <juce_gui_basics/juce_gui_basics.h>

#include "app/AudioEngine.h"

#include <functional>

namespace gui
{

class DevicePanel : public juce::Component
{
public:
    explicit DevicePanel (AudioEngine& engine);

    // Repopulate every combo from the engine and select what is ACTUALLY
    // running. Safe with no device open -- that is the state at startup.
    void refresh();

    // Something the user needs to see, or empty to clear it. In practice: the
    // hardware refused a setting.
    std::function<void (const juce::String&)> onMessage;

    void resized() override;

    // Public for the same reason ModeBar's buttons are: they are the
    // component's interface.
    juce::ComboBox deviceTypeBox, deviceBox, sampleRateBox, bufferSizeBox;

private:
    // stop() -> set -> start(): both setters apply on the NEXT start(), by
    // design, so a device change is always a restart.
    void restartWith (const juce::String& typeName, const juce::String& deviceName);
    void applySampleRate();
    void applyBufferSize();
    void report (const juce::String& message);

    AudioEngine& engine_;

    juce::Label deviceTypeLabel_ { {}, "Audio Device:" };
    juce::Label deviceLabel_     { {}, "Device:" };
    juce::Label sampleRateLabel_ { {}, "Sample Rate:" };
    juce::Label bufferSizeLabel_ { {}, "Buffer:" };

    // Kept alongside the combos so a selection maps back to a value without
    // parsing the item text we just formatted.
    juce::Array<double> sampleRates_;
    juce::Array<int>    bufferSizes_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DevicePanel)
};

} // namespace gui
