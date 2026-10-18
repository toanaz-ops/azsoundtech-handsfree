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
