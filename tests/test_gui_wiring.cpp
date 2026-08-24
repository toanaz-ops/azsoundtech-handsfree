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
#include "gui/DeviceDrawer.h"
#include "gui/DevicePanel.h"
#include "gui/ModeBar.h"

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
// Task 3 -- L1/L2 layouts, DeviceDrawer, persistence (spec 2026-08-23
// sections 0 G-2, 2 and 3; test table row "Layout switch").
//
// Isolation: every layout test redirects persistence to a scratch directory
// under %TEMP% via an ABSOLUTE folderName (File::getChildFile returns an
// absolute path as-is, so PropertiesFile::Options::getDefaultFile resolves
// there instead of %APPDATA%). A real user's saved choice is never read or
// clobbered, and no restore step can be forgotten.

namespace
{
class TempLayoutStore
{
public:
    TempLayoutStore()
        : directory (juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("HandsFreeLayoutTests_"
                                        + juce::String (juce::Random::getSystemRandom().nextInt())))
    {
        directory.createDirectory();
    }

    ~TempLayoutStore() { directory.deleteRecursively(); }

    juce::PropertiesFile::Options options() const
    {
        juce::PropertiesFile::Options o;
        o.applicationName = "AZ Soundtech Hands-free";   // same file NAME as production
        o.filenameSuffix  = "xml";
        o.folderName      = directory.getFullPathName(); // absolute: wins over app-data dir
        return o;
    }

private:
    juce::File directory;
};
} // namespace

TEST (MainComponent, DefaultLayoutIsPerformance)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    TempLayoutStore store;
    MainComponent app (&store.options());

    // G-2: L2 Performance is the shipped default -- even when a real user on
    // this machine has Classic saved.
    EXPECT_EQ (app.getLayout(), gui::ScreenLayout::Performance);
}

TEST (MainComponent, LayoutSwitchPersistsAcrossReconstruction)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    TempLayoutStore store;

    {
        MainComponent app (&store.options());
        app.setLayout (gui::ScreenLayout::Classic);

        // The in-memory value AND the persisted property must agree right now.
        EXPECT_EQ (app.getLayout(), gui::ScreenLayout::Classic);
    }

    {
        // "Restart": a fresh component reading the SAME storage must come back
        // in the saved layout.
        MainComponent reopened (&store.options());
        EXPECT_EQ (reopened.getLayout(), gui::ScreenLayout::Classic);
    }
    // Scratch directory removed with the store: nothing to restore.
}

TEST (MainComponent, DrawerSettingsToggleSwitchesTheLayout)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    TempLayoutStore store;
    MainComponent app (&store.options());

    ASSERT_EQ (app.getLayout(), gui::ScreenLayout::Performance);
    EXPECT_TRUE (app.getDeviceDrawer().performanceButton_.getToggleState());

    // Clicking the L1 cell in the drawer's settings row drives the same path
    // the operator uses. sendNotificationSync dispatches inline: no message
    // pump needed with modal loops off.
    app.getDeviceDrawer().classicButton_.setToggleState (true, juce::sendNotificationSync);

    EXPECT_EQ (app.getLayout(), gui::ScreenLayout::Classic);
    EXPECT_TRUE (app.getDeviceDrawer().classicButton_.getToggleState());
}

TEST (MainComponent, GearToggleRelayoutsTheParentSoContentAppears)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    TempLayoutStore store;
    MainComponent app (&store.options());   // L2 default, drawer starts closed

    auto& drawer = app.getDeviceDrawer();
    ASSERT_FALSE (drawer.isOpen());
    ASSERT_TRUE (drawer.wrappedBoundsForTest().isEmpty());

    drawer.setOpen (true);

    // The parent must have re-run its resized(): the wrapped DevicePanel gets
    // a real rect NOW, not after some unrelated window resize.
    EXPECT_TRUE (drawer.isOpen());
    const auto openBounds = drawer.wrappedBoundsForTest();
    EXPECT_FALSE (openBounds.isEmpty());
    EXPECT_EQ (openBounds.getHeight(), gui::DeviceDrawer::kContentHeight);
    EXPECT_EQ (drawer.getBounds().getHeight(),
               gui::DeviceDrawer::kHeaderHeight + gui::DeviceDrawer::kContentHeight);

    drawer.setOpen (false);

    EXPECT_FALSE (drawer.isOpen());
    EXPECT_TRUE (drawer.wrappedBoundsForTest().isEmpty());
    EXPECT_EQ (drawer.getBounds().getHeight(), gui::DeviceDrawer::kHeaderHeight);
}

TEST (MainComponent, MinimumSizeKeepsRailAndSpectrumDisjoint)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    TempLayoutStore store;
    MainComponent app (&store.options());

    for (const auto layout : { gui::ScreenLayout::Classic, gui::ScreenLayout::Performance })
    {
        app.setLayout (layout);
        app.setSize (MainComponent::kMinimumWidth, MainComponent::kMinimumHeight);

        const auto rail     = app.railBoundsForTest();
        const auto spectrum = app.spectrumBoundsForTest();

        // Both must actually be laid out before the non-overlap claim means
        // anything.
        EXPECT_FALSE (rail.isEmpty());
        EXPECT_FALSE (spectrum.isEmpty());

        // The binding assertion: zero intersecting pixels between the fixed
        // rail and the spectrum at the smallest window we allow.
        const auto overlap = rail.getIntersection (spectrum);
        EXPECT_EQ (overlap.getWidth() * overlap.getHeight(), 0)
            << "rail and spectrum overlap at minimum window size, layout "
            << (int) layout;
    }
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
    // Destruction runs notchController_'s destructor (joins its own thread)
    // BEFORE engine_, reverse declaration order. A hang here means stop()
    // failed to join; a leak-detector hit means something was left running.
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
