#include "app/MainComponent.h"

#include "app/PresetManager.h"
#include "gui/DeviceViewModel.h"

#include <cstddef>
#include <vector>

namespace
{
// 5 Hz. The status line carries a latency figure and two channel counts; there
// is nothing here a soundman reads faster than that, and the poll costs three
// relaxed atomic loads plus a device query.
constexpr int kStatusRefreshMs = 200;

// Smallest spectrum worth looking at; resized() clamps the drawer against it.
constexpr int kMinSpectrumHeight = 120;

// ApplicationProperties key for the persisted layout (spec G-2 / section 3).
constexpr const char* kLayoutPropertyKey = "layout";
} // namespace

MainComponent::MainComponent()
    : notchControllers_ ([this]
      {
          // Heap allocation per slot -- see the member comment in the header
          // for why these cannot live inside this object's stack frame.
          decltype (notchControllers_) controllers;
          for (int i = 0; i < kMaxSlots; ++i)
              controllers[(std::size_t) i] =
                  std::make_unique<NotchController> (engine_.getTapBuffer (i),
                                                     engine_.getCommandQueue (i),
                                                     systemClock_, i);
          return controllers;
      }())
    , spectrumView_ (*notchControllers_[0])
    , modeRail_ (gui::ModeRail::Orientation::Vertical)
    , deviceDrawer_ (devicePanel_)
{
    // The Console-industrial theme, applied once here and inherited by every
    // child through the Component::getLookAndFeel() chain.
    setLookAndFeel (&azLookAndFeel_);

    addAndMakeVisible (spectrumView_);
    addAndMakeVisible (modeRail_);
    addAndMakeVisible (deviceDrawer_);
    // statusBar_ / modeBar_ stay alive but hidden: see MainComponent.h.

    modeBar_.onModeRequested = [this] (AudioEngine::Mode mode) { requestMode (mode); };

    // New console wiring. The rail requests modes through the same
    // requestMode() path the old bar used -- one route to the engine.
    modeRail_.onSoundcheck = [this] { requestMode (AudioEngine::Mode::Soundcheck); };
    modeRail_.onAuto       = [this] { requestMode (AudioEngine::Mode::Auto); };
    modeRail_.onBypass     = [this] { requestMode (AudioEngine::Mode::Bypass); };

    // R-3 already guards this behind ModeRail's confirmation hook. Only the
    // slots the engine has enabled hold live notches.
    modeRail_.onClearAllConfirmed = [this]
    {
        for (int i = 0; i < kMaxSlots; ++i)
            if (engine_.getSlotConfig (i).enabled)
                notchControllers_[(std::size_t) i]->clearAll();
    };
    modeRail_.getSoundcheckRemainingMs = [this]
    {
        double remaining = 0.0;
        for (auto& controller : notchControllers_)
            remaining = juce::jmax (remaining, controller->getSoundcheckRemainingMs());
        return remaining;
    };

    deviceDrawer_.setStatusBadge (&statusBadge_);
    deviceDrawer_.onLayoutSelected = [this] (gui::ScreenLayout layout) { setLayout (layout); };

    // Bridge design §6.5: a device change in the panel is always an engine
    // RESTART, and the rings are cleared on the way -- so the detector thread
    // must be joined first and relaunched after. These hooks live on the ONE
    // DevicePanel instance; re-parenting it into the drawer changed nothing.
    devicePanel_.onBeforeRestart = [this]
    {
        // §6.5 for EVERY slot: all detector threads must be joined before a
        // device restart can clear the rings.
        for (auto& controller : notchControllers_)
            controller->stop (1000);
    };
    devicePanel_.onAfterRestart  = [this]
    {
        // setWidth() is only legal while the thread is stopped, so the width
        // from the (freshly restarted) engine config is applied here, then
        // the poll loop relaunches.
        for (int i = 0; i < kMaxSlots; ++i)
        {
            auto& controller = *notchControllers_[(std::size_t) i];
            controller.setWidth (engine_.getSlotConfig (i).width);
            controller.start();
        }
    };

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
    // §6.5: every detector thread must be dead before the engine tears down.
    for (auto& controller : notchControllers_)
        controller->stop (1000);
    engine_.stop();
}

AudioEngine& MainComponent::getAudioEngine()
{
    return engine_;
}

NotchController* MainComponent::getNotchControllerForTest (int slot)
{
    if (slot < 0 || slot >= kMaxSlots)
        return nullptr;

    return notchControllers_[(std::size_t) slot].get();
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

    // The device is open: the detectors can start pumping. start() is a no-op
    // if the thread already runs. Width first, for the same reason as the
    // after-restart hook: setWidth() needs the thread stopped.
    for (int i = 0; i < kMaxSlots; ++i)
    {
        auto& controller = *notchControllers_[(std::size_t) i];
        controller.setWidth (engine_.getSlotConfig (i).width);
        controller.start();
    }

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

    // KD-9 detection gating lives HERE because this is the one object that owns
    // both the mode controls and the controllers: Bypass must never place a
    // notch, Soundcheck detects for its 15 s live-time window (KD-7 exempts its
    // notches from auto-release), Auto detects continuously. Applied to every
    // slot the engine has enabled -- a disabled slot has no live chain to
    // protect and its controller must stay silent.
    for (int i = 0; i < kMaxSlots; ++i)
    {
        if (! engine_.getSlotConfig (i).enabled)
            continue;

        auto& controller = *notchControllers_[(std::size_t) i];

        switch (mode)
        {
            case AudioEngine::Mode::Bypass:
                controller.setDetectionActive (false);
                break;
            case AudioEngine::Mode::Auto:
                controller.setDetectionActive (true);
                break;
            case AudioEngine::Mode::Soundcheck:
                controller.startSoundcheck();
                break;
        }
    }
}

bool MainComponent::loadPreset (const juce::File& file)
{
    // Channel counts come from the OPEN device. With no device yet the engine
    // reports zero, but the loader needs real numbers to clamp each slot's
    // channel mapping against -- stereo is what a default interface implies,
    // and matches what startAudio() will find on the common rig.
    const auto channelCount = [] (int reported)
    {
        return reported > 0 ? reported : 2;
    };

    const auto result = PresetManager::loadFromFile (
        file,
        channelCount (engine_.getNumInputChannels()),
        channelCount (engine_.getNumOutputChannels()));

    if (! result.ok)
        return false;

    if (result.skippedNotchCount > 0)
    {
        // A skipped notch is a WARNING, never a refused file (PresetManager.h).
        // The GUI toast is future work; until then the log carries it.
        juce::Logger::writeToLog (
            "preset \"" + file.getFileName() + "\": skipped "
            + juce::String (result.skippedNotchCount)
            + " notch(es) whose routing slot is out of range");
    }

    // Routing configs land FIRST, so the widths adopted below are read back
    // from the engine state this very load established.
    for (const auto& entry : result.preset.slots)
        engine_.setSlotConfig (entry.index, entry.config);

    // Detectors run exactly while audio does (startAudio / the after-restart
    // hook), so that is also the only time they need stopping for setWidth()
    // and adoptPreset(), whose precondition is a STOPPED detector thread.
    // Stop ALL of them once, mutate, restart once -- the §6.5 shape.
    const bool detectorsRunning = engine_.isRunning();

    if (detectorsRunning)
        for (auto& controller : notchControllers_)
            controller->stop (1000);

    // Width resync for EVERY slot whose config landed from the file -- not
    // just notch-bearing ones. A slot declared mono with zero notches today
    // must not detect on two lanes tomorrow. (The channel-aware loader also
    // auto-adds every referenced slot to this list, so every adopt below is
    // covered too. An out-of-range index was already ignored by
    // setSlotConfig() above and is ignored here for the same reason.)
    for (const auto& entry : result.preset.slots)
    {
        if (entry.index < 0 || entry.index >= kMaxSlots)
            continue;

        notchControllers_[(std::size_t) entry.index]->setWidth (
            engine_.getSlotConfig (entry.index).width);
    }

    for (int s = 0; s < kMaxSlots; ++s)
    {
        std::vector<PresetNotch> notchesForSlot;

        for (const auto& notch : result.preset.notches)
            if (notch.slot == s)
                notchesForSlot.push_back (notch);

        if (! notchesForSlot.empty())
            notchControllers_[(std::size_t) s]->adoptPreset (notchesForSlot);
    }

    if (detectorsRunning)
        for (auto& controller : notchControllers_)
            controller->start();

    return true;
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

void MainComponent::applyLayoutState (gui::ScreenLayout layout)
{
    // Same components, different arrangement -- nothing is destroyed or
    // recreated here (spec section 3).
    modeRail_.setOrientation (layout == gui::ScreenLayout::Performance
                                  ? gui::ModeRail::Orientation::Vertical
                                  : gui::ModeRail::Orientation::Horizontal);

    deviceDrawer_.applyLayoutMode (layout);
    deviceDrawer_.setSelectedLayout (layout);
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

    g.setColour (juce::Colours::white);
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

    juce::FlexBox main;
    main.flexDirection = juce::FlexBox::Direction::column;

    // Reserve room for the rail plus a minimum usable spectrum, then clamp the
    // drawer so a too-small window shrinks the drawer instead of overlapping
    // siblings (spec section 3). setResizeLimits should make the clamp a
    // no-op in practice.
    const int reserveBelowDrawer = kMinSpectrumHeight + gap
                                 + (layout_ == gui::ScreenLayout::Performance
                                        ? 320   // vertical rail: 4 cells + label + trailing gaps
                                        : buttonCellHeight);
    const int maxDrawerHeight = juce::jmax (0, area.getHeight() - reserveBelowDrawer);
    const int drawerHeight    = juce::jlimit (0, maxDrawerHeight,
                                              deviceDrawer_.getPreferredHeight());

    main.items.add (juce::FlexItem (deviceDrawer_)
                        .withHeight ((float) drawerHeight)
                        .withMargin ({ 0.0f, 0.0f, (float) gap, 0.0f }));

    if (layout_ == gui::ScreenLayout::Performance)
    {
        // L2: big spectrum with the fixed-width rail down the right edge.
        juce::FlexBox row;
        row.items.add (juce::FlexItem (spectrumView_).withFlex (1.0f));
        row.items.add (juce::FlexItem (modeRail_).withWidth ((float) railWidth));
        main.items.add (juce::FlexItem (row).withFlex (1.0f));
    }
    else
    {
        // L1: horizontal rail strip under the device bar, spectrum below.
        main.items.add (juce::FlexItem (modeRail_)
                            .withHeight ((float) buttonCellHeight)
                            .withMargin ({ 0.0f, 0.0f, (float) gap, 0.0f }));
        main.items.add (juce::FlexItem (spectrumView_).withFlex (1.0f));
    }

    // R-1: NotchListPanel arrives in Task 4. Until then this slot stays null
    // and contributes nothing to the layout.
    if (notchListSlot_ != nullptr)
        main.items.add (juce::FlexItem (*notchListSlot_).withHeight (120.0f));

    main.performLayout (area);
}
