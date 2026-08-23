#include "app/MainComponent.h"

#include "gui/DeviceViewModel.h"

namespace
{
// 5 Hz. The status line carries a latency figure and two channel counts; there
// is nothing here a soundman reads faster than that, and the poll costs three
// relaxed atomic loads plus a device query.
constexpr int kStatusRefreshMs = 200;
} // namespace

MainComponent::MainComponent()
    : notchController_ (engine_.getTapBuffer(), engine_.getCommandQueue(), systemClock_)
{
    addAndMakeVisible (devicePanel_);
    addAndMakeVisible (statusBar_);
    addAndMakeVisible (modeBar_);

    modeBar_.onModeRequested = [this] (AudioEngine::Mode mode) { requestMode (mode); };

    // Bridge design §6.5: a device change in the panel is always an engine
    // RESTART, and the rings are cleared on the way -- so the detector thread
    // must be joined first and relaunched after.
    devicePanel_.onBeforeRestart = [this] { notchController_.stop (1000); };
    devicePanel_.onAfterRestart  = [this] { notchController_.start(); };

    // A setting the hardware refused. Held rather than flashed: the user needs
    // to still be reading it a few seconds later.
    devicePanel_.onMessage = [this] (const juce::String& message)
    {
        panelMessage_ = message;
        refreshStatus();
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
    auto area = getLocalBounds();

    area.removeFromTop (36);  // title, drawn in paint()

    devicePanel_.setBounds (area.removeFromTop (64));
    statusBar_  .setBounds (area.removeFromTop (50));
    modeBar_    .setBounds (area.removeFromTop (36));

    // What is left is where the spectrum, the notch overlay and the notch list
    // go (Tasks 19, 20, 22). Those are blocked on the bridge design and are not
    // this lane's.
}
