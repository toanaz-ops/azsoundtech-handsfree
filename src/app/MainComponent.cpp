#include "app/MainComponent.h"

#include "gui/DeviceViewModel.h"

namespace
{
// 5 Hz. The status line carries a latency figure and two channel counts; there
// is nothing here a soundman reads faster than that, and the poll costs three
// relaxed atomic loads plus a device query.
constexpr int kStatusRefreshMs = 200;

// Smallest spectrum worth looking at; resized() clamps the drawer against it.
constexpr int kMinSpectrumHeight = 120;

// Height of the notch list strip (Task 4): header row plus ~3 visible notch
// rows. The same figure the R-1 slot reservation assumed.
constexpr int kNotchListHeight = 120;

// ApplicationProperties key for the persisted layout (spec G-2 / section 3).
constexpr const char* kLayoutPropertyKey = "layout";
} // namespace

MainComponent::MainComponent (
    const juce::PropertiesFile::Options* propertyOptionsOverride)
    : notchController_ (engine_.getTapBuffer(), engine_.getCommandQueue(), systemClock_)
    , spectrumView_ (notchController_)
    , modeRail_ (gui::ModeRail::Orientation::Vertical)
    , notchListPanel_ (notchController_)
    , deviceDrawer_ (devicePanel_)
{
    // The Console-industrial theme, applied once here and inherited by every
    // child through the Component::getLookAndFeel() chain.
    setLookAndFeel (&azLookAndFeel_);

    addAndMakeVisible (spectrumView_);
    addAndMakeVisible (modeRail_);
    addAndMakeVisible (deviceDrawer_);
    // The notch list is added but its VISIBILITY is layout business:
    // resized() shows it only per the L1/L2 rules.
    addAndMakeVisible (notchListPanel_);
    // statusBar_ / modeBar_ stay alive but hidden: see MainComponent.h.

    modeBar_.onModeRequested = [this] (AudioEngine::Mode mode) { requestMode (mode); };

    // New console wiring. The rail requests modes through the same
    // requestMode() path the old bar used -- one route to the engine.
    modeRail_.onSoundcheck = [this] { requestMode (AudioEngine::Mode::Soundcheck); };
    modeRail_.onAuto       = [this] { requestMode (AudioEngine::Mode::Auto); };
    modeRail_.onBypass     = [this] { requestMode (AudioEngine::Mode::Bypass); };

    // R-3 already guards this behind ModeRail's confirmation hook.
    modeRail_.onClearAllConfirmed = [this] { notchController_.clearAll(); };
    modeRail_.getSoundcheckRemainingMs = [this]
    {
        return notchController_.getSoundcheckRemainingMs();
    };

    // R-5: the rail's LIST cell slides the strip; the open/closed meaning is
    // entirely this side's (L2 toggle vs L1 fixed strip).
    modeRail_.onToggleNotchList = [this] (bool open) { setNotchListOpen (open); };

    deviceDrawer_.setStatusBadge (&statusBadge_);
    deviceDrawer_.onLayoutSelected = [this] (gui::ScreenLayout layout) { setLayout (layout); };

    // Bridge design §6.5: a device change in the panel is always an engine
    // RESTART, and the rings are cleared on the way -- so the detector thread
    // must be joined first and relaunched after. These hooks live on the ONE
    // DevicePanel instance; re-parenting it into the drawer changed nothing.
    devicePanel_.onBeforeRestart = [this] { notchController_.stop (1000); };
    devicePanel_.onAfterRestart  = [this] { notchController_.start(); };

    // A setting the hardware refused. Held rather than flashed: the user needs
    // to still be reading it a few seconds later.
    devicePanel_.onMessage = [this] (const juce::String& message)
    {
        panelMessage_ = message;
        refreshStatus();
    };

    // Layout persistence (spec section 3). Stored under %APPDATA%\AZ Soundtech,
    // read back on every launch; G-2 fixes the default at Performance.
    juce::PropertiesFile::Options propertyOptions;
    propertyOptions.applicationName     = "AZ Soundtech Hands-free";
    propertyOptions.filenameSuffix      = "xml";
    propertyOptions.folderName          = "AZ Soundtech";

    if (propertyOptionsOverride != nullptr)
        propertyOptions = *propertyOptionsOverride;
    appProperties_.setStorageParameters (propertyOptions);

    {
        const int saved = appProperties_.getUserSettings()
                              ->getIntValue (kLayoutPropertyKey, (int) gui::ScreenLayout::Performance);
        layout_ = (saved == (int) gui::ScreenLayout::Classic)
                      ? gui::ScreenLayout::Classic
                      : gui::ScreenLayout::Performance;
    }

    applyLayoutState (layout_);

    // Enumeration is safe with no device open -- every AudioEngine query used
    // here guards getCurrentAudioDevice() being null. The rate and buffer
    // combos will come up empty and are refilled by startAudio().
    devicePanel_.refresh();
    modeBar_.setDisplayedMode (engine_.getMode());
    refreshStatus();

    startTimer (kStatusRefreshMs);

    setSize (800, 600);
}

MainComponent::~MainComponent()
{
    stopTimer();
    // Detach the look and feel while every child is still alive -- a
    // Component must not outlive the LookAndFeel it points at.
    setLookAndFeel (nullptr);
    // §6.5: the detector thread must be dead before the engine tears down.
    notchController_.stop (1000);
    engine_.stop();
}

AudioEngine& MainComponent::getAudioEngine()
{
    return engine_;
}

void MainComponent::startAudio()
{
    // Task 16: prefer ASIO, but do NOT require it. The plan said "filter to
    // show ASIO devices only"; the ASIO SDK is excluded from this repo for
    // licensing reasons, so on CI and on this dev machine the ASIO type is
    // never registered and an ASIO-only list is empty. Asking the engine for a
    // type that IS registered also matters: AudioEngine's own default is the
    // literal string "ASIO", and JUCE silently keeps whatever type is current
    // when the requested one does not exist.
    engine_.setAudioDeviceType (gui::chooseDefaultDeviceType (engine_.getAvailableDeviceTypeNames()));
    engine_.start();

    // The device is open: the detector can start pumping. start() is a no-op
    // if the thread already runs.
    notchController_.start();

    // Only now do getAvailableSampleRates() and getAvailableBufferSizes()
    // return anything.
    devicePanel_.refresh();
    refreshStatus();
}

void MainComponent::requestMode (AudioEngine::Mode mode)
{
    // Soundcheck sets the mode and nothing else. The specified countdown is NOT
    // implemented, deliberately: owner decision D-06 freezes the detector's
    // timers while the tap is dead, so a GUI-side wall-clock juce::Timer would
    // disagree with a frozen detector and show a countdown that does not match
    // what the app is doing. The remaining time has to come from the detector's
    // getSoundcheckSecondsRemaining() (bridge design section 4), which does not
    // exist yet.
    engine_.setMode (mode);
    modeBar_.setDisplayedMode (engine_.getMode());
}

void MainComponent::setLayout (gui::ScreenLayout layout)
{
    if (layout == layout_)
        return;

    layout_ = layout;

    // Persist immediately: a crash or power cut mid-show must not lose the
    // operator's arrangement choice.
    auto* storedProperties = appProperties_.getUserSettings();
    storedProperties->setValue (kLayoutPropertyKey, (int) layout);
    storedProperties->saveIfNeeded();

    applyLayoutState (layout);
    resized();
}

void MainComponent::setNotchListOpen (const bool open)
{
    if (open == notchListOpen_)
        return;

    notchListOpen_ = open;

    // Keep the rail's LIST cell in step with programmatic changes. A click
    // re-enters here already carrying the same state, and
    // dontSendNotification cannot loop the callback.
    modeRail_.listToggleButton.setToggleState (open, juce::dontSendNotification);

    resized();
}

void MainComponent::applyLayoutState (gui::ScreenLayout layout)
{
    // Same components, different arrangement -- nothing is destroyed or
    // recreated here (spec section 3).
    modeRail_.setOrientation (layout == gui::ScreenLayout::Performance
                                  ? gui::ModeRail::Orientation::Vertical
                                  : gui::ModeRail::Orientation::Horizontal);

    deviceDrawer_.applyLayoutMode (layout);
    deviceDrawer_.setSelectedLayout (layout);

    // R-5: the LIST toggle belongs to L2 only -- L1's strip is fixed, so the
    // cell would command nothing.
    modeRail_.setListToggleVisible (layout == gui::ScreenLayout::Performance);
}

void MainComponent::refreshStatus()
{
    gui::DeviceStatus status;
    status.running           = engine_.isRunning();
    status.sampleRateHz      = engine_.getCurrentSampleRateHz();
    status.numInputChannels  = engine_.getNumInputChannels();
    status.numOutputChannels = engine_.getNumOutputChannels();
    status.latencySeconds    = engine_.getCurrentLatency();

    // A device error outranks a refused setting: the refusal describes a device
    // that is no longer running.
    juce::String message = gui::formatDeviceBanner (engine_.getLastDeviceError());

    if (message.isEmpty())
        message = panelMessage_;

    statusBar_.setStatus (status, message);
}

void MainComponent::timerCallback()
{
    refreshStatus();

    // The badge answers "is it protecting?" in one glance (spec sections 2
    // and 5): IDLE with no device running, BYPASSED when the mode says so,
    // PROTECTING only while audio actually flows through active detection.
    if (! engine_.isRunning())
        statusBadge_.setState (gui::ProtectionState::Idle);
    else if (engine_.getMode() == AudioEngine::Mode::Bypass)
        statusBadge_.setState (gui::ProtectionState::Bypassed);
    else
        statusBadge_.setState (gui::ProtectionState::Protecting);

    // The engine's mode can change from somewhere other than these buttons --
    // it already can via setMode(), and the detector will do it when Soundcheck
    // becomes real. setDisplayedMode() reflects without requesting, so this
    // cannot fight the user.
    modeBar_.setDisplayedMode (engine_.getMode());
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));

    g.setColour (az::theme::text);
    g.setFont (20.0f);
    g.drawText ("AZ Soundtech Hands-free",
                getLocalBounds().removeFromTop (36),
                juce::Justification::centred,
                true);
}

void MainComponent::resized()
{
    using namespace az::theme;

    auto area = getLocalBounds();
    area.removeFromTop (36);  // title, drawn in paint()

    // Reserve room for the rail plus a minimum usable spectrum, then clamp the
    // drawer so a too-small window shrinks the drawer instead of overlapping
    // siblings (spec section 3). setResizeLimits should make the clamp a
    // no-op in practice.
    const int reserveBelowDrawer = kMinSpectrumHeight + gap
                                 + (layout_ == gui::ScreenLayout::Performance
                                        ? 320   // vertical rail floor: modes + countdown + CLEAR ALL/LIST
                                        : buttonCellHeight);
    const int maxDrawerHeight = juce::jmax (0, area.getHeight() - reserveBelowDrawer);
    const int drawerHeight    = juce::jlimit (0, maxDrawerHeight,
                                              deviceDrawer_.getPreferredHeight());

    // Same footprint the drawer's FlexItem used to occupy: its height plus
    // the trailing gap margin.
    deviceDrawer_.setBounds (area.removeFromTop (drawerHeight));
    area.removeFromTop (gap);

    // Task 4: the notch list strip. L1 Classic pins it to the bottom; L2
    // Performance shows it only while toggled open. The panel itself stays
    // layout-agnostic -- it just receives visibility and bounds.
    const bool listShown = (layout_ == gui::ScreenLayout::Classic) || notchListOpen_;
    notchListPanel_.setVisible (listShown);

    if (layout_ == gui::ScreenLayout::Performance)
    {
        // L2: big spectrum with the fixed-width rail down the right edge,
        // the notch strip (when open) pinned underneath.
        //
        // The row is laid out DIRECTLY on its own rectangle rather than
        // nested inside a column FlexBox. History: while wiring the notch
        // strip (2026-08-24) L2 bounds came up stale under instrumentation,
        // so this was restructured to single-level FlexBox passes; the root
        // cause of the original mis-layout was never independently
        // reproduced (the earlier L2 coverage passed vacuously off stale
        // Classic bounds, since setSize() delivers no resized() callback to
        // a peer-less component). PerformanceLayoutHidesTheStripUntilToggled
        // guards real geometry in both layouts.
        if (listShown)
            notchListPanel_.setBounds (area.removeFromBottom (kNotchListHeight));

        juce::FlexBox row;
        row.flexDirection = juce::FlexBox::Direction::row;
        row.items.add (juce::FlexItem (spectrumView_).withFlex (1.0f));
        row.items.add (juce::FlexItem (modeRail_).withWidth ((float) railWidth));
        row.performLayout (area.toFloat());
    }
    else
    {
        // L1: horizontal rail strip under the device bar, spectrum below,
        // fixed notch strip at the bottom.
        juce::FlexBox main;
        main.flexDirection = juce::FlexBox::Direction::column;
        main.items.add (juce::FlexItem (modeRail_)
                            .withHeight ((float) buttonCellHeight)
                            .withMargin ({ 0.0f, 0.0f, (float) gap, 0.0f }));
        main.items.add (juce::FlexItem (spectrumView_).withFlex (1.0f));
        if (listShown)
            main.items.add (juce::FlexItem (notchListPanel_)
                                .withHeight ((float) kNotchListHeight));
        main.performLayout (area);
    }
}
