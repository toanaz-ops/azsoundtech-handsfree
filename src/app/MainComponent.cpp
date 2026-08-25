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

// Height of the notch list strip (Task 4): header row plus ~3 visible notch
// rows. The same figure the R-1 slot reservation assumed.
constexpr int kNotchListHeight = 120;

// ApplicationProperties key for the persisted layout (spec G-2 / section 3).
constexpr const char* kLayoutPropertyKey = "layout";
} // namespace

MainComponent::MainComponent (
    const juce::PropertiesFile::Options* propertyOptionsOverride)
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
    , notchListPanel_ (*notchControllers_[0])
    , deviceDrawer_ (devicePanel_)
{
    // The Console-industrial theme, applied once here and inherited by every
    // child through the Component::getLookAndFeel() chain.
    setLookAndFeel (&azLookAndFeel_);

    addAndMakeVisible (spectrumView_);
    addAndMakeVisible (modeRail_);
    addAndMakeVisible (deviceDrawer_);
    addAndMakeVisible (slotScroller_);
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

    // R-5: the rail's LIST cell slides the strip; the open/closed meaning is
    // entirely this side's (L2 toggle vs L1 fixed strip).
    modeRail_.onToggleNotchList = [this] (bool open) { setNotchListOpen (open); };

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

    // The routing table never touches the engine itself: a mapping change is
    // the SAME §6.5 shape as a device change, and it goes through the ONE
    // restart cycle (changeSlotConfig, shared with nothing else) rather than
    // re-typing the thread-join/restart bodies here.
    slotScroller_.setViewedComponent (&slotPanel_, false);
    // The Add button grows the table through this callback: MainComponent::
    // resized() is what sizes slotPanel_ from getPreferredHeight(), and the
    // Viewport parent alone never would.
    slotPanel_.onPreferredHeightChanged = [this] { resized(); };
    slotPanel_.onSlotConfigChanged = [this] (int slotIndex, const SlotConfig& config)
    {
        changeSlotConfig (slotIndex, config);
        slotPanel_.refresh();
    };

    // Detection tuning (brief 2026-08-24): the panel never touches a
    // controller -- a Params change loops over ALL eight controllers here,
    // exactly like modeRail_'s CLEAR ALL loop. Slot 0 is also the read-back
    // source: every controller carries the same values because every change
    // fans out to all of them.
    addAndMakeVisible (tuningPanel_);
    tuningPanel_.onTuningChanged = [this] (const gui::TuningPanel::Params& p)
    {
        for (auto& controller : notchControllers_)
        {
            controller->setRiseReferenceMs ((double) p.riseReferenceMs);
            controller->setPersistenceBlocks (p.persistenceBlocks);
            controller->setNotchDefaults ((double) p.q, (double) p.depthDb);
            controller->setPeakinessThreshold (p.peakinessThreshold);
        }
    };
    tuningPanel_.paramsProvider = [this]
    {
        const auto& c = *notchControllers_[0];
        return gui::TuningPanel::Params { (int) c.getRiseReferenceMs(),
                                          c.getPersistenceBlocks(),
                                          (int) c.getNotchDepthDb(),
                                          (int) c.getNotchQ(),
                                          c.getPeakinessThreshold() };
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
    // Same for the routing table's channel names: empty until the device is
    // open, which is why refresh() has to run again here.
    slotPanel_.refresh();
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
        applyModeGating (i);
}

void MainComponent::applyModeGating (int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= kMaxSlots)
        return;

    if (! engine_.getSlotConfig (slotIndex).enabled)
        return;

    auto& controller = *notchControllers_[(std::size_t) slotIndex];

    switch (engine_.getMode())
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

void MainComponent::changeSlotConfig (int slotIndex, const SlotConfig& config)
{
    jassert (devicePanel_.onBeforeRestart != nullptr);

    if (devicePanel_.onBeforeRestart != nullptr)
        devicePanel_.onBeforeRestart();

    engine_.setSlotConfig (slotIndex, config);

    if (devicePanel_.onAfterRestart != nullptr)
        devicePanel_.onAfterRestart();

    // The restart cycle above restores widths and threads but NOT the
    // detection gate. A slot enabled or re-mapped while Auto/Soundcheck is
    // already running would otherwise stay DEAF until the next mode request
    // -- protection shown on screen that does not exist in the chains. Re-arm
    // it to whatever the current mode says, right now. Soundcheck windows are
    // measured in each controller's OWN live time (D-06), so a slot armed
    // mid-show gets its own full 15 s live-time window; there is no shared
    // clock to inherit a partial one from.
    applyModeGating (slotIndex);
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

    // The DETECTION strip sits directly above the routing table in BOTH
    // layouts (brief 2026-08-24): a fixed ~34 px band carved before the slot
    // block below, so the same code serves L1 and L2.
    tuningPanel_.setBounds (area.removeFromTop (gui::TuningPanel::kPanelHeight));
    area.removeFromTop (gap);

    // The routing table sits under the drawer, scrollable, and yields first:
    // its height is whatever remains once the spectrum's own minimum (and the
    // rail) are reserved. area has ALREADY had the drawer + gap removed, so
    // the only remaining reservation is the one below -- subtracting the
    // drawer again (the pre-2026-08-24 form) starves the table and shears its
    // last visible row.
    const int slotPreferred = slotPanel_.getPreferredHeight();
    const int maxSlotHeight = juce::jmax (0, area.getHeight() - reserveBelowDrawer);
    const int slotHeight = juce::jlimit (0, maxSlotHeight, slotPreferred);

    // Visibility tracks the layout decision: a shrinking window that drops
    // the FlexItem must not leave the scroller sitting at stale bounds.
    slotScroller_.setVisible (slotHeight > 0);

    // Bounds are carved from the remaining area up front (a full-width strip
    // under the drawer) so the SAME code serves both layouts: L2 then lays the
    // spectrum/rail row into what is left, L1 lays its FlexBox column.
    if (slotHeight > 0)
    {
        slotScroller_.setBounds (area.removeFromTop (slotHeight));
        area.removeFromTop (gap);

        // A Viewport never sizes its content by itself: without this the
        // table renders as an empty black rect (the bug the 2026-08-24
        // screenshots showed). Full preferred height for the CURRENT visible
        // row count; the viewport adds a scrollbar only when the window is
        // shorter than the table.
        slotPanel_.setSize (juce::jmax (1, slotScroller_.getMaximumVisibleWidth()),
                            slotPreferred);
    }

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
