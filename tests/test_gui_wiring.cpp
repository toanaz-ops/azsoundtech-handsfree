// GUI wiring tests -- Task 23 (mode buttons) and the defect underneath all of
// Tasks 16-18.
//
// THE DEFECT THESE EXIST FOR
// ==========================
// Until this lane, `AudioEngine` was never instantiated anywhere in the
// application. Tasks 8 and 9 built the engine and its tap, 75 tests passed, and
// the shipped binary was an 800x600 window that drew a string and processed no
// audio whatsoever. Nothing in the suite could notice, because every test
// constructed its own AudioEngine.
//
// TheApplicationActuallyOwnsAnAudioEngine below is the test that would have
// caught it. It is deliberately the first one in the file.
//
// Why a Component can be built with no window
// ===========================================
// juce::ScopedJuceInitialiser_GUI brings up the MessageManager; a Component
// that is never added to the desktop needs nothing else. Button clicks are
// driven with sendNotificationSync, which JUCE dispatches inline
// (juce_Button.cpp:199-202), so no message loop has to be pumped.

#include <gtest/gtest.h>

#include "app/MainComponent.h"
#include "app/NotchController.h"
#include "app/PresetManager.h"
#include "gui/DeviceDrawer.h"
#include "gui/theme/AzTheme.h"
#include "gui/DevicePanel.h"
#include "gui/ModeBar.h"
#include "test_gui_helpers.h"

#include <cmath>
#include <vector>

//==============================================================================
// Task 23 -- mode buttons. Auto and Bypass in full; Soundcheck sets the mode
// but deliberately runs no countdown (owner decision D-06 -- see ModeBar.cpp).

TEST (ModeBar, ClickingAutoRequestsAutoMode)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    gui::ModeBar bar;
    AudioEngine::Mode requested = AudioEngine::Mode::Bypass;
    bar.onModeRequested = [&requested] (AudioEngine::Mode m) { requested = m; };

    bar.autoButton.setToggleState (true, juce::sendNotificationSync);

    EXPECT_EQ (requested, AudioEngine::Mode::Auto);
}

TEST (ModeBar, ClickingBypassRequestsBypassMode)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    gui::ModeBar bar;
    AudioEngine::Mode requested = AudioEngine::Mode::Auto;
    bar.onModeRequested = [&requested] (AudioEngine::Mode m) { requested = m; };

    bar.bypassButton.setToggleState (true, juce::sendNotificationSync);

    EXPECT_EQ (requested, AudioEngine::Mode::Bypass);
}

TEST (ModeBar, ClickingSoundcheckRequestsSoundcheckMode)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    gui::ModeBar bar;
    AudioEngine::Mode requested = AudioEngine::Mode::Bypass;
    bar.onModeRequested = [&requested] (AudioEngine::Mode m) { requested = m; };

    bar.soundcheckButton.setToggleState (true, juce::sendNotificationSync);

    EXPECT_EQ (requested, AudioEngine::Mode::Soundcheck);
}

TEST (ModeBar, ReflectingTheEnginesModeDoesNotRequestAModeChange)
{
    // setDisplayedMode() is called from the status refresh timer, several times
    // a second. If it fired onModeRequested, the refresh would keep re-issuing
    // the mode -- and a mode set anywhere other than these buttons would be
    // fought by the GUI on the next tick.
    const juce::ScopedJuceInitialiser_GUI juceInit;

    gui::ModeBar bar;
    int requests = 0;
    bar.onModeRequested = [&requests] (AudioEngine::Mode) { ++requests; };

    bar.setDisplayedMode (AudioEngine::Mode::Auto);

    EXPECT_EQ (requests, 0);
    EXPECT_TRUE (bar.autoButton.getToggleState());
}

TEST (ModeBar, TheActiveModeIsVisuallyDistinguishableFromTheInactiveOnes)
{
    // Found by running the app and looking at it. JUCE's default LookAndFeel
    // draws buttonOnColourId close enough to the off state on a dark scheme
    // that all three buttons looked identical, and the live mode was
    // unreadable. Bypass versus Auto is the difference between notches being
    // applied to a live PA and not; it has to be legible at a glance.
    const juce::ScopedJuceInitialiser_GUI juceInit;

    gui::ModeBar bar;

    for (const auto* button : { &bar.soundcheckButton, &bar.autoButton, &bar.bypassButton })
    {
        EXPECT_TRUE (button->isColourSpecified (juce::TextButton::buttonOnColourId));
        EXPECT_NE (button->findColour (juce::TextButton::buttonOnColourId),
                   button->findColour (juce::TextButton::buttonColourId));
    }
}

//==============================================================================
// The application object itself.

TEST (MainComponent, TheApplicationActuallyOwnsAnAudioEngine)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    MainComponent app;

    // Reaching the engine at all is the assertion. Before this lane there was
    // no engine to reach: `grep -rn AudioEngine src/main.cpp src/app/MainComponent.*`
    // returned nothing.
    EXPECT_EQ (app.getAudioEngine().getMode(), AudioEngine::Mode::Bypass);
}

TEST (MainComponent, ConstructionOpensNoAudioDevice)
{
    // Opening a device is an explicit startAudio() call, not a constructor side
    // effect. That is what lets this test -- and any future one -- construct the
    // whole application object on a build machine with no audio hardware.
    const juce::ScopedJuceInitialiser_GUI juceInit;

    MainComponent app;

    EXPECT_FALSE (app.getAudioEngine().isRunning());
    EXPECT_TRUE (app.getAudioEngine().getCurrentDeviceName().isEmpty());
}

TEST (MainComponent, RequestingAModeReachesTheEngine)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    MainComponent app;

    app.requestMode (AudioEngine::Mode::Auto);

    EXPECT_EQ (app.getAudioEngine().getMode(), AudioEngine::Mode::Auto);
}

//==============================================================================
// Task 6 -- MainComponent builds stereo controllers and carries `linked`.

// Lane S. Red if the app wires controllers with one tap (legacy ctor ⇒ LINKED
// forever) or if setSlotLinked stops reaching the controller.
TEST (MainComponent, ControllersGetBothTapsAndFollowTheLinkFlag)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent mc;
    for (int s = 0; s < kMaxSlots; ++s)
        EXPECT_TRUE (mc.getControllerForTest (s).hasLaneOneTapForTest()) << "slot " << s;

    EXPECT_FALSE (mc.isSlotLinked (2));
    mc.setSlotLinked (2, true);
    EXPECT_TRUE (mc.isSlotLinked (2));
    EXPECT_TRUE (mc.getControllerForTest (2).isLinked());
    EXPECT_FALSE (mc.getControllerForTest (1).isLinked());
}

// Red if loadPreset stops applying a slot's "linked" before adopting notches.
TEST (MainComponent, LoadPresetAppliesTheSlotsLinkedFlag)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent mc;
    Preset p;
    p.version = PresetManager::CURRENT_VERSION; p.device = "test"; p.sampleRate = 48000.0; p.bufferSize = 512;
    PresetSlot s; s.index = 3; s.config.enabled = true; s.config.width = 2; s.linked = true;
    p.slots.push_back (s);
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("lane-s-linked.json");
    ASSERT_TRUE (file.replaceWithText (PresetManager::toJSON (p)));
    ASSERT_TRUE (mc.loadPreset (file));
    EXPECT_TRUE (mc.isSlotLinked (3));
    EXPECT_TRUE (mc.getControllerForTest (3).isLinked());
    file.deleteFile();
}

//==============================================================================
// Single Classic layout (2026-08-25 owner decision): the L1/L2 switching
// mechanism is gone. The drawer is always open with no collapse gear, and
// there is no setLayout API left to call.

TEST (MainComponent, DrawerIsAlwaysOpenWithFullHeight)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    MainComponent app;
    app.setSize (900, 900);
    app.resized();

    auto& drawer = app.getDeviceDrawer();
    // Full height now includes the PRESET row (caption + LOAD.../SAVE...)
    // added below the wrapped DevicePanel -- see getPreferredHeight().
    EXPECT_EQ (drawer.getBounds().getHeight(),
               gui::DeviceDrawer::kHeaderHeight + gui::DeviceDrawer::kContentHeight
                 + gui::DeviceDrawer::kPresetCaptionHeight
                 + gui::DeviceDrawer::kPresetRowHeight);

    // The wrapped DevicePanel always has a real rect -- never squeezed out.
    const auto wrapped = drawer.wrappedBoundsForTest();
    EXPECT_FALSE (wrapped.isEmpty());
    EXPECT_EQ (wrapped.getHeight(), gui::DeviceDrawer::kContentHeight);
}

TEST (MainComponent, ClickingAddSlotGrowsTheTableImmediately)
{
    // The 2026-08-25 bug: "+ Add slot" only took effect after cycling the
    // L1/L2 layouts. Root cause: SlotPanel::setVisibleRowCount resized its
    // DIRECT parent -- the Viewport, whose resized() never re-sizes the
    // content. Only MainComponent::resized() hands slotPanel_ its size, so
    // the height change must reach THIS component's resized() immediately.
    const juce::ScopedJuceInitialiser_GUI juceInit;

    MainComponent app;

    app.setSize (900, 900);
    app.resized();   // headless: no peer, so drive the layout pass directly

    const int twoRowHeight = app.slotTableBoundsForTest().getHeight();
    EXPECT_GT (twoRowHeight, 0);

    // The Add button's own path: reveal row 3. The table must be taller NOW,
    // with no further layout pass and no layout switching.
    app.getSlotPanelForTest().setVisibleRowCount (3);

    const auto table = app.slotTableBoundsForTest();
    EXPECT_EQ (table.getHeight(),
               gui::SlotPanel::kCaptionHeight
                 + 4 * gui::SlotPanel::kRowHeight
                 + 3 * az::theme::spacing);
    EXPECT_GT (table.getHeight(), twoRowHeight);
}

TEST (MainComponent, SlotRoutingTableGetsSizedContentInsideTheViewport)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    MainComponent app;

    app.setSize (900, 900);
    app.resized();   // headless: no peer, so drive the layout pass directly

    // The Viewport bug (2026-08-24) left the table a 0x0 rect -- an empty
    // black band where the routing controls should be. Default shows 2 rows
    // plus the Add row; the content must be sized to exactly that.
    const auto table = app.slotTableBoundsForTest();
    EXPECT_EQ (table.getHeight(),
               gui::SlotPanel::kCaptionHeight
                 + 3 * gui::SlotPanel::kRowHeight
                 + 3 * az::theme::spacing);
    EXPECT_GT (table.getWidth(), 0);
}

TEST (MainComponent, MinimumSizeKeepsRailAndSpectrumDisjoint)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    MainComponent app;

    app.setSize (MainComponent::kMinimumWidth, MainComponent::kMinimumHeight);
    app.resized();

    const auto rail     = app.railBoundsForTest();
    const auto spectrum = app.spectrumBoundsForTest();

    // Both must actually be laid out before the non-overlap claim means
    // anything.
    EXPECT_FALSE (rail.isEmpty());
    EXPECT_FALSE (spectrum.isEmpty());

    // The binding assertion: zero intersecting pixels between the fixed
    // rail and the spectrum at the smallest window we allow.
    const auto overlap = rail.getIntersection (spectrum);
    EXPECT_EQ (overlap.getWidth() * overlap.getHeight(), 0);
}

//==============================================================================
// Tasks 16 and 17 -- the panel, with NO device open. That is the state the GUI
// is in at startup, and every AudioEngine query used here is documented safe in
// it.

TEST (DevicePanel, WithNoDeviceOpenTheRateAndBufferCombosAreEmptyRatherThanGuessed)
{
    // getAvailableSampleRates() and getAvailableBufferSizes() are empty until a
    // device is open. Filling these from a hardcoded list of "standard" rates
    // would be a plausible-looking improvement and a lie: it would offer the
    // user rates this device may not support, and the offer would come from us
    // rather than from the hardware.
    const juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;
    gui::DevicePanel panel (engine);

    panel.refresh();

    EXPECT_EQ (panel.sampleRateBox.getNumItems(), 0);
    EXPECT_EQ (panel.bufferSizeBox.getNumItems(), 0);
}

TEST (DevicePanel, TheTypeShownIsOneTheManagerHasRatherThanTheOneRequested)
{
    // AudioEngine::start() documents that JUCE silently keeps the current
    // device type when the requested one is not registered -- which is what
    // happens to "ASIO" on every machine this project currently builds on. A
    // panel that echoed the REQUEST would tell the user ASIO was running while
    // WASAPI actually was.
    const juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;
    engine.setAudioDeviceType ("ASIO");   // desired; registered or not

    gui::DevicePanel panel (engine);
    panel.refresh();

    const juce::String shown = panel.deviceTypeBox.getText();

    if (shown.isNotEmpty())
        EXPECT_TRUE (engine.getAvailableDeviceTypeNames().contains (shown));
}

TEST (DevicePanel, RefreshOffersTheDriverTypesTheMachineActuallyHas)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;
    gui::DevicePanel panel (engine);

    panel.refresh();

    // Windows always registers at least one type; the point is that refresh()
    // populated from the engine at all, rather than leaving an empty combo the
    // user cannot choose a device from.
    EXPECT_EQ (panel.deviceTypeBox.getNumItems(),
               engine.getAvailableDeviceTypeNames().size());
}

//==============================================================================
// Task 6 -- detector thread lifecycle. The controller's poll loop is joined
// BEFORE the engine tears down (bridge design §6.5); these tests fail by
// hanging or by a leak-detector hit if that ordering is wrong.

TEST (MainComponent, ControllerLifecycleSurvivesConstructionDestruction)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    {
        // Same pattern as every other MainComponent test in this file: built
        // directly, never added to a desktop window. No message pump needed:
        // the constructor wires callbacks but posts no messages, and
        // runDispatchLoopUntil() is unavailable with JUCE_MODAL_LOOPS_
        // PERMITTED off.
        MainComponent app;
    }
    // Destruction runs notchControllers_' destructors (each joins its own
    // thread) BEFORE engine_, reverse declaration order. A hang here means
    // stop() failed to join; a leak-detector hit means something was left
    // running.
    SUCCEED();
}

// Task 6 structural test: the 8-controller array must construct with no device
// open (the state this machine's tests are always in) and destroy cleanly --
// eight joined threads, not one.
TEST (MainComponent, EightControllerArrayConstructsWithoutDeviceAndDestroysCleanly)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    {
        MainComponent app;

        EXPECT_FALSE (app.getAudioEngine().isRunning());

        // Exercising the hooks' code paths without a device is safe: every
        // engine query they use guards the null-device case.
        app.requestMode (AudioEngine::Mode::Bypass);
    }
    SUCCEED();
}

TEST (NotchControllerThread, StartStopCycleJoinsCleanly)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    LockFreeRingBuffer<float> tap { 8192 };
    LockFreeRingBuffer<NotchCommand> commands { 128 };
    JuceMonotonicClock clock;
    NotchController controller { tap, commands, clock };

    controller.start();
    std::vector<float> hop (512, 0.1f);
    for (int i = 0; i < 50; ++i)
    {
        tap.write (hop.data(), hop.size());
        juce::Thread::sleep (5);
    }
    controller.stop (2000);

    // juce::Thread is inherited PRIVATELY, so isThreadRunning() is not visible
    // here by design. Liveness is asserted through observable effects: the
    // poll loop published frames, and stop() returning at all means the join
    // completed within its 2 s budget.
    NotchController::SnapshotBuffer snap {};
    controller.copySnapshot (snap);
    EXPECT_GT (snap.sequence, 0u);
}

//==============================================================================
// Task 6B -- preset loading through the CHANNEL-AWARE path, and detection
// gating per enabled slot. With no audio device open the engine reports zero
// channels, so the loader must fall back to stereo; and since nothing drains
// a controller's outbox headlessly, each test pumps runOnce() once and reads
// the ENGINE's per-slot command queues: whatever lands there is what the
// audio thread would consume.

namespace
{
juce::String makeSlotAwarePresetJson()
{
    // One legacy notch (slot 0 implied, as every v1 file) plus one explicitly
    // routed to slot 3, whose mono config the file declares.
    return R"({"version":"1.0","device":"","sampleRate":48000,"bufferSize":256,)"
           R"("slots":[{"index":3,"enabled":true,"width":1,"inputChannels":[1,0],"outputChannels":[1,0]}],)"
           R"("notches":[{"index":0,"freq":482.0,"Q":30.0,"depth":-12.0},)"
           R"({"index":1,"freq":2500.0,"Q":25.0,"depth":-10.0,"slot":3}]})";
}
} // namespace

TEST (MainComponent, SlotAwarePresetLoadRoutesConfigsAndNotchesPerSlot)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    MainComponent app;

    auto presetFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("az-handsfree-task6b-routing.json");
    ASSERT_TRUE (presetFile.replaceWithText (makeSlotAwarePresetJson()));

    EXPECT_TRUE (app.loadPreset (presetFile));
    presetFile.deleteFile();

    // The declared routing config reached the ENGINE...
    const auto slot3 = app.getAudioEngine().getSlotConfig (3);
    EXPECT_TRUE (slot3.enabled);
    EXPECT_EQ (slot3.width, 1);

    // ...and the loader's fallback channel counts (no device -> 2/2) clamped
    // lane 0 of slot 3 from the file's input 1 / output 1 into range unchanged.
    EXPECT_EQ (slot3.inputChannels[0], 1);
    EXPECT_EQ (slot3.outputChannels[0], 1);

    // Slot 0 was only IMPLIED (legacy notch): the loader auto-activated it
    // with the stereo config every v1 file means.
    const auto slot0 = app.getAudioEngine().getSlotConfig (0);
    EXPECT_TRUE (slot0.enabled);
    EXPECT_EQ (slot0.width, 2);

    // Each notch landed in ITS SLOT's controller: pumping that controller
    // flushes its outbox into the engine's per-slot command queue.
    auto* controller3 = app.getNotchControllerForTest (3);
    ASSERT_NE (controller3, nullptr);
    controller3->runOnce();

    auto& queue3 = app.getAudioEngine().getCommandQueue (3);
    ASSERT_EQ (queue3.getAvailableRead(), 1u);   // width 1 -> one lane
    NotchCommand cmd3 {};
    ASSERT_EQ (queue3.read (&cmd3, 1), 1u);
    EXPECT_EQ (cmd3.type, NotchCommandType::Set);
    EXPECT_EQ (cmd3.index, 1);
    EXPECT_FLOAT_EQ (cmd3.frequency, 2500.0f);

    auto* controller0 = app.getNotchControllerForTest (0);
    ASSERT_NE (controller0, nullptr);
    controller0->runOnce();

    auto& queue0 = app.getAudioEngine().getCommandQueue (0);
    ASSERT_EQ (queue0.getAvailableRead(), 2u);   // width 2 -> both lanes
    NotchCommand cmd0a {};
    NotchCommand cmd0b {};
    ASSERT_EQ (queue0.read (&cmd0a, 1), 1u);
    ASSERT_EQ (queue0.read (&cmd0b, 1), 1u);
    EXPECT_EQ (cmd0a.type, NotchCommandType::Set);
    EXPECT_EQ (cmd0b.type, NotchCommandType::Set);
    EXPECT_FLOAT_EQ (cmd0a.frequency, 482.0f);
    EXPECT_FLOAT_EQ (cmd0b.frequency, 482.0f);
}

TEST (MainComponent, PresetWithOutOfRangeSlotStillLoadsTheRest)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    MainComponent app;

    // slot 9 is outside [0, kMaxSlots): the channel-aware load SKIPS that
    // notch and counts it -- never refuses the file.
    const juce::String json = R"({"version":"1.0","device":"","sampleRate":48000,"bufferSize":256,)"
                              R"("notches":[{"index":0,"freq":482.0,"Q":30.0,"depth":-12.0},)"
                              R"({"index":1,"freq":900.0,"Q":30.0,"depth":-12.0,"slot":9}]})";

    auto presetFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("az-handsfree-task6b-skipped.json");
    ASSERT_TRUE (presetFile.replaceWithText (json));

    EXPECT_TRUE (app.loadPreset (presetFile));
    presetFile.deleteFile();

    // The surviving notch still reached slot 0's controller.
    auto* controller0 = app.getNotchControllerForTest (0);
    ASSERT_NE (controller0, nullptr);
    controller0->runOnce();

    auto& queue0 = app.getAudioEngine().getCommandQueue (0);
    ASSERT_GE (queue0.getAvailableRead(), 2u);
    NotchCommand cmd {};
    ASSERT_EQ (queue0.read (&cmd, 1), 1u);
    EXPECT_FLOAT_EQ (cmd.frequency, 482.0f);
}

TEST (MainComponent, PresetReferencedSlotIsEnabledOnEngineAndAdoptedTogether)
{
    // Spec §6 auto-activation pinned from BOTH sides: a notch routed to a
    // slot the file never declares must switch that slot ON in the ENGINE
    // (enabled flag) AND land in its CONTROLLER. Either alone is half a
    // protection claim -- the chain would be silent or the detector deaf.
    const juce::ScopedJuceInitialiser_GUI juceInit;

    MainComponent app;

    // No "slots" section at all: slot 5 exists only because the notch
    // references it.
    const juce::String json = R"({"version":"1.0","device":"","sampleRate":48000,"bufferSize":256,)"
                              R"("notches":[{"index":2,"freq":1250.0,"Q":30.0,"depth":-12.0,"slot":5}]})";

    auto presetFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("az-handsfree-task6b-activate.json");
    ASSERT_TRUE (presetFile.replaceWithText (json));

    EXPECT_TRUE (app.loadPreset (presetFile));
    presetFile.deleteFile();

    EXPECT_TRUE (app.getAudioEngine().getSlotConfig (5).enabled);

    auto* controller5 = app.getNotchControllerForTest (5);
    ASSERT_NE (controller5, nullptr);
    controller5->runOnce();

    auto& queue5 = app.getAudioEngine().getCommandQueue (5);
    ASSERT_GE (queue5.getAvailableRead(), 1u);   // default width 2 -> up to 2 lanes
    NotchCommand cmd {};
    ASSERT_EQ (queue5.read (&cmd, 1), 1u);
    EXPECT_EQ (cmd.type, NotchCommandType::Set);
    EXPECT_FLOAT_EQ (cmd.frequency, 1250.0f);
}

TEST (MainComponent, EnablingASlotMidModeArmsOrDisarmsImmediately)
{
    // A slot enabled while a mode is ALREADY running must not wait for the
    // next mode request: the restart cycle restores width and threads but
    // not the detection gate. Auto arms it; Bypass keeps it disarmed.
    const juce::ScopedJuceInitialiser_GUI juceInit;

    MainComponent app;

    app.requestMode (AudioEngine::Mode::Auto);

    SlotConfig enableStereo;
    enableStereo.enabled = true;
    enableStereo.width   = 2;
    app.changeSlotConfig (3, enableStereo);

    EXPECT_TRUE (app.getNotchControllerForTest (3)->detectionActiveForTest());

    app.requestMode (AudioEngine::Mode::Bypass);
    app.changeSlotConfig (4, enableStereo);
    EXPECT_FALSE (app.getNotchControllerForTest (4)->detectionActiveForTest());

    // Mid-Soundcheck: the new slot gets its own 15 s live-time window (each
    // controller measures its own live clock, D-06 -- there is no shared
    // window to inherit a remainder from).
    app.requestMode (AudioEngine::Mode::Soundcheck);
    app.changeSlotConfig (5, enableStereo);
    EXPECT_GT (app.getNotchControllerForTest (5)->getSoundcheckRemainingMs(), 0.0);
}

TEST (MainComponent, SoundcheckGatingAppliesToEnabledSlotsOnly)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    MainComponent app;

    // Fresh engine: only slot 0 is enabled, slots 1..7 are not.
    EXPECT_TRUE (app.getAudioEngine().getSlotConfig (0).enabled);
    EXPECT_FALSE (app.getAudioEngine().getSlotConfig (1).enabled);

    app.requestMode (AudioEngine::Mode::Soundcheck);

    // Enabled slot: detection armed for the 15 s live-time window...
    EXPECT_GT (app.getNotchControllerForTest (0)->getSoundcheckRemainingMs(), 0.0);
    // ...disabled slot: its controller stays silent.
    EXPECT_DOUBLE_EQ (app.getNotchControllerForTest (1)->getSoundcheckRemainingMs(), 0.0);

    // And Bypass disarms again through the same route.
    app.requestMode (AudioEngine::Mode::Bypass);
    SUCCEED();
}

//==============================================================================
// Per-slot tuning (brief 2026-08-24): a slot switched to Custom keeps its own
// detector parameters when the global DETECTION strip changes; Global slots
// still follow the strip.

TEST (MainComponent, GlobalTuningChangeSkipsCustomSlots)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    MainComponent app;
    auto& panel = app.getSlotPanelForTest();

    // Switch slot 3 to Custom through the panel's own path -- MainComponent's
    // wiring records the flag and seeds the controller from its own values.
    panel.getRowForTest (3).tune.setSelectedId (2 /* C */, juce::sendNotificationSync);

    // Edit the custom set: rise 250 ms -> 750 ms (combo id 4).
    panel.getDetailForTest (3).rise.setSelectedId (4, juce::sendNotificationSync);

    auto* custom = app.getNotchControllerForTest (3);
    ASSERT_NE (custom, nullptr);
    EXPECT_DOUBLE_EQ (custom->getRiseReferenceMs(), 750.0);
    // The provider reads back what the controller holds.
    const auto seeded = panel.slotTuningProvider (3);
    EXPECT_FALSE (seeded.usesGlobal);
    EXPECT_DOUBLE_EQ (seeded.riseMs, 750.0);

    // A change on the global DETECTION strip: rise -> 1000 ms (id 5).
    app.getTuningPanel().getRiseComboForTest().setSelectedId (5,
        juce::sendNotificationSync);

    // Slot 0 (still Global) followed the strip...
    auto* globalSlot = app.getNotchControllerForTest (0);
    ASSERT_NE (globalSlot, nullptr);
    EXPECT_DOUBLE_EQ (globalSlot->getRiseReferenceMs(), 1000.0);
    // ...and the Custom slot kept its own value.
    EXPECT_DOUBLE_EQ (custom->getRiseReferenceMs(), 750.0);

    // Switching slot 3 back to G re-attaches it: the next global change lands
    // on its controller again...
    panel.getRowForTest (3).tune.setSelectedId (1 /* G */, juce::sendNotificationSync);
    app.getTuningPanel().getPersistComboForTest().setSelectedId (5,
        juce::sendNotificationSync);
    EXPECT_EQ (custom->getPersistenceBlocks(), 5);

    // ...and choosing C again seeds the editor FROM the controller's current
    // state (persist 5), not from the struct defaults.
    panel.getRowForTest (3).tune.setSelectedId (2 /* C */, juce::sendNotificationSync);
    EXPECT_FALSE (panel.slotTuningProvider (3).usesGlobal);
    EXPECT_EQ (custom->getPersistenceBlocks(), 5);
    EXPECT_EQ (panel.getDetailForTest (3).persist.getSelectedId(), 5);
}

//==============================================================================
// PER-SLOT MONITORING (2026-08-25).
//
// The app has always run one NotchController per routing slot, but the GUI
// could only ever display slot 0's: SpectrumView and NotchListPanel were
// handed notchControllers_[0] at construction with no way to be re-pointed.
// A rig with a vocal mic on slot 1 and a lectern on slot 2 could see one of
// them. The masthead selector is the route that changes it.

TEST (MainComponent, TheDisplayStartsOnSlotZero)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    MainComponent app;

    EXPECT_EQ (app.getDisplayedSlot(), 0);
    EXPECT_EQ (app.getSlotTabsForTest().getSelected(), 0);
}

TEST (MainComponent, SelectingASlotMovesTheSelectorAndIsReportedBack)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    MainComponent app;
    app.setSize (1280, 880);
    app.resized();

    app.setDisplayedSlot (1);

    EXPECT_EQ (app.getDisplayedSlot(), 1);
    EXPECT_EQ (app.getSlotTabsForTest().getSelected(), 1);
}

TEST (MainComponent, ASlotWithNoRoutingRowCannotBeMonitored)
{
    // Slots past the routing table's visible row count have no controls the
    // user could act on, so pointing the analyser at one would show a spectrum
    // they cannot do anything about. The request is IGNORED rather than
    // clamped -- clamping would quietly show a different slot's notches.
    const juce::ScopedJuceInitialiser_GUI juceInit;

    MainComponent app;
    app.setSize (1280, 880);
    app.resized();

    ASSERT_EQ (app.getSlotPanelForTest().getVisibleRowCount(), 2);

    app.setDisplayedSlot (5);
    EXPECT_EQ (app.getDisplayedSlot(), 0);

    app.setDisplayedSlot (-1);
    EXPECT_EQ (app.getDisplayedSlot(), 0);
}

TEST (MainComponent, RevealingARoutingRowMakesThatSlotSelectable)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    MainComponent app;
    app.setSize (1280, 880);
    app.resized();

    EXPECT_EQ (app.getSlotTabsForTest().getSlotCount(), 2);

    app.getSlotPanelForTest().setVisibleRowCount (4);

    EXPECT_EQ (app.getSlotTabsForTest().getSlotCount(), 4);

    app.setDisplayedSlot (3);
    EXPECT_EQ (app.getDisplayedSlot(), 3);
}

TEST (MainComponent, HidingTheMonitoredRowFallsBackToSlotZero)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    MainComponent app;
    app.setSize (1280, 880);
    app.resized();

    app.getSlotPanelForTest().setVisibleRowCount (4);
    app.setDisplayedSlot (3);
    ASSERT_EQ (app.getDisplayedSlot(), 3);

    // The table shrinks back under the monitored slot: the display must not be
    // left pointed at a slot that no longer has a row.
    app.getSlotPanelForTest().setVisibleRowCount (2);

    EXPECT_EQ (app.getDisplayedSlot(), 0);
    EXPECT_EQ (app.getSlotTabsForTest().getSlotCount(), 2);
}

//==============================================================================
// RING RISK -- the readout exists, its data source does not yet.

TEST (MainComponent, RingRiskReadsUnavailableUntilSomethingProvidesIt)
{
    // The honest default, and the one thing about this readout that MUST NOT
    // regress: an unwired risk indicator that reads "low" is worse than one
    // that reads "n/a", because a soundman would act on it. See
    // docs/spec-ring-risk.md.
    const juce::ScopedJuceInitialiser_GUI juceInit;

    MainComponent app;

    EXPECT_EQ (app.getSpectrumViewForTest().getRingRisk(),
               gui::SpectrumView::RingRisk::Unavailable);
}

//==============================================================================
// Task P3 -- the PRESET row (LOAD... / SAVE...) under the INTERFACE drawer.
//
// The two file choosers are INJECTABLE (mirroring ModeRail::confirmHook): a
// test swaps in a fake that hands the inner callback a known temp file, so no
// native dialog is ever opened under a headless suite. The buttons are driven
// through onClick() directly -- triggerClick() posts a command message no
// headless loop dispatches (test_moderail.cpp:76). A notch is proven present
// exactly as test_notchcontroller.cpp does it: feed a synthetic block into the
// slot's tap, runOnce() to publish, then copySnapshot() and read it back.

TEST (GuiWiring, LoadPresetButtonRoutesThroughInjectableChooserToLoadPreset)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    MainComponent app;

    // A valid one-notch preset routed to slot 0, at a real sample rate.
    const juce::String json =
        R"({"version":"1.0","device":"","sampleRate":48000,"bufferSize":256,)"
        R"("notches":[{"index":0,"freq":482.0,"Q":30.0,"depth":-12.0}]})";
    auto presetFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("az-handsfree-p3-load.json");
    ASSERT_TRUE (presetFile.replaceWithText (json));

    // Fake chooser: hands loadPreset the temp file with no dialog. A real pick.
    app.presetLoadChooser = [presetFile] (std::function<void (const juce::File&)> onPicked)
    {
        onPicked (presetFile);
    };

    // The button's own handler -- the whole routing lives in there.
    app.getDeviceDrawer().loadButton.onClick();
    presetFile.deleteFile();

    // The notch reached slot 0's controller: publish a frame and read it back.
    // loadPreset adopts even with no device running.
    auto* controller0 = app.getNotchControllerForTest (0);
    ASSERT_NE (controller0, nullptr);
    std::vector<float> hop (512, 0.25f);
    app.getAudioEngine().getTapBuffer (0).write (hop.data(), hop.size());
    controller0->runOnce();

    NotchController::SnapshotBuffer snap {};
    controller0->copySnapshot (snap);
    ASSERT_GT (snap.notchCount, 0u);

    bool found = false;
    for (std::uint32_t n = 0; n < snap.notchCount; ++n)
        if (std::abs (snap.notches[n].frequency - 482.0f) < 1.0f)
            found = true;
    EXPECT_TRUE (found);
}

TEST (GuiWiring, SavePresetWritesAFileThatLoadPresetReopensIdentically)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    MainComponent app;

    auto* controller0 = app.getNotchControllerForTest (0);
    ASSERT_NE (controller0, nullptr);

    // A known notch adopted into slot 0, published at a known rate. The rate
    // matters: saveToFile validates freq < Nyquist of Preset.sampleRate, and
    // savePreset records the rate the notch was published at.
    controller0->setSampleRate (48000.0);
    PresetNotch n;
    n.index = 2; n.freq = 1234.0; n.Q = 28.0; n.depthDB = -9.0; n.slot = 0;
    ASSERT_EQ (controller0->adoptPreset ({ n }), 1);

    std::vector<float> hop (512, 0.25f);
    app.getAudioEngine().getTapBuffer (0).write (hop.data(), hop.size());
    controller0->runOnce();

    auto outFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("az-handsfree-p3-roundtrip.json");
    outFile.deleteFile();

    ASSERT_TRUE (app.savePreset (outFile));

    const auto result = PresetManager::loadFromFile (outFile);
    outFile.deleteFile();

    ASSERT_TRUE (result.ok);
    // The PresetNotch above names no lane, so adoptPreset mirrors it onto both
    // lanes of the stereo slot -- and savePreset writes ONE entry per (slot,
    // lane, index) rather than collapsing them, so both come back, each
    // carrying its own lane. (Merged 2026-09-05: the dedup this test used to
    // assert is exactly what drops an INDEP lane-1 notch.)
    ASSERT_EQ (result.preset.notches.size(), 2u);

    bool lanesSeen[2] = { false, false };

    for (const auto& saved : result.preset.notches)
    {
        EXPECT_EQ (saved.index, 2);
        EXPECT_EQ (saved.slot, 0);
        EXPECT_NEAR (saved.freq, 1234.0, 0.5);
        EXPECT_NEAR (saved.Q, 28.0, 0.5);
        EXPECT_NEAR (saved.depthDB, -9.0, 0.5);
        ASSERT_GE (saved.lane, 0);
        ASSERT_LT (saved.lane, 2);
        lanesSeen[saved.lane] = true;
    }

    EXPECT_TRUE (lanesSeen[0]);
    EXPECT_TRUE (lanesSeen[1]);
    EXPECT_DOUBLE_EQ (result.preset.sampleRate, 48000.0);
}

//==============================================================================
// Lane S x lane P -- what the two lanes' merge has to keep true.
//
// RED IF savePreset dedups notches across lanes (lane 1's 2 kHz notch is gone,
// or lane 0's 1 kHz notch comes back on both lanes), or if it stops writing the
// "slots" section (the reopened app comes back with slot 1 INDEP).
//
// The setup is the one main's savePreset could not express: one routing slot in
// INDEP mode whose two lanes hold DIFFERENT notches at the SAME chain index.
TEST (GuiWiring, SavePresetRoundTripsPerLaneNotchesAndTheLinkedFlag)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    auto outFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("az-handsfree-lane-roundtrip.json");
    outFile.deleteFile();

    // Deletes on EVERY path out of this test, a failed ASSERT_* included.
    struct FileGuard
    {
        juce::File f;
        ~FileGuard() { f.deleteFile(); }
    } const guard { outFile };

    {
        MainComponent app;

        SlotConfig stereo;
        stereo.enabled = true;
        stereo.width   = 2;
        app.getAudioEngine().setSlotConfig (0, stereo);
        app.getAudioEngine().setSlotConfig (1, stereo);

        auto* controller0 = app.getNotchControllerForTest (0);
        ASSERT_NE (controller0, nullptr);
        controller0->setWidth (2);
        controller0->setSampleRate (48000.0);

        app.setSlotLinked (0, false);   // INDEP -- each lane on its own
        app.setSlotLinked (1, true);    // and a LINKED slot to carry the flag

        PresetNotch left;
        left.index = 2; left.freq = 1000.0; left.Q = 30.0; left.depthDB = -12.0;
        left.slot = 0;  left.lane = 0;

        PresetNotch right;
        right.index = 2; right.freq = 2000.0; right.Q = 30.0; right.depthDB = -12.0;
        right.slot = 0;  right.lane = 1;

        ASSERT_EQ (controller0->adoptPreset ({ left, right }), 2);

        // The snapshot only refreshes when runOnce() has a block to chew on.
        std::vector<float> hop (512, 0.25f);
        app.getAudioEngine().getTapBuffer (0).write (hop.data(), hop.size());
        controller0->runOnce();

        ASSERT_TRUE (app.savePreset (outFile));
    }

    // A FRESH app: nothing survives but what the file carries.
    MainComponent reopened;
    ASSERT_TRUE (reopened.loadPreset (outFile));

    auto* reopenedController0 = reopened.getNotchControllerForTest (0);
    ASSERT_NE (reopenedController0, nullptr);

    std::vector<float> hop (512, 0.25f);
    reopened.getAudioEngine().getTapBuffer (0).write (hop.data(), hop.size());
    reopenedController0->runOnce();

    NotchController::SnapshotBuffer snap {};
    reopenedController0->copySnapshot (snap);

    bool leftFound = false;
    bool rightFound = false;

    for (std::uint32_t n = 0; n < snap.notchCount; ++n)
    {
        const auto& sn = snap.notches[n];

        if ((int) sn.index != 2)
            continue;

        if (sn.channel == 0 && std::abs (sn.frequency - 1000.0f) < 1.0f)
            leftFound = true;

        if (sn.channel == 1 && std::abs (sn.frequency - 2000.0f) < 1.0f)
            rightFound = true;
    }

    EXPECT_TRUE (leftFound)
        << "lane 0's 1 kHz notch at index 2 did not survive the round trip";
    EXPECT_TRUE (rightFound)
        << "lane 1's 2 kHz notch at index 2 did not survive the round trip";
    EXPECT_TRUE (reopened.isSlotLinked (1))
        << "the saved \"slots\" section did not carry `linked`";
}

//==============================================================================
// Lane D (data loop): the session log through the real wiring.

namespace
{
juce::File freshLogDir (const juce::String& name)
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("az-handsfree-sessionlog").getChildFile (name);
    dir.deleteRecursively();
    return dir;
}

std::vector<juce::var> parsedLines (const juce::File& file)
{
    juce::StringArray lines;
    lines.addLines (file.loadFileAsString());
    lines.removeEmptyStrings();
    std::vector<juce::var> out;
    for (const auto& l : lines)
    {
        juce::var v;
        EXPECT_TRUE (juce::JSON::parse (l, v).wasOk()) << l;
        out.push_back (v);
    }
    return out;
}

const juce::var* firstEvent (const std::vector<juce::var>& events, const juce::String& name)
{
    for (const auto& e : events)
        if (e["ev"].toString() == name)
            return &e;
    return nullptr;
}
} // namespace

// Spec test 14. Red if FALSE stops clearing with VerdictFalse, or the logger
// stops receiving verdict / notch_clear / session_start in order.
TEST (GuiWiring, FalseVerdictLogsVerdictThenClearsWithVerdictFalse)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent app;
    app.setAppVersion ("9.9.9-test");
    const auto dir = freshLogDir ("verdict");
    ASSERT_TRUE (app.startSessionLog (dir));
    const auto file = app.sessionLogFileForTest();

    auto* c0 = app.getNotchControllerForTest (0);
    ASSERT_NE (c0, nullptr);
    ASSERT_TRUE (c0->setNotch (0, 0, 1234.0, 30.0, -12.0, NotchController::Origin::Manual));
    std::vector<float> hop (512, 0.1f);
    app.getAudioEngine().getTapBuffer (0, 0).write (hop.data(), hop.size());
    app.getAudioEngine().getTapBuffer (0, 1).write (hop.data(), hop.size());
    c0->runOnce();   // publishes the snapshot AND flushes the Set event

    auto& panel = app.getNotchListPanelForTest();
    panel.refreshFromSnapshot();
    ASSERT_EQ (panel.rowCountForTest(), 1);
    panel.falseButtonForTest (0)->onClick();

    app.getAudioEngine().getTapBuffer (0, 0).write (hop.data(), hop.size());
    app.getAudioEngine().getTapBuffer (0, 1).write (hop.data(), hop.size());
    c0->runOnce();   // flushes the Clear event; republishes without the notch
    NotchController::SnapshotBuffer snap {};
    c0->copySnapshot (snap);
    EXPECT_EQ (snap.notchCount, 0u);

    app.stopSessionLog();
    const auto events = parsedLines (file);
    ASSERT_GE (events.size(), 5u);
    EXPECT_EQ (events.front()["ev"].toString(), "session_start");
    EXPECT_EQ (events.front()["app_version"].toString(), "9.9.9-test");
    EXPECT_TRUE (events.front()["slots"].isArray());
    EXPECT_EQ (events.back()["ev"].toString(), "session_end");

    const auto* set = firstEvent (events, "notch_set");
    ASSERT_NE (set, nullptr);
    EXPECT_EQ ((*set)["origin"].toString(), "manual");
    EXPECT_NEAR ((double) (*set)["hz"], 1234.0, 0.5);
    EXPECT_FALSE (set->hasProperty ("ctx"));

    const auto* verdict = firstEvent (events, "verdict");
    ASSERT_NE (verdict, nullptr);
    EXPECT_EQ ((*verdict)["verdict"].toString(), "false");
    EXPECT_EQ ((int) (*verdict)["slot"], 0);
    EXPECT_EQ ((int) (*verdict)["lane"], 0);
    EXPECT_EQ ((int) (*verdict)["index"], 0);

    const auto* clear = firstEvent (events, "notch_clear");
    ASSERT_NE (clear, nullptr);
    EXPECT_EQ ((*clear)["reason"].toString(), "verdict_false");

    // Order: verdict is written before the clear it causes.
    EXPECT_LT (verdict - events.data(), clear - events.data());
}

// Mode and tuning land in the log. Red if requestMode / onTuningChanged stop
// logging, or the field names drift from the logstats contract.
TEST (GuiWiring, ModeAndTuningChangesAreLogged)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent app;
    const auto dir = freshLogDir ("mode");
    ASSERT_TRUE (app.startSessionLog (dir));
    const auto file = app.sessionLogFileForTest();

    app.requestMode (AudioEngine::Mode::Auto);
    gui::TuningPanel::Params p;
    p.riseReferenceMs = 300; p.persistenceBlocks = 4; p.q = 25.0f; p.depthDb = -10.0f; p.peakinessThreshold = 12.0f;
    app.getTuningPanel().onTuningChanged (p);   // MainComponent.h:151

    app.stopSessionLog();
    const auto events = parsedLines (file);
    const auto* mode = firstEvent (events, "mode");
    ASSERT_NE (mode, nullptr);
    EXPECT_EQ ((*mode)["mode"].toString(), "auto");
    const auto* tuning = firstEvent (events, "tuning");
    ASSERT_NE (tuning, nullptr);
    EXPECT_EQ ((int) (*tuning)["slot"], -1);
    EXPECT_EQ ((int) (*tuning)["persist"], 4);
    EXPECT_NEAR ((double) (*tuning)["depth_db"], -10.0, 1e-6);
}

// Spec test 15. Red if MainComponent's teardown order lets a detector thread
// deliver an event into a logger that is already gone (or vice versa).
TEST (GuiWiring, DestroyingTheAppWhileADetectorIsPlacingNotchesDoesNotCrash)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    for (int run = 0; run < 20; ++run)
    {
        const auto dir = freshLogDir ("teardown-" + juce::String (run));
        MainComponent app;
        ASSERT_TRUE (app.startSessionLog (dir));
        auto* c0 = app.getNotchControllerForTest (0);
        ASSERT_NE (c0, nullptr);
        c0->setDetectionActive (true);
        c0->start();   // real detector thread

        // A loud 1 kHz tone into both lanes for ~1 s of audio, then die.
        std::vector<float> hop (512);
        double n = 0.0;
        for (int block = 0; block < 90; ++block)
        {
            for (auto& s : hop) { s = std::sin (2.0 * 3.14159265358979 * 1000.0 * n / 48000.0); n += 1.0; }
            app.getAudioEngine().getTapBuffer (0, 0).write (hop.data(), hop.size());
            app.getAudioEngine().getTapBuffer (0, 1).write (hop.data(), hop.size());
            if (block % 8 == 7) juce::Thread::sleep (1);
        }
        // `app` is destroyed here, mid-placement, with the thread running.
    }
    SUCCEED();
}

// Lane S loose end (A-9). Red if loadPreset stops counting a lane-1 notch a
// mono slot cannot take.
TEST (GuiWiring, LoadPresetCountsNotchesSkippedByAMonoSlot)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent app;
    const juce::String json =
        R"({"version":"1.0","device":"","sampleRate":48000,"bufferSize":256,)"
        R"("slots":[{"index":0,"enabled":true,"width":1,"inputChannels":[0,1],"outputChannels":[0,1]}],)"
        R"("notches":[{"slot":0,"lane":1,"index":0,"freq":482.0,"Q":30.0,"depth":-12.0},)"
        R"({"slot":0,"lane":0,"index":1,"freq":982.0,"Q":30.0,"depth":-12.0}]})";
    auto presetFile = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("az-handsfree-d-skipped.json");
    ASSERT_TRUE (presetFile.replaceWithText (json));
    ASSERT_TRUE (app.loadPreset (presetFile));
    presetFile.deleteFile();
    EXPECT_EQ (app.lastLoadSkippedNotchesForTest(), 1);
}
