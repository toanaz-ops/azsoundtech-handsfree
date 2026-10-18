// Lane U (spec 2026-10-18 section 3.5): the Dumb-mode setup wizard -- device,
// mic, output -- drawn over the whole window.
//
// Holds no engine reference. Everything it shows comes through providers and
// everything it does goes out through callbacks, which MainComponent wires to
// the SAME paths Geek uses (DevicePanel's restart, changeSlotConfig). The
// state lives in SetupWizardModel, which is tested on its own.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "app/SetupWizardModel.h"

#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace gui
{

class SetupWizard : public juce::Component,
                    private juce::Timer
{
public:
    SetupWizard();
    ~SetupWizard() override;

    // ---- What it reads --------------------------------------------------
    std::function<juce::StringArray()> typeNamesProvider, deviceNamesProvider;
    std::function<juce::String()>      currentTypeProvider, currentDeviceProvider;
    // What the audio CALLBACK receives, not what the device advertises (I-7).
    std::function<std::pair<int, int>()> channelCountsProvider;
    // takeInputPeak: the largest |x| since the last call, for one input channel.
    std::function<float (int)> inputPeakProvider;

    // ---- What it does ---------------------------------------------------
    std::function<void (const juce::String& type, const juce::String& name)> onDeviceChosen;
    std::function<void (bool)>             onMeterEnabled;
    std::function<void (const SlotConfig&)> onFinish;
    std::function<void()>                   onCancel;

    // Shows the wizard at step 1. Idempotent while open.
    void open();
    // Hides it and turns the meter off. Does NOT call onCancel.
    void close();
    [[nodiscard]] bool isOpen() const { return isVisible(); }

    // Re-reads the device lists and channel counts -- after a restart.
    void refreshDevice();

    void paint (juce::Graphics&) override;
    void resized() override;

    // TEST ACCESSORS.
    [[nodiscard]] SetupWizardModel& getModelForTest() { return model_; }
    [[nodiscard]] juce::TextButton& getNextButtonForTest()   { return nextButton_; }
    [[nodiscard]] juce::TextButton& getBackButtonForTest()   { return backButton_; }
    [[nodiscard]] juce::TextButton& getCancelButtonForTest() { return cancelButton_; }
    [[nodiscard]] juce::ComboBox& getTypeBoxForTest()   { return typeBox_; }
    [[nodiscard]] juce::ComboBox& getDeviceBoxForTest() { return deviceBox_; }
    [[nodiscard]] int getChoiceCountForTest() const { return (int) choiceButtons_.size(); }
    [[nodiscard]] juce::TextButton& getChoiceButtonForTest (int i) { return *choiceButtons_[(std::size_t) i]; }
    void tickForTest() { timerCallback(); }

private:
    void timerCallback() override;
    void showStep();
    void rebuildChoices();
    void updateButtons();
    void setMeter (bool on);
    [[nodiscard]] static juce::String labelFor (const SetupWizardModel::Choice& c);

    SetupWizardModel model_;

    juce::ComboBox   typeBox_, deviceBox_;
    juce::TextButton nextButton_, backButton_, cancelButton_;
    std::vector<std::unique_ptr<juce::TextButton>> choiceButtons_;

    std::vector<float> meterLevels_;   // per input channel, decaying
    bool meterOn_ = false;
    bool filling_ = false;             // suppresses combo onChange while filling

    juce::Rectangle<int> textArea_, meterArea_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SetupWizard)
};

} // namespace gui
