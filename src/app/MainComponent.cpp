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

// Height of the notch list column: caption, column headers and ~3 visible
// notch rows.
constexpr int kNotchListHeight = 120;

// The largest share of the space under the transport the bottom floor may
// take, however much its contents want. See floorHeightFor().
constexpr float kMaxFloorShare = 0.55f;

// Breathing room added to the height the floor needs, so "it fits" is visibly
// true rather than true to the pixel. See heightThatFitsTheFloor().
constexpr int kFloorSlack = 24;
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
    , modeRail_ (gui::ModeRail::Orientation::Horizontal)
    , notchListPanel_ (*notchControllers_[0])
    , deviceDrawer_ (devicePanel_)
{
    // The Sodium Rack theme, applied once here and inherited by every child
    // through the Component::getLookAndFeel() chain.
    setLookAndFeel (&azLookAndFeel_);

    // The window sizes itself from this (DocumentWindow::setContentOwned), so
    // an unsized content component opens at the resize LIMIT instead.
    //
    // Taken as the LARGER of the nominal default and the height the floor
    // actually needs: the routing table's height depends on how many rows are
    // showing, so a fixed number is a guess that goes stale the moment that
    // changes. parentHierarchyChanged() re-checks once a real window exists.
    setSize (kDefaultWidth, juce::jmax (kDefaultHeight, heightThatFitsTheFloor()));

    // Per-slot tuning (brief 2026-08-24): every slot starts on Global.
    slotUsesGlobalTuning_.fill (true);

    addAndMakeVisible (spectrumView_);
    addAndMakeVisible (modeRail_);
    addAndMakeVisible (deviceDrawer_);
    addAndMakeVisible (slotScroller_);
    // The notch list is a FIXED bottom strip -- always visible.
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

    // The protection badge is MASTHEAD furniture, not drawer furniture. It
    // answers the question the whole window exists to answer, so it belongs in
    // the one band that is always visible and never scrolls.
    addAndMakeVisible (statusBadge_);

    // The masthead's slot selector. It reflects and reports; what a selection
    // MEANS is owned here, in setDisplayedSlot.
    slotTabs_.setSlotCount (slotPanel_.getVisibleRowCount());
    slotTabs_.onSlotSelected = [this] (int slot) { setDisplayedSlot (slot); };

    // Hosted BY the notch panel, so it sits directly above the table it
    // changes. It re-points the analyser too -- that is this class's job to
    // know, not the panel's.
    notchListPanel_.setSlotTabs (&slotTabs_);

    // Seed the display at slot 0 through the SAME route a click takes, so the
    // notch table's caption names its slot from the first frame instead of
    // only after the user has picked something.
    setDisplayedSlot (0);

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
    slotPanel_.onPreferredHeightChanged = [this]
    {
        // Revealing a routing row also makes that slot selectable: a slot with
        // no row in the table is a slot the user has no way to configure, and
        // monitoring one would show a spectrum they cannot act on.
        slotTabs_.setSlotCount (slotPanel_.getVisibleRowCount());

        if (displayedSlot_ >= slotPanel_.getVisibleRowCount())
            setDisplayedSlot (0);

        // The "+ Add slot" button revealed a row; the window takes the height
        // to show it rather than pushing the button under a scrollbar. Called
        // BEFORE resized() so the layout runs once, at the final size.
        growWindowToFitFloor();

        resized();
    };
    slotPanel_.onSlotConfigChanged = [this] (int slotIndex, const SlotConfig& config)
    {
        changeSlotConfig (slotIndex, config);
        slotPanel_.refresh();
    };

    // Per-slot tuning (brief 2026-08-24): the panel reports a COMPLETE
    // SlotTuning; here is where it means something. Global just flips the
    // flag -- the strip keeps driving the controller. Custom applies the five
    // values to THAT slot's controller through the existing clamping setters.
    slotPanel_.onSlotTuningChanged = [this] (int slotIndex,
                                             const gui::SlotPanel::SlotTuning& t)
    {
        if (slotIndex < 0 || slotIndex >= kMaxSlots)
            return;

        auto& controller = *notchControllers_[(std::size_t) slotIndex];
        slotUsesGlobalTuning_[(std::size_t) slotIndex] = t.usesGlobal;

        if (! t.usesGlobal)
        {
            controller.setRiseReferenceMs (t.riseMs);
            controller.setPersistenceBlocks (t.persist);
            controller.setNotchDefaults (t.q, t.depthDb);
            controller.setPeakinessThreshold ((float) t.thr);
        }
    };

    // The editor seeds itself from the slot's controller plus its mode flag.
    slotPanel_.slotTuningProvider = [this] (int slotIndex) -> gui::SlotPanel::SlotTuning
    {
        if (slotIndex < 0 || slotIndex >= kMaxSlots)
            return {};

        const auto& c = *notchControllers_[(std::size_t) slotIndex];

        gui::SlotPanel::SlotTuning t;
        t.usesGlobal = slotUsesGlobalTuning_[(std::size_t) slotIndex];
        t.riseMs  = c.getRiseReferenceMs();
        t.persist = c.getPersistenceBlocks();
        t.depthDb = c.getNotchDepthDb();
        t.q       = c.getNotchQ();
        t.thr     = (double) c.getPeakinessThreshold();
        return t;
    };

    // Detection tuning (brief 2026-08-24): the panel never touches a
    // controller -- a Params change loops over the controllers HERE, exactly
    // like modeRail_'s CLEAR ALL loop. Per-slot tuning (brief 2026-08-24):
    // a slot switched to Custom is SKIPPED -- it keeps its own values until
    // its Tune combo goes back to G. Slot 0 is also the read-back source for
    // the strip: every Global controller carries the same values because every
    // change fans out to all of them.
    addAndMakeVisible (tuningPanel_);
    tuningPanel_.onTuningChanged = [this] (const gui::TuningPanel::Params& p)
    {
        for (int i = 0; i < kMaxSlots; ++i)
        {
            if (! slotUsesGlobalTuning_[(std::size_t) i])
                continue;

            auto& controller = *notchControllers_[(std::size_t) i];
            controller.setRiseReferenceMs ((double) p.riseReferenceMs);
            controller.setPersistenceBlocks (p.persistenceBlocks);
            controller.setNotchDefaults ((double) p.q, (double) p.depthDb);
            controller.setPeakinessThreshold (p.peakinessThreshold);
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

void MainComponent::setDisplayedSlot (const int slotIndex)
{
    if (! juce::isPositiveAndBelow (slotIndex, slotPanel_.getVisibleRowCount()))
        return;

    displayedSlot_ = slotIndex;

    auto& controller = *notchControllers_[(std::size_t) slotIndex];

    // BOTH panels, together. Each drops everything it derived from the slot it
    // was on -- see their setController comments for why an age ledger cannot
    // cross a slot boundary.
    spectrumView_.setController (controller);
    notchListPanel_.setController (controller);
    notchListPanel_.setDisplayedSlot (slotIndex);

    slotTabs_.setSelected (slotIndex);
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

    // The masthead readout. A device error replaces the rig line entirely --
    // when the interface is gone, its sample rate is not the news.
    rigIsHealthy_ = status.running && message.isEmpty();

    // Device presence, stated separately from protection state.
    deviceIsUp_ = status.running;
    deviceLine_ = engine_.getLastDeviceError().isNotEmpty() ? "DEVICE ERROR"
                : status.running                           ? "DEVICE OK"
                                                           : "NO DEVICE";

    const double cpu = engine_.getCpuUsage();
    cpuIsHot_ = cpu >= kCpuWarnFraction;
    cpuLine_  = status.running
                    ? "CPU " + juce::String (juce::roundToInt (cpu * 100.0)) + "%"
                    : juce::String ("CPU --");

    const auto next = message.isNotEmpty() ? message
                                           : gui::formatStatusLine (status);

    if (next != rigLine_)
    {
        rigLine_ = next;
        repaint (getLocalBounds().removeFromTop (az::theme::mastheadHeight));
    }
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

    // The RAIL is the one the user actually looks at, and its lit lamp is the
    // primary "what mode am I in" signal in this design -- so it has to track
    // the engine too, not just the last click it received.
    switch (engine_.getMode())
    {
        case AudioEngine::Mode::Soundcheck:
            modeRail_.setDisplayedMode (gui::ModeRail::Mode::Soundcheck);
            break;
        case AudioEngine::Mode::Auto:
            modeRail_.setDisplayedMode (gui::ModeRail::Mode::Auto);
            break;
        case AudioEngine::Mode::Bypass:
            modeRail_.setDisplayedMode (gui::ModeRail::Mode::Bypass);
            break;
    }
}

//==============================================================================
// The window's own painting: the masthead, the raised transport and floor
// bands, and the engraved grooves that separate them. Every child paints its
// own inside.

void MainComponent::paint (juce::Graphics& g)
{
    using namespace az::theme;

    g.fillAll (background);

    auto area = getLocalBounds();

    //--------------------------------------------------------------------
    // Masthead: mark, rig readout, protection badge (a child, positioned in
    // resized()).
    auto masthead = area.removeFromTop (mastheadHeight);
    g.setColour (panel);
    g.fillRect (masthead);
    drawEngravedDivider (g, masthead);

    auto mark = masthead.reduced (kEdgePad, 0);

    // The sodium bar IS the brand mark. It is also the only place on screen
    // the accent appears without meaning "a notch just fired".
    g.setColour (accent);
    g.fillRect (mark.removeFromLeft (3).withSizeKeepingCentre (3, 18));
    mark.removeFromLeft (10);

    g.setColour (text);
    g.setFont (legendFont (brandFontSize, true, trackingCaption));
    g.drawText ("HANDS-FREE", mark.removeFromLeft (kNameWidth),
                juce::Justification::centredLeft, false);
    mark.removeFromLeft (gap + spacing);

    g.setColour (dim);
    g.setFont (legendFont (columnFontSize, false, trackingCaption));
    g.drawText ("AZ SOUNDTECH", mark.removeFromLeft (kCompanyWidth),
                juce::Justification::centredLeft, false);

    // Right of the masthead, reading outward from the protection badge:
    //
    //     ... rig line ...   CPU 12%   [• DEVICE OK]   [• PROTECTING]
    //
    // The badge itself is a child and was positioned in resized(); everything
    // else here is painted, because none of it is interactive.
    auto right = masthead.reduced (kEdgePad, 0);
    right.removeFromRight (kBadgeWidth + gap);

    const auto deviceChip = right.removeFromRight (kDeviceChipWidth)
                                 .withSizeKeepingCentre (kDeviceChipWidth, kBadgeHeight);
    right.removeFromRight (gap);

    const auto deviceColour = deviceLine_ == "DEVICE ERROR" ? danger
                            : deviceIsUp_                   ? ok
                                                            : dim;

    g.setColour (well);
    g.fillRoundedRectangle (deviceChip.toFloat(), cornerRadius);
    g.setColour (border);
    g.drawRoundedRectangle (deviceChip.toFloat().reduced (0.5f), cornerRadius, 1.0f);

    constexpr float dotSize = 8.0f;
    g.setColour (deviceColour);
    g.fillEllipse ((float) deviceChip.getX() + 10.0f,
                   (float) deviceChip.getCentreY() - dotSize * 0.5f,
                   dotSize, dotSize);

    g.setFont (legendFont (columnFontSize, true, trackingColumn));
    g.drawText (deviceLine_, deviceChip.withTrimmedLeft (24),
                juce::Justification::centredLeft, false);

    // CPU: a number, so mono, and no chrome around it -- it is a reading, not
    // a state.
    const auto cpuArea = right.removeFromRight (kCpuChipWidth);
    right.removeFromRight (gap);

    g.setColour (cpuIsHot_ ? warn : dim);
    g.setFont (monoFont (readoutFontSize));
    g.drawText (cpuLine_, cpuArea, juce::Justification::centredRight, false);

    // The rig readout fills whatever is left between the mark and those.
    auto rigArea = right.withTrimmedLeft (kMarkWidth);
    if (rigArea.getWidth() > 0)
    {
        g.setColour (rigIsHealthy_ ? dim : warn);
        g.setFont (monoFont (readoutFontSize));
        g.drawText (rigLine_, rigArea, juce::Justification::centredRight, true);
    }

    //--------------------------------------------------------------------
    // Transport: a raised band, so the switches sit ON something instead of
    // floating on the canvas.
    auto transport = area.removeFromTop (transportHeight);
    g.setColour (panel);
    g.fillRect (transport);
    drawEngravedDivider (g, transport);

    //--------------------------------------------------------------------
    // Floor: the same raised band at the window's bottom, with a vertical
    // groove milled between its two columns.
    const auto floor = floorBoundsForPaint();
    if (floor.getHeight() > 2 * gap)
    {
        g.setColour (panel);
        g.fillRect (floor);

        // The groove ABOVE the floor: drawEngravedDivider draws on a band's
        // bottom edge, so it is handed a zero-height band sitting one pixel up.
        drawEngravedDivider (g, floor.withHeight (0).translated (0, -1));

        const int grooveX = floor.getX() + kEdgePad
                          + juce::roundToInt ((float) (floor.getWidth() - 2 * kEdgePad)
                                              * kNotchColumnFraction)
                          + gap;

        g.setColour (shade);
        g.fillRect (grooveX, floor.getY() + gap, 1, floor.getHeight() - 2 * gap);
        g.setColour (sheen);
        g.fillRect (grooveX + 1, floor.getY() + gap, 1, floor.getHeight() - 2 * gap);
    }
}

//==============================================================================
// Layout.
//
// The shape, and why: masthead / transport / ANALYSER / floor. The analyser is
// the only thing on screen that changes thirty times a second, so it takes
// every pixel the fixed bands do not need. The pre-rebuild layout gave it a
// middling slice between seven equal-weight full-width strips, which is most
// of why nothing on screen read as more important than anything else.

int MainComponent::naturalFloorHeight() const
{
    using namespace az::theme;

    // What the rig column WANTS: device drawer, detection strip, routing table.
    const int rigColumn = deviceDrawer_.getPreferredHeight()
                        + gap + gui::TuningPanel::kPanelHeight
                        + gap + slotPanel_.getPreferredHeight();

    return juce::jmax (kNotchListHeight, rigColumn) + 2 * gap;
}

int MainComponent::heightThatFitsTheFloor() const
{
    using namespace az::theme;

    const int natural = naturalFloorHeight();
    const int chrome  = mastheadHeight + transportHeight;

    // BOTH ceilings in floorHeightFor have to clear, or the floor is trimmed
    // anyway and the extra height goes to the analyser instead of to the row
    // the user just asked for.
    const int forAnalyser = chrome + kMinSpectrumHeight + gap + natural;
    const int forShare    = chrome + juce::roundToInt ((float) natural / kMaxFloorShare);

    // Plus slack. Without it the floor fits EXACTLY, which means one pixel of
    // rounding anywhere puts the Add row under the window edge -- and "fits
    // exactly" is indistinguishable from "clipped" to the person looking at it.
    return juce::jmax (kMinimumHeight, juce::jmax (forAnalyser, forShare)) + kFloorSlack;
}

void MainComponent::parentHierarchyChanged()
{
    // The app used to open at a height where the routing table's "+ Add slot"
    // row landed exactly on the window's bottom edge, so the first thing a
    // user had to do was drag the window taller. Sizing here, once a real
    // window exists, is what makes the opening size honest.
    growWindowToFitFloor();
}

void MainComponent::growWindowToFitFloor()
{
    // Headless (and in every test): a component with no desktop peer is its
    // own top level, and there is no window to grow.
    auto* window = getTopLevelComponent();
    if (window == nullptr || window == this)
        return;

    const int wanted = heightThatFitsTheFloor();
    if (wanted <= getHeight())
        return;

    // Never past the display the window is on. A window taller than the screen
    // puts the row the user just revealed under the taskbar, which is the
    // problem this is here to solve.
    const auto* display = juce::Desktop::getInstance().getDisplays()
                              .getDisplayForRect (window->getScreenBounds());

    const int grown = window->getHeight() + (wanted - getHeight());
    const int ceiling = display != nullptr ? display->userArea.getHeight() : grown;

    window->setSize (window->getWidth(), juce::jmin (ceiling, grown));
}

int MainComponent::floorHeightFor (const int available) const
{
    using namespace az::theme;

    const int wanted = naturalFloorHeight();

    // TWO ceilings, and the floor gets the lower of them.
    //
    // The first is the analyser's hard minimum, reserved before the floor gets
    // anything: a short window shrinks the floor -- whose routing table
    // scrolls -- rather than the analyser, which has nowhere to go.
    //
    // The second is a SHARE cap, and it is the one that matters in practice.
    // The rig column's natural height is fixed (device + detection + two slot
    // rows), so without a cap a modest window hands the floor more pixels than
    // the analyser -- which inverts the whole point of the layout. The
    // analyser is the only thing on screen that moves, and it keeps the
    // majority of the space at every window size.
    const int analyserFloor = juce::jmax (0, available - kMinSpectrumHeight - gap);
    const int shareCeiling  = juce::roundToInt ((float) available * kMaxFloorShare);

    return juce::jlimit (0, juce::jmin (analyserFloor, shareCeiling), wanted);
}

juce::Rectangle<int> MainComponent::floorBoundsForPaint() const
{
    using namespace az::theme;

    auto area = getLocalBounds();
    area.removeFromTop (mastheadHeight);
    area.removeFromTop (transportHeight);

    return area.removeFromBottom (floorHeightFor (area.getHeight()));
}

void MainComponent::resized()
{
    using namespace az::theme;

    auto area = getLocalBounds();

    //--------------------------------------------------------------------
    // 1. Masthead -- painted; the badge is its only child.
    auto masthead = area.removeFromTop (mastheadHeight).reduced (kEdgePad, 0);
    statusBadge_.setBounds (masthead.removeFromRight (kBadgeWidth)
                                    .withSizeKeepingCentre (kBadgeWidth, kBadgeHeight));

    // The selector's WIDTH is set here and its position by its host panel:
    // the chip count follows the routing table's visible rows and changes
    // while the app runs, so the width is asked for rather than assumed.
    slotTabs_.setSize (slotTabs_.getPreferredWidth(), kBadgeHeight);

    //--------------------------------------------------------------------
    // 2. Transport. The rail owns its internal grid; all this owes it is a
    //    band of the right height with the window margin applied.
    auto transport = area.removeFromTop (transportHeight);
    modeRail_.setBounds (transport.reduced (kEdgePad, gap + spacing));

    //--------------------------------------------------------------------
    // 3. The floor, carved off the bottom BEFORE the analyser is measured.
    notchListPanel_.setVisible (true);

    auto floor = area.removeFromBottom (floorHeightFor (area.getHeight()));

    //--------------------------------------------------------------------
    // 4. The analyser takes everything that is left.
    spectrumView_.setBounds (area.reduced (kEdgePad, 0).withTrimmedBottom (gap));

    //--------------------------------------------------------------------
    // 5. Floor columns: the notch table reads left -- it is the answer -- and
    //    the rig sits right, because it is the setup you touch once.
    auto inner = floor.reduced (kEdgePad, gap);

    const int notchWidth = juce::roundToInt ((float) inner.getWidth() * kNotchColumnFraction);
    notchListPanel_.setBounds (inner.removeFromLeft (notchWidth));
    notchListPanel_.resized();   // re-place the hosted selector at the new width
    inner.removeFromLeft (2 * gap + 2);   // the painted groove lives in here

    const int drawerHeight = juce::jmin (deviceDrawer_.getPreferredHeight(),
                                         inner.getHeight());
    deviceDrawer_.setBounds (inner.removeFromTop (drawerHeight));
    inner.removeFromTop (gap);

    if (inner.getHeight() > gui::TuningPanel::kPanelHeight)
    {
        tuningPanel_.setVisible (true);
        tuningPanel_.setBounds (inner.removeFromTop (gui::TuningPanel::kPanelHeight));
        inner.removeFromTop (gap);
    }
    else
    {
        tuningPanel_.setVisible (false);
    }

    // Visibility tracks the layout decision: a shrinking window that drops the
    // table must not leave the scroller sitting at stale bounds.
    slotScroller_.setVisible (inner.getHeight() > 0);

    if (inner.getHeight() > 0)
    {
        slotScroller_.setBounds (inner);

        // A Viewport never sizes its content by itself: without this the table
        // renders as an empty black rect (the 2026-08-24 bug). Full preferred
        // height for the CURRENT visible row count; the viewport adds a
        // scrollbar only when the column is shorter than the table.
        // At LEAST the table's own preferred width: below that the Viewport
        // scrolls sideways rather than clipping the TUNE column off the edge.
        slotPanel_.setSize (juce::jmax (slotScroller_.getMaximumVisibleWidth(),
                                        slotPanel_.getPreferredWidth()),
                            slotPanel_.getPreferredHeight());
    }
}
