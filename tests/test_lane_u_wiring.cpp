// Lane U (spec 2026-10-18) -- MainComponent's settings memory and UI modes.
//
// Headless: no device ever opens (an engine that passes input to output is a
// feedback path in a test suite), so channel counts read 0 and the
// device-dependent steps are driven through their test seams.

#include <gtest/gtest.h>

#include "app/MainComponent.h"
#include "app/PresetManager.h"

namespace
{
struct TempDir
{
    juce::File dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("hf-laneu-" + juce::Uuid().toString());
    TempDir()  { dir.createDirectory(); }
    ~TempDir() { dir.deleteRecursively(); }
    juce::File file() const { return dir.getChildFile ("settings.json"); }
};

void writeSettings (const juce::File& f, const AppSettings& s)
{
    ASSERT_TRUE (az::host::saveHostSettings (f, appSettingsToHost (s)));
}

AppSettings routedSettings()
{
    AppSettings s;
    s.uiMode     = UiMode::Dumb;
    s.device     = { "ASIO", "Saved Card", 48000.0, 128 };
    s.hasRouting = true;
    s.slots[0]   = { true, 1, { 1, 0 }, { 0, 0 } };   // fits a 2x2 device
    s.slots[1]   = { true, 2, { 0, 1 }, { 4, 5 } };   // outputs 4/5 do not
    return s;
}
}

TEST (LaneU, WithoutEnableSettingsTheConsoleIsGeekAndTouchesNoFile)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent mc;

    EXPECT_EQ (mc.getUiMode(), UiMode::Geek);
    EXPECT_FALSE (mc.getDumbViewForTest().isVisible());
    EXPECT_TRUE (mc.getSimpleModeButtonForTest().isVisible());
    EXPECT_FALSE (mc.saveSettings());
}

TEST (LaneU, DumbCoversTheWholeConsoleAndGeekComesBack)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent mc;
    mc.setSize (MainComponent::kDefaultWidth, MainComponent::kDefaultHeight);

    mc.setUiMode (UiMode::Dumb);
    auto& dumb = mc.getDumbViewForTest();
    EXPECT_TRUE (dumb.isVisible());
    EXPECT_TRUE (dumb.isOpaque());
    EXPECT_EQ (dumb.getBounds(), mc.getLocalBounds());
    EXPECT_EQ (mc.getIndexOfChildComponent (&dumb), mc.getNumChildComponents() - 1);
    EXPECT_FALSE (mc.getSimpleModeButtonForTest().isVisible());

    dumb.geekButton.onClick();
    EXPECT_EQ (mc.getUiMode(), UiMode::Geek);
    EXPECT_FALSE (dumb.isVisible());

    mc.getSimpleModeButtonForTest().onClick();
    EXPECT_EQ (mc.getUiMode(), UiMode::Dumb);
}

TEST (LaneU, DumbSwitchesDriveTheSameModeRoute)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent mc;
    mc.setUiMode (UiMode::Dumb);
    auto& dumb = mc.getDumbViewForTest();

    dumb.offButton.onClick();
    EXPECT_EQ (mc.getAudioEngine().getMode(), AudioEngine::Mode::Bypass);
    EXPECT_TRUE (mc.dumbInputsForTest().bypassed);

    dumb.onEnable();
    EXPECT_EQ (mc.getAudioEngine().getMode(), AudioEngine::Mode::Auto);
    EXPECT_TRUE (mc.dumbInputsForTest().autoMode);
}

TEST (LaneU, NoDeviceMeansNotConfiguredAndTheLampOffersSetup)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent mc;
    mc.setUiMode (UiMode::Dumb);

    EXPECT_FALSE (mc.dumbInputsForTest().configured);
    EXPECT_TRUE (mc.getDumbViewForTest().getStatus().lampOpensWizard);
    EXPECT_TRUE (mc.getDumbViewForTest().offButton.isEnabled());
}

TEST (LaneU, MeasureFollowsTheConfirmationGuardLikeGeeksDoButton)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent mc;

    std::function<void (bool)> pending;
    mc.soundcheckConfirmHook = [&] (const juce::String&, std::function<void (bool)> answer)
    {
        pending = std::move (answer);
    };

    EXPECT_TRUE (mc.dumbInputsForTest().measureAllowed);
    mc.askSoundcheckConfirmationForTest ({ SoundcheckController::Target {} });
    EXPECT_FALSE (mc.dumbInputsForTest().measureAllowed);

    pending (false);
    EXPECT_TRUE (mc.dumbInputsForTest().measureAllowed);
}

TEST (LaneU, FreshMachineOpensDumbAndMachineWithPresetsOpensGeek)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    TempDir t;
    MainComponent mc;
    mc.enableSettings (t.file());

    const auto expected = PresetManager::getPresetDirectory().isDirectory() ? UiMode::Geek
                                                                           : UiMode::Dumb;
    EXPECT_EQ (mc.getUiMode(), expected);
    EXPECT_TRUE (t.file().existsAsFile());   // the mode is remembered at once
}

TEST (LaneU, SavedModeWinsOverTheFirstRunGuess)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    TempDir t;
    AppSettings s;
    s.uiMode = UiMode::Dumb;
    writeSettings (t.file(), s);

    MainComponent mc;
    mc.enableSettings (t.file());
    EXPECT_EQ (mc.getUiMode(), UiMode::Dumb);

    mc.setUiMode (UiMode::Geek);
    EXPECT_EQ (appSettingsFromHost (az::host::loadHostSettings (t.file()).settings).uiMode,
               UiMode::Geek);
}

TEST (LaneU, RestoredRoutingLandsInTheStoppedEngineAndZeroChannelsDisableNothing)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    TempDir t;
    writeSettings (t.file(), routedSettings());

    MainComponent mc;
    mc.enableSettings (t.file());

    auto& engine = mc.getAudioEngine();
    EXPECT_TRUE (engine.getSlotConfig (0).enabled);
    EXPECT_EQ   (engine.getSlotConfig (0).width, 1);
    EXPECT_EQ   (engine.getSlotConfig (0).inputChannels[0], 1);
    EXPECT_TRUE (engine.getSlotConfig (1).enabled);

    // B-1: the headless engine reports 0 channels. Nothing may be judged
    // against that -- the timer's guard never calls the check, and the
    // check itself refuses a zero count.
    EXPECT_EQ (engine.getNumInputChannels(), 0);
    EXPECT_TRUE (engine.getSlotConfig (1).enabled);
}

TEST (LaneU, RestoredSlotThatDoesNotFitTheDeviceIsDisabledNotClamped)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    TempDir t;
    writeSettings (t.file(), routedSettings());

    MainComponent mc;
    mc.enableSettings (t.file());
    mc.validateRestoredSlotsForTest (2, 2);

    auto& engine = mc.getAudioEngine();
    EXPECT_TRUE  (engine.getSlotConfig (0).enabled);
    EXPECT_FALSE (engine.getSlotConfig (1).enabled);
    EXPECT_EQ    (engine.getSlotConfig (1).outputChannels[0], 4);   // not clamped
    EXPECT_FALSE (mc.lastMessageForTest().isEmpty());
}

TEST (LaneU, DeviceFallbackTakesTheRoutingOffAndKeepsTheMemory)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    TempDir t;
    const auto saved = routedSettings();
    writeSettings (t.file(), saved);

    MainComponent mc;
    mc.enableSettings (t.file());
    mc.checkRestoredDeviceForTest ("ASIO", "Some Other Card");

    EXPECT_TRUE (mc.deviceMismatchForTest());
    for (int i = 0; i < kMaxSlots; ++i)
        EXPECT_FALSE (mc.getAudioEngine().getSlotConfig (i).enabled) << i;
    EXPECT_FALSE (mc.dumbInputsForTest().configured);

    // The memory survives: device AND routing as they were read.
    ASSERT_TRUE (mc.saveSettings());
    const auto back = appSettingsFromHost (az::host::loadHostSettings (t.file()).settings);
    EXPECT_EQ (back.device, saved.device);
    ASSERT_TRUE (back.hasRouting);
    EXPECT_TRUE (back.slots[0].enabled);
    EXPECT_TRUE (back.slots[1].enabled);
}

TEST (LaneU, SameDeviceIsNotAMismatch)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    TempDir t;
    writeSettings (t.file(), routedSettings());

    MainComponent mc;
    mc.enableSettings (t.file());
    mc.checkRestoredDeviceForTest ("ASIO", "Saved Card");

    EXPECT_FALSE (mc.deviceMismatchForTest());
    EXPECT_TRUE (mc.getAudioEngine().getSlotConfig (0).enabled);
}

TEST (LaneU, NoRunningDeviceNeverOverwritesTheSavedDevice)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    TempDir t;
    const auto saved = routedSettings();
    writeSettings (t.file(), saved);

    MainComponent mc;
    mc.enableSettings (t.file());
    mc.setUiMode (UiMode::Geek);   // a save, with no device open

    const auto back = appSettingsFromHost (az::host::loadHostSettings (t.file()).settings);
    EXPECT_EQ (back.device, saved.device);
    EXPECT_TRUE (back.slots[1].enabled);
    EXPECT_EQ (back.uiMode, UiMode::Geek);
}

TEST (LaneU, ANewerBuildsFileIsNeverOverwritten)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    TempDir t;
    const juce::String newer = R"({"schema":99,"product":{"uiMode":"geek"}})";
    ASSERT_TRUE (t.file().replaceWithText (newer));

    MainComponent mc;
    mc.enableSettings (t.file());
    EXPECT_FALSE (mc.saveSettings());
    mc.setUiMode (UiMode::Geek);
    EXPECT_EQ (t.file().loadFileAsString(), newer);
}

//==============================================================================
// Spec 3.6: the wizard's input peak meter.

namespace
{
struct Block
{
    std::vector<float> l, r, outL, outR;
    const float* ins[2] {};
    float* outs[2] {};

    Block (float peakL, float peakR, int n = 256)
        : l ((std::size_t) n, 0.0f), r ((std::size_t) n, 0.0f),
          outL ((std::size_t) n, 0.0f), outR ((std::size_t) n, 0.0f)
    {
        l[10] = -peakL;   // negative on purpose: the meter reads |x|
        r[20] =  peakR;
        ins[0] = l.data(); ins[1] = r.data();
        outs[0] = outL.data(); outs[1] = outR.data();
    }

    void run (AudioEngine& e, int numIns = 2)
    {
        const juce::AudioIODeviceCallbackContext ctx {};
        e.audioDeviceIOCallbackWithContext (ins, numIns, outs, 2, (int) l.size(), ctx);
    }
};
}

TEST (LaneUInputMeter, OffMeansNothingIsMeasured)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    Block b (0.5f, 0.25f);
    b.run (engine);
    EXPECT_EQ (engine.takeInputPeak (0), 0.0f);
    EXPECT_EQ (engine.takeInputPeak (1), 0.0f);
}

TEST (LaneUInputMeter, OnReportsTheAbsolutePeakAndTakeResetsIt)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    engine.setInputMeterEnabled (true);

    Block b (0.5f, 0.25f);
    b.run (engine);
    EXPECT_FLOAT_EQ (engine.takeInputPeak (0), 0.5f);
    EXPECT_FLOAT_EQ (engine.takeInputPeak (1), 0.25f);
    EXPECT_EQ (engine.takeInputPeak (0), 0.0f);

    // Out of range and channels the callback was not handed read 0.
    EXPECT_EQ (engine.takeInputPeak (-1), 0.0f);
    EXPECT_EQ (engine.takeInputPeak (AudioEngine::kInputMeterChannels), 0.0f);

    Block one (0.1f, 0.9f);
    one.run (engine, 1);
    EXPECT_FLOAT_EQ (engine.takeInputPeak (0), 0.1f);
    EXPECT_EQ (engine.takeInputPeak (1), 0.0f);
}

TEST (LaneUInputMeter, NullChannelPointerIsSkipped)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    engine.setInputMeterEnabled (true);

    Block b (0.5f, 0.25f);
    b.ins[0] = nullptr;
    b.run (engine);
    EXPECT_EQ (engine.takeInputPeak (0), 0.0f);
    EXPECT_FLOAT_EQ (engine.takeInputPeak (1), 0.25f);
}

TEST (LaneUInputMeter, TheMeterNeverChangesAnOutputSample)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine off, on;
    on.setInputMeterEnabled (true);

    Block a (0.5f, 0.25f), b (0.5f, 0.25f);
    a.run (off);
    b.run (on);
    EXPECT_EQ (a.outL, b.outL);
    EXPECT_EQ (a.outR, b.outR);
}

//==============================================================================
// Spec 3.5: the wizard as MainComponent wires it.

namespace
{
SlotConfig monoRoute (int in, int out)
{
    SlotConfig c;
    c.enabled = true; c.width = 1;
    c.inputChannels[0] = in; c.outputChannels[0] = out;
    return c;
}
}

TEST (LaneUWizard, OpensOverEverythingAndCannotLeaveStepOneWithoutChannels)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent mc;
    mc.setSize (MainComponent::kDefaultWidth, MainComponent::kDefaultHeight);
    mc.setUiMode (UiMode::Dumb);
    mc.getDumbViewForTest().onOpenWizard();

    auto& w = mc.getSetupWizardForTest();
    EXPECT_TRUE (w.isOpen());
    EXPECT_EQ (mc.getIndexOfChildComponent (&w), mc.getNumChildComponents() - 1);
    EXPECT_EQ (w.getModelForTest().getStep(), SetupWizardModel::Step::Device);
    EXPECT_FALSE (w.getNextButtonForTest().isEnabled());   // headless: 0 channels
}

TEST (LaneUWizard, RefusedWhileAMeasurementHoldsTheConsole)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent mc;
    mc.setSoundcheckLockForTest (MainComponent::SoundcheckLock::Measuring);
    mc.openSetupWizard();
    EXPECT_FALSE (mc.getSetupWizardForTest().isOpen());
    EXPECT_FALSE (mc.lastMessageForTest().isEmpty());
}

TEST (LaneUWizard, FinishWithOneSlotOnWritesSlotZeroWithoutAsking)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent mc;
    bool asked = false;
    mc.wizardConfirmHook = [&] (const juce::String&, std::function<void (bool)>) { asked = true; };

    mc.openSetupWizard();
    mc.getSetupWizardForTest().onFinish (monoRoute (1, 0));

    EXPECT_FALSE (asked);
    const auto s0 = mc.getAudioEngine().getSlotConfig (0);
    EXPECT_TRUE (s0.enabled);
    EXPECT_EQ (s0.width, 1);
    EXPECT_EQ (s0.inputChannels[0], 1);
    EXPECT_FALSE (mc.getSetupWizardForTest().isOpen());
}

TEST (LaneUWizard, FinishOverSeveralSlotsAsksAndOnlyYesOverwrites)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent mc;
    mc.changeSlotConfig (2, monoRoute (0, 1));
    ASSERT_TRUE (mc.getAudioEngine().getSlotConfig (0).enabled);   // default slot 0
    ASSERT_TRUE (mc.getAudioEngine().getSlotConfig (2).enabled);

    std::function<void (bool)> answer;
    juce::String question;
    mc.wizardConfirmHook = [&] (const juce::String& text, std::function<void (bool)> a)
    {
        question = text;
        answer   = std::move (a);
    };

    mc.openSetupWizard();
    mc.getSetupWizardForTest().onFinish (monoRoute (1, 1));
    ASSERT_TRUE (answer != nullptr);
    EXPECT_TRUE (question.contains ("2"));

    answer (false);
    EXPECT_TRUE (mc.getAudioEngine().getSlotConfig (2).enabled);
    EXPECT_TRUE (mc.getSetupWizardForTest().isOpen());

    mc.getSetupWizardForTest().onFinish (monoRoute (1, 1));
    answer (true);
    EXPECT_FALSE (mc.getAudioEngine().getSlotConfig (2).enabled);
    EXPECT_EQ (mc.getAudioEngine().getSlotConfig (0).inputChannels[0], 1);
    EXPECT_FALSE (mc.getSetupWizardForTest().isOpen());
}

TEST (LaneUWizard, CancelWritesNoRouting)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent mc;
    const auto before = mc.getAudioEngine().getSlotConfig (0);

    mc.openSetupWizard();
    mc.getSetupWizardForTest().getCancelButtonForTest().onClick();

    EXPECT_FALSE (mc.getSetupWizardForTest().isOpen());
    const auto after = mc.getAudioEngine().getSlotConfig (0);
    EXPECT_EQ (after.enabled, before.enabled);
    EXPECT_EQ (after.inputChannels[0], before.inputChannels[0]);
}

TEST (LaneUWizard, LeavingForGeekCancelsTheWizard)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent mc;
    mc.setUiMode (UiMode::Dumb);
    mc.openSetupWizard();
    mc.setUiMode (UiMode::Geek);
    EXPECT_FALSE (mc.getSetupWizardForTest().isOpen());
}

TEST (LaneUWizard, FinishedWizardIsRememberedAndClearsAMismatch)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    TempDir t;
    writeSettings (t.file(), routedSettings());

    MainComponent mc;
    mc.enableSettings (t.file());
    mc.checkRestoredDeviceForTest ("ASIO", "Fallback");
    ASSERT_TRUE (mc.deviceMismatchForTest());

    mc.openSetupWizard();
    mc.wizardConfirmHook = [] (const juce::String&, std::function<void (bool)> a) { a (true); };
    mc.getSetupWizardForTest().onFinish (monoRoute (0, 1));

    EXPECT_FALSE (mc.deviceMismatchForTest());
    EXPECT_TRUE (mc.getAudioEngine().getSlotConfig (0).enabled);
}

TEST (LaneUWizard, AnOldConfirmationAnsweredAfterReopeningIsDropped)
{
    // Verification I-3: XONG (box opens) -> HUỶ -> reopen -> OK on the old box.
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent mc;
    mc.changeSlotConfig (2, monoRoute (0, 1));

    std::vector<std::function<void (bool)>> answers;
    mc.wizardConfirmHook = [&] (const juce::String&, std::function<void (bool)> a)
    {
        answers.push_back (std::move (a));
    };

    mc.openSetupWizard();
    auto& w = mc.getSetupWizardForTest();
    w.onFinish (monoRoute (1, 1));
    w.onFinish (monoRoute (1, 1));             // second XONG: no second box
    ASSERT_EQ (answers.size(), 1u);

    w.getCancelButtonForTest().onClick();
    mc.openSetupWizard();
    answers[0] (true);

    EXPECT_TRUE (mc.getAudioEngine().getSlotConfig (2).enabled);   // untouched
    EXPECT_TRUE (w.isOpen());
}

TEST (LaneUWizard, CancelPutsTheMemoryBackAsItWas)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    TempDir t;
    const auto saved = routedSettings();
    writeSettings (t.file(), saved);

    MainComponent mc;
    mc.enableSettings (t.file());
    mc.checkRestoredDeviceForTest ("ASIO", "Fallback");

    mc.openSetupWizard();
    mc.getSetupWizardForTest().getCancelButtonForTest().onClick();

    EXPECT_TRUE (mc.deviceMismatchForTest());
    ASSERT_TRUE (mc.saveSettings());
    const auto back = appSettingsFromHost (az::host::loadHostSettings (t.file()).settings);
    EXPECT_EQ (back.device, saved.device);
    EXPECT_TRUE (back.slots[1].enabled);
}
