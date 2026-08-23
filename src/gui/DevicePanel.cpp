#include "gui/DevicePanel.h"

#include "gui/DeviceViewModel.h"

namespace gui
{

DevicePanel::DevicePanel (AudioEngine& engine)
    : engine_ (engine)
{
    for (auto* label : { &deviceTypeLabel_, &deviceLabel_, &sampleRateLabel_, &bufferSizeLabel_ })
        addAndMakeVisible (*label);

    for (auto* box : { &deviceTypeBox, &deviceBox, &sampleRateBox, &bufferSizeBox })
        addAndMakeVisible (*box);

    // Changing the TYPE invalidates the device name: the name sitting in
    // deviceBox belongs to the type being left. An empty name opens the new
    // driver's default device instead.
    deviceTypeBox.onChange = [this] { restartWith (deviceTypeBox.getText(), {}); };
    deviceBox    .onChange = [this] { restartWith (deviceTypeBox.getText(), deviceBox.getText()); };
    sampleRateBox.onChange = [this] { applySampleRate(); };
    bufferSizeBox.onChange = [this] { applyBufferSize(); };
}

void DevicePanel::refresh()
{
    // dontSendNotification throughout: every selection below is us reflecting
    // the engine, and a notification would re-enter the change handlers and
    // restart the very device being described.

    const juce::StringArray types = engine_.getAvailableDeviceTypeNames();

    deviceTypeBox.clear (juce::dontSendNotification);

    for (int i = 0; i < types.size(); ++i)
        deviceTypeBox.addItem (types[i], i + 1);

    // The ACTUAL type, not the requested one. AudioEngine::start() documents
    // that JUCE silently keeps the current type when the requested one is not
    // registered -- which is what happens every time ASIO is asked for on a
    // machine with no ASIO driver. Echoing the request would show the user
    // "ASIO" while WASAPI was running.
    const int typeIndex = types.indexOf (engine_.getCurrentDeviceType());

    if (typeIndex >= 0)
        deviceTypeBox.setSelectedId (typeIndex + 1, juce::dontSendNotification);

    const juce::StringArray devices = engine_.getAvailableDeviceNames();

    deviceBox.clear (juce::dontSendNotification);

    for (int i = 0; i < devices.size(); ++i)
        deviceBox.addItem (devices[i], i + 1);

    const int deviceIndex = devices.indexOf (engine_.getCurrentDeviceName());

    if (deviceIndex >= 0)
        deviceBox.setSelectedId (deviceIndex + 1, juce::dontSendNotification);

    // Both of these are EMPTY with no device open, which is why refresh() has
    // to run again after every successful start().
    sampleRates_ = engine_.getAvailableSampleRates();

    sampleRateBox.clear (juce::dontSendNotification);

    for (int i = 0; i < sampleRates_.size(); ++i)
        sampleRateBox.addItem (formatSampleRateItem (sampleRates_[i]), i + 1);

    const int rateIndex = indexOfSampleRate (sampleRates_, engine_.getCurrentSampleRateHz());

    if (rateIndex >= 0)
        sampleRateBox.setSelectedId (rateIndex + 1, juce::dontSendNotification);

    bufferSizes_ = engine_.getAvailableBufferSizes();

    bufferSizeBox.clear (juce::dontSendNotification);

    for (int i = 0; i < bufferSizes_.size(); ++i)
        bufferSizeBox.addItem (formatBufferSizeItem (bufferSizes_[i]), i + 1);

    const int bufferIndex = bufferSizes_.indexOf (engine_.getCurrentBufferSize());

    if (bufferIndex >= 0)
        bufferSizeBox.setSelectedId (bufferIndex + 1, juce::dontSendNotification);
}

void DevicePanel::restartWith (const juce::String& typeName, const juce::String& deviceName)
{
    if (onBeforeRestart != nullptr)
        onBeforeRestart();

    engine_.stop();
    engine_.setAudioDeviceType (typeName);
    engine_.setAudioDevice (deviceName);
    engine_.start();

    if (onAfterRestart != nullptr)
        onAfterRestart();

    // A failed start leaves its reason in getLastDeviceError(), which the
    // status bar polls, so nothing is reported here. Any stale refusal message
    // is cleared though: it described a device that is no longer open.
    report ({});

    // The new device's supported rates and buffer sizes only become knowable
    // now that it is open.
    refresh();
}

void DevicePanel::applySampleRate()
{
    const int index = sampleRateBox.getSelectedId() - 1;

    if (! juce::isPositiveAndBelow (index, sampleRates_.size()))
        return;

    const double requested = sampleRates_[index];

    // Honour the bool. setSampleRate() returns false when the hardware refuses
    // and supplies no reason string, so the honest report is the fact plus what
    // is still running. The refresh() below re-selects the ACTUAL rate, which
    // IS the revert -- leaving the combo showing a rate the device is not
    // running is precisely the failure the bool exists to prevent.
    if (onBeforeRestart != nullptr)
        onBeforeRestart();

    if (engine_.setSampleRate (requested))
        report ({});
    else
        report (refusedSampleRateMessage (requested, engine_.getCurrentSampleRateHz()));

    if (onAfterRestart != nullptr)
        onAfterRestart();

    refresh();
}

void DevicePanel::applyBufferSize()
{
    const int index = bufferSizeBox.getSelectedId() - 1;

    if (! juce::isPositiveAndBelow (index, bufferSizes_.size()))
        return;

    const int requested = bufferSizes_[index];

    if (onBeforeRestart != nullptr)
        onBeforeRestart();

    if (engine_.setBufferSize (requested))
        report ({});
    else
        report (refusedBufferSizeMessage (requested, engine_.getCurrentBufferSize()));

    if (onAfterRestart != nullptr)
        onAfterRestart();

    refresh();
}

void DevicePanel::report (const juce::String& message)
{
    if (onMessage != nullptr)
        onMessage (message);
}

void DevicePanel::resized()
{
    auto area = getLocalBounds().reduced (4, 2);

    auto row = area.removeFromTop (26);
    deviceTypeLabel_.setBounds (row.removeFromLeft (100));
    deviceTypeBox   .setBounds (row.removeFromLeft (160).reduced (2, 1));
    deviceLabel_    .setBounds (row.removeFromLeft (60));
    deviceBox       .setBounds (row.reduced (2, 1));

    area.removeFromTop (4);

    row = area.removeFromTop (26);
    sampleRateLabel_.setBounds (row.removeFromLeft (100));
    sampleRateBox   .setBounds (row.removeFromLeft (110).reduced (2, 1));
    bufferSizeLabel_.setBounds (row.removeFromLeft (60));
    bufferSizeBox   .setBounds (row.removeFromLeft (140).reduced (2, 1));
}

} // namespace gui
