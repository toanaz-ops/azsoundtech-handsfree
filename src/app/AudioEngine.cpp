#include "app/AudioEngine.h"

namespace
{
constexpr int kNumChannels = 2;  // stereo in/out
}

AudioEngine::AudioEngine()
{
    // Legacy default mapping: slot 0 enabled stereo {0,1} -> {0,1}, slots 1..7
    // empty. This is exactly the pre-multi-slot behaviour, so nothing that
    // only ever used slot 0 changes.
    slotEnabled_[0].store (true,  std::memory_order_relaxed);
    slotWidth_[0].store  (2,      std::memory_order_relaxed);
    slotInCh_[0][0].store (0,     std::memory_order_relaxed);
    slotInCh_[0][1].store (1,     std::memory_order_relaxed);
    slotOutCh_[0][0].store (0,    std::memory_order_relaxed);
    slotOutCh_[0][1].store (1,    std::memory_order_relaxed);

    // The chains are pre-built at the nominal 48 kHz rate.
    // audioDeviceAboutToStart() retargets every chain to the device's actual
    // rate via NotchChain::setSampleRate() before any notch is set, so the
    // coefficients produced by a later setNotch() call always match the
    // running device (Task 9 Part C).
}

AudioEngine::~AudioEngine()
{
    stop();
}

//==============================================================================
// Device management

void AudioEngine::start()
{
    if (isRunning_.load (std::memory_order_acquire))
        return;

    // 1) Switch to the requested device type (default "ASIO") if it differs
    //    from what is currently selected. JUCE synchronously opens a device
    //    of the new type, or silently keeps the current one if the requested
    //    type is not registered (e.g. ASIO SDK not installed).
    if (deviceManager_.getCurrentAudioDeviceType() != desiredDeviceType_)
        deviceManager_.setCurrentAudioDeviceType (desiredDeviceType_, true);

    // 2) Open the device: the requested device name if one was given,
    //    otherwise the driver's default device.
    const juce::String error = desiredDeviceName_.isEmpty()
                                   ? deviceManager_.initialiseWithDefaultDevices (kNumChannels, kNumChannels)
                                   : deviceManager_.initialise (kNumChannels, kNumChannels, nullptr, true,
                                                                desiredDeviceName_, nullptr);

    if (error.isNotEmpty())
    {
        // Retain it, so the GUI can say WHY the device did not open rather
        // than just showing a dark indicator.
        {
            const std::lock_guard<std::mutex> lock (lastDeviceErrorLock_);
            lastDeviceError_ = error;
        }

        juce::Logger::writeToLog ("AudioEngine: failed to open audio device: " + error);
        return;
    }

    // The device opened: whatever went wrong last time no longer applies.
    {
        const std::lock_guard<std::mutex> lock (lastDeviceErrorLock_);
        lastDeviceError_.clear();
    }

    // 3) Register this as the audio callback. If the device is already
    //    running, audioDeviceAboutToStart() is invoked before this returns.
    deviceManager_.addAudioCallback (this);
    isRunning_.store (true, std::memory_order_release);
}

void AudioEngine::stop()
{
    // Exchange-and-check: tear down exactly once even if stop() is called
    // twice, or the device died underneath us (audioDeviceError also clears
    // the flag from the device thread).
    if (! isRunning_.exchange (false, std::memory_order_acq_rel))
        return;

    // removeAudioCallback() blocks until the audio thread has released the
    // callback, so it must never be called from the audio thread itself.
    deviceManager_.removeAudioCallback (this);
    deviceManager_.closeAudioDevice();
}

bool AudioEngine::isRunning() const
{
    return isRunning_.load (std::memory_order_acquire);
}

//==============================================================================
// ASIO device control

void AudioEngine::setAudioDeviceType (const juce::String& typeName)
{
    desiredDeviceType_ = typeName;  // applied on the next start()
}

void AudioEngine::setAudioDevice (const juce::String& deviceName)
{
    desiredDeviceName_ = deviceName;  // applied on the next start()
}

juce::String AudioEngine::getCurrentDeviceName() const
{
    if (auto* device = deviceManager_.getCurrentAudioDevice())
        return device->getName();

    return {};
}

juce::String AudioEngine::getCurrentSampleRate() const
{
    return juce::String (currentSampleRate_.load (std::memory_order_acquire)) + " Hz";
}

int AudioEngine::getCurrentBufferSize() const
{
    return currentBufferSize_.load (std::memory_order_acquire);
}

double AudioEngine::getCpuUsage() const
{
    return deviceManager_.getCpuUsage();
}

double AudioEngine::getCurrentLatency() const
{
    if (auto* device = deviceManager_.getCurrentAudioDevice())
    {
        const double sampleRate = device->getCurrentSampleRate();

        if (sampleRate > 0.0)
            return device->getOutputLatencyInSamples() / sampleRate;
    }

    return 0.0;
}

//==============================================================================
// Device enumeration, configuration and status.
//
// Every one of these runs before start() in the GUI's startup path, when
// deviceManager_.getCurrentAudioDevice() is nullptr. The nullptr guard is the
// contract, not defensive noise.

juce::StringArray AudioEngine::getAvailableDeviceTypeNames()
{
    juce::StringArray names;

    for (auto* type : deviceManager_.getAvailableDeviceTypes())
    {
        if (type != nullptr)
            names.add (type->getTypeName());
    }

    return names;
}

juce::StringArray AudioEngine::getAvailableDeviceNames()
{
    auto* type = deviceManager_.getCurrentDeviceTypeObject();

    if (type == nullptr)
        return {};

    // JUCE requires scanForDevices() before getDeviceNames() -- without it the
    // list is whatever the last scan found, or empty on a fresh type object.
    // This is why the method cannot be const.
    type->scanForDevices();
    return type->getDeviceNames();
}

juce::String AudioEngine::getCurrentDeviceType() const
{
    // The manager's ACTUAL type, not desiredDeviceType_. See the header.
    return deviceManager_.getCurrentAudioDeviceType();
}

juce::Array<double> AudioEngine::getAvailableSampleRates()
{
    if (auto* device = deviceManager_.getCurrentAudioDevice())
        return device->getAvailableSampleRates();

    return {};
}

juce::Array<int> AudioEngine::getAvailableBufferSizes()
{
    if (auto* device = deviceManager_.getCurrentAudioDevice())
        return device->getAvailableBufferSizes();

    return {};
}

bool AudioEngine::setSampleRate (double newRate)
{
    if (newRate <= 0.0)
        return false;

    if (deviceManager_.getCurrentAudioDevice() == nullptr)
        return false;

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    deviceManager_.getAudioDeviceSetup (setup);

    if (setup.sampleRate == newRate)
        return true;   // already there; not a failure

    setup.sampleRate = newRate;

    // setAudioDeviceSetup returns an EMPTY string on success and the error text
    // on failure -- the inverse of the usual convention, so the test reads
    // backwards on purpose. A rate the hardware refuses lands here rather than
    // being applied silently.
    return deviceManager_.setAudioDeviceSetup (setup, true).isEmpty();
}

bool AudioEngine::setBufferSize (int newSize)
{
    if (newSize <= 0)
        return false;

    if (deviceManager_.getCurrentAudioDevice() == nullptr)
        return false;

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    deviceManager_.getAudioDeviceSetup (setup);

    if (setup.bufferSize == newSize)
        return true;

    setup.bufferSize = newSize;
    return deviceManager_.setAudioDeviceSetup (setup, true).isEmpty();
}

double AudioEngine::getCurrentSampleRateHz() const
{
    return currentSampleRate_.load (std::memory_order_acquire);
}

int AudioEngine::getNumInputChannels() const
{
    return numInputChannels_.load (std::memory_order_relaxed);
}

int AudioEngine::getNumOutputChannels() const
{
    return numOutputChannels_.load (std::memory_order_relaxed);
}

juce::String AudioEngine::getLastDeviceError() const
{
    const std::lock_guard<std::mutex> lock (lastDeviceErrorLock_);
    return lastDeviceError_;
}

//==============================================================================
// Mode control

void AudioEngine::setMode (Mode mode)
{
    currentMode_.store (mode, std::memory_order_release);
}

AudioEngine::Mode AudioEngine::getMode() const
{
    return currentMode_.load (std::memory_order_acquire);
}

LockFreeRingBuffer<float>& AudioEngine::getTapBuffer()
{
    return getTapBuffer (0);
}

LockFreeRingBuffer<float>& AudioEngine::getTapBuffer (int slot)
{
    // Clamped, never OOB: tests and future callers get slot 0 for bad input,
    // matching the getNotchChainForTest convention.
    return tapBuffers_[(std::size_t) (slot < 0 ? 0 : (slot >= kMaxSlots ? 0 : slot))];
}

std::uint64_t AudioEngine::getTapDropCount() const
{
    return getTapDropCount (0);
}

std::uint64_t AudioEngine::getTapDropCount (int slot) const
{
    return tapDropCounts_[(std::size_t) (slot < 0 ? 0 : (slot >= kMaxSlots ? 0 : slot))]
        .load (std::memory_order_relaxed);
}

LockFreeRingBuffer<NotchCommand>& AudioEngine::getCommandQueue()
{
    return getCommandQueue (0);
}

LockFreeRingBuffer<NotchCommand>& AudioEngine::getCommandQueue (int slot)
{
    return commandQueues_[(std::size_t) (slot < 0 ? 0 : (slot >= kMaxSlots ? 0 : slot))];
}

void AudioEngine::setSlotConfig (int slotIndex, const SlotConfig& config)
{
    if (slotIndex < 0 || slotIndex >= kMaxSlots)
        return;

    // Relaxed stores are sufficient WITHOUT a restart: the audio callback
    // snapshots the mapping at the top of every block and bounds-checks each
    // lane against the device's actual channel counts before use, so these
    // landing mid-callback cost at most one block with a valid-but-mixed
    // route -- never an out-of-bounds access.
    const auto s = (std::size_t) slotIndex;
    slotEnabled_[s].store (config.enabled, std::memory_order_relaxed);
    slotWidth_[s].store  (config.width,    std::memory_order_relaxed);

    for (int lane = 0; lane < kMaxSlotLanes; ++lane)
    {
        slotInCh_[s][(std::size_t) lane].store (config.inputChannels[lane],
                                                std::memory_order_relaxed);
        slotOutCh_[s][(std::size_t) lane].store (config.outputChannels[lane],
                                                 std::memory_order_relaxed);
    }
}

SlotConfig AudioEngine::getSlotConfig (int slotIndex) const
{
    SlotConfig c;

    if (slotIndex < 0 || slotIndex >= kMaxSlots)
        return c;

    const auto s = (std::size_t) slotIndex;
    c.enabled = slotEnabled_[s].load (std::memory_order_relaxed);
    c.width   = slotWidth_[s].load (std::memory_order_relaxed);

    for (int lane = 0; lane < kMaxSlotLanes; ++lane)
    {
        c.inputChannels[lane]  = slotInCh_[s][(std::size_t) lane].load (std::memory_order_relaxed);
        c.outputChannels[lane] = slotOutCh_[s][(std::size_t) lane].load (std::memory_order_relaxed);
    }

    return c;
}

juce::StringArray AudioEngine::getInputChannelNames()
{
    if (auto* device = deviceManager_.getCurrentAudioDevice())
        return device->getInputChannelNames();

    return {};
}

juce::StringArray AudioEngine::getOutputChannelNames()
{
    if (auto* device = deviceManager_.getCurrentAudioDevice())
        return device->getOutputChannelNames();

    return {};
}

const NotchChain& AudioEngine::getNotchChainForTest (int channel) const
{
    // Out-of-range callers get channel 0 clamped; tests never pass bad input,
    // and the sentinel dance of NotchChain::getNotchInfo would be overkill here.
    return notchChains_[0][(std::size_t) (channel < 0 ? 0 : (channel > 1 ? 0 : channel))];
}

const NotchChain& AudioEngine::getNotchChainForTest (int slot, int lane) const
{
    if (slot < 0 || slot >= kMaxSlots || lane < 0 || lane >= kMaxSlotLanes)
        return notchChains_[0][0];

    return notchChains_[(std::size_t) slot][(std::size_t) lane];
}

int AudioEngine::drainCommandsFrom (LockFreeRingBuffer<NotchCommand>& ring, int slot,
                                    int maxCommands)
{
    // Real-time discipline: one bulk read into a pre-allocated stack array,
    // then apply. Bounds-check EVERY field used as an index -- content that
    // crossed a lock-free ring is trusted only after it is checked. The batch
    // is capped by the caller's REMAINING shared budget, and the return value
    // is what the caller charges against it.
    if (maxCommands <= 0)
        return 0;

    NotchCommand batch[kMaxCommandsPerCallback];
    const std::size_t want = (std::size_t) ((maxCommands < kMaxCommandsPerCallback)
                                                ? maxCommands
                                                : kMaxCommandsPerCallback);
    const std::size_t count = ring.read (batch, want);

    for (std::size_t i = 0; i < count; ++i)
    {
        const NotchCommand& cmd = batch[i];

        // A command only applies to the slot whose queue carried it -- the
        // `slot` field must agree with the ring it arrived on. (channel is
        // uint8_t, so it can never be negative.) cmd.index >= 16 would index
        // past the chain's notch slots.
        if (cmd.slot != slot || cmd.channel >= kMaxSlotLanes || cmd.index >= 16)
            continue;   // corrupt or hostile command: skip, never index OOB

        switch (cmd.type)
        {
            case NotchCommandType::Set:
                notchChains_[(std::size_t) slot][(std::size_t) cmd.channel]
                    .setNotch (cmd.index, cmd.frequency, cmd.Q, cmd.depthDB);
                break;
            case NotchCommandType::Clear:
                notchChains_[(std::size_t) slot][(std::size_t) cmd.channel]
                    .clearNotch (cmd.index);
                break;
        }
    }

    return (int) count;
}

//==============================================================================
// AudioIODeviceCallback -- real-time audio thread.

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                                    int numInputChannels,
                                                    float* const* outputChannelData,
                                                    int numOutputChannels,
                                                    int numSamples,
                                                    const juce::AudioIODeviceCallbackContext&)
{
    // FTZ/DAZ for the whole callback. Must be the FIRST statement.
    //
    // A notch biquad at Q=10 has poles at radius ~0.9935. On digital silence
    // -- an engineer muting the mic after soundcheck -- the Direct-Form state
    // decays into the subnormal range after ~108,000 samples and then never
    // reaches zero: the smallest positive double is 4.94e-324 and
    // 0.9935 * 4.94e-324 rounds straight back to 4.94e-324, so the feedback
    // term sustains a permanent subnormal limit cycle. Subnormal SSE
    // arithmetic traps to microcode.
    //
    // Measured here, 16 active notches, Release build, steady state:
    // 77 ms of CPU per second of audio with denormals enabled versus 0.9 ms
    // with them flushed -- an ~80x penalty that at a 32-sample ASIO buffer
    // eats a third of the callback budget and makes xruns likely. The only
    // other escape is Biquad::reset(), which only happens on device restart.
    //
    // ScopedNoDenormals sets FTZ/DAZ in MXCSR for this scope and restores the
    // previous value on exit. It costs two register writes per callback and
    // nothing per sample. (Verified 2026-08-21 that the build sets no /fp:fast
    // and no FTZ flag anywhere: CMAKE_CXX_FLAGS_RELEASE is /O2 /Ob2 /DNDEBUG
    // and juce_recommended_config_flags adds only /Ox /MP /EHsc.)
    // tests/test_biquad.cpp SilenceDoesNotLeaveDenormalState pins both halves.
    const juce::ScopedNoDenormals noDenormals;

    // Commands first (bridge design §2): a notch commanded this callback takes
    // effect on THIS callback's samples instead of being one buffer late.
    // Every slot's queue is visited under ONE SHARED budget of
    // kMaxCommandsPerCallback across ALL eight rings -- eight per-ring caps
    // would multiply the worst-case recompute time by 8. A ring holding more
    // than the budget allows keeps the rest for the NEXT callback: delayed,
    // never dropped.
    int commandBudget = kMaxCommandsPerCallback;
    for (int slot = 0; slot < kMaxSlots && commandBudget > 0; ++slot)
        commandBudget -= drainCommandsFrom (commandQueues_[(std::size_t) slot],
                                            slot, commandBudget);

    // Publish what this callback was ACTUALLY handed, for the status display.
    // Two relaxed stores per callback: no allocation, no lock, no ordering
    // relationship with anything else, and read only for display. Sourcing the
    // counts here rather than from the device handle is deliberate -- a device
    // can advertise channels the callback is not given.
    numInputChannels_.store (numInputChannels, std::memory_order_relaxed);
    numOutputChannels_.store (numOutputChannels, std::memory_order_relaxed);

    // Real-time thread: no allocation, no locking. The only shared state read
    // here is the mode and the slot mapping, via lock-free atomics. The mode
    // AND the mapping are snapshotted ONCE at the top of the block: a config
    // change mid-block would otherwise mix lanes from two mappings inside one
    // output buffer.
    const bool bypass = (currentMode_.load (std::memory_order_acquire) == Mode::Bypass);

    struct LaneRef { const float* in; float* out; NotchChain* chain; };

    // Stack-resident lane table -- no heap, no lock. Upper bound: every slot
    // stereo = 8 x 2 lanes.
    LaneRef lanes[kMaxSlots * kMaxSlotLanes];
    int numLanes = 0;

    // Per-slot tap source: slot s taps its LANE 0 output post-DSP when that
    // lane exists and is valid. Null means "do not tap this slot" -- disabled
    // slot, invalid width, or no valid lane 0.
    const float* tapSource[kMaxSlots] {};

    for (int slot = 0; slot < kMaxSlots; ++slot)
    {
        if (! slotEnabled_[slot].load (std::memory_order_relaxed))
            continue;

        // Width outside {1,2} is treated as an empty/disabled slot (ledger
        // ruling from the Task 1 review) rather than being clamped -- a bad
        // value must never widen into an unplanned route.
        const int width = slotWidth_[slot].load (std::memory_order_relaxed);

        if (width != 1 && width != 2)
            continue;

        for (int lane = 0; lane < width; ++lane)
        {
            const int inIdx  = slotInCh_[slot][(std::size_t) lane].load (std::memory_order_relaxed);
            const int outIdx = slotOutCh_[slot][(std::size_t) lane].load (std::memory_order_relaxed);

            if (inIdx < 0 || inIdx >= numInputChannels
                || outIdx < 0 || outIdx >= numOutputChannels)
                continue;

            const float* in = (inputChannelData != nullptr)
                                  ? inputChannelData[inIdx] : nullptr;
            float* out      = (outputChannelData != nullptr)
                                  ? outputChannelData[outIdx] : nullptr;

            if (in == nullptr || out == nullptr)
                continue;

            lanes[numLanes++] = { in, out,
                                  &notchChains_[(std::size_t) slot][(std::size_t) lane] };

            if (lane == 0)
                tapSource[slot] = out;
        }
    }

    // JUCE requires every output channel to be written or cleared, and the
    // output buffers are NOT pre-cleared for us. Clearing ALL of them up front
    // lets the DSP accumulate (`out[n] +=`) on top of silence afterwards --
    // which is what makes several slots aiming at one output channel SUM
    // instead of overwrite each other.
    if (outputChannelData != nullptr)
    {
        for (int ch = 0; ch < numOutputChannels; ++ch)
        {
            if (outputChannelData[ch] != nullptr)
                juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);
        }
    }

    if (bypass)
    {
        // Bypass copies IN->OUT along the mapping (the detector still sees
        // signal via the per-slot taps below).
        for (int i = 0; i < numLanes; ++i)
            juce::FloatVectorOperations::copy (lanes[i].out, lanes[i].in, numSamples);
    }
    else
    {
        // Sample-by-sample processing through the per-lane notch chain,
        // ACCUMULATED onto the cleared output: several slots routed at one
        // output channel is valid and must SUM. NotchChain::processSample()
        // never allocates and never locks.
        for (int i = 0; i < numLanes; ++i)
        {
            NotchChain&   chain = *lanes[i].chain;
            const float*  in    = lanes[i].in;
            float*        out   = lanes[i].out;

            for (int n = 0; n < numSamples; ++n)
                out[n] += static_cast<float> (
                    chain.processSample (static_cast<double> (in[n])));
        }
    }

    // Tap EVERY enabled slot's post-DSP lane-0 output -- the signal actually
    // leaving that slot -- in every mode, including Bypass (the detector must
    // still see the signal when bypassed). Written straight from the
    // already-processed output buffer in ONE bulk write() per callback: no
    // heap buffer, no per-sample writes. The detector may be slow or absent:
    // a short write (or 0) is expected and silently tolerated -- never block,
    // never spin, never log. A slot with no valid lane-0 input/output pair is
    // not tapped at all.
    for (int slot = 0; slot < kMaxSlots; ++slot)
    {
        if (tapSource[slot] == nullptr)
            continue;

        const size_t requested = static_cast<size_t> (numSamples);
        const size_t written   =
            tapBuffers_[(std::size_t) slot].write (tapSource[slot], requested);

        // Dropping is the right BEHAVIOUR here -- blocking or spinning on the
        // audio thread is not an option -- but discarding the FACT is not. A
        // truncated write leaves no gap for the detector to notice; it leaves
        // a SPLICE, sample N followed immediately by sample N+k. Through a
        // 1024-point Hann window that step is broadband energy in every bin,
        // which is exactly the shape the peakiness scorer is built to react
        // to, and a notch would get placed on a frequency that never fed back.
        //
        // One relaxed read-modify-write: lock-free, allocation-free, and no
        // ordering relationship with anything else, since the count is only
        // ever read for display.
        if (written < requested)
            tapDropCounts_[(std::size_t) slot].fetch_add (requested - written,
                                                          std::memory_order_relaxed);
    }
}

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    if (device != nullptr)
    {
        const double sampleRate = device->getCurrentSampleRate();
        currentSampleRate_.store (sampleRate, std::memory_order_release);
        currentBufferSize_.store (device->getCurrentBufferSizeSamples(), std::memory_order_release);

        // Retarget ALL notch chains (8 slots x 2 lanes) to the device's actual
        // rate. JUCE calls audioDeviceAboutToStart BEFORE inserting the
        // callback into its dispatch list, so this runs before the audio
        // thread can touch the chains -- no synchronization needed.
        // setSampleRate() never allocates: it only rewrites pre-allocated
        // coefficient slots and clears filter state. A non-positive rate
        // (should not happen on a running device) is safely ignored inside
        // setSampleRate().
        for (auto& slotChains : notchChains_)
            for (auto& chain : slotChains)
                chain.setSampleRate (sampleRate);
    }

    // Clear any filter state left over from a previous device session so the
    // DSP starts from a clean slate -- every chain in the grid.
    for (auto& slotChains : notchChains_)
        for (auto& chain : slotChains)
            chain.reset();

    // Drain EVERY tap ring. Whatever is still in a ring was captured by the
    // PREVIOUS device session, possibly at a different sample rate. Leaving it
    // there splices old audio onto the front of the first analysis windows of
    // the new session, so the detector's first spectra would be labelled with
    // the new rate while half their content came from the old one -- every
    // bin-to-Hz conversion wrong by up to an octave, and the first notches
    // after a rate change placed on frequencies that never rang.
    //
    // clear() requires that neither the producer nor the consumer is running.
    // This is the one place in the codebase where that holds: JUCE inserts the
    // callback into its dispatch list only after audioDeviceAboutToStart()
    // returns, so the audio thread cannot be inside the callback yet.
    //
    // Detector::reset() is the consumer-side counterpart. Calling it belongs
    // to Task 14, which owns the detector instance; the method exists and is
    // tested here so that wiring is a one-liner.
    for (auto& tap : tapBuffers_)
        tap.clear();

    // All 16 rings (8 taps + 8 command queues) are cleared under the same
    // precondition (no producer/consumer running); the detector side is
    // stopped by MainComponent BEFORE any device restart reaches this point
    // (bridge design §6.5).
    for (auto& queue : commandQueues_)
        queue.clear();
}

void AudioEngine::audioDeviceStopped()
{
    // Nothing to release here yet: the notch chains are reset in
    // audioDeviceAboutToStart() on the next start. The override is kept so
    // the device lifecycle is explicit.
}

void AudioEngine::audioDeviceError (const juce::String& errorMessage)
{
    // The device is no longer usable; reflect that on the UI side. The store
    // is atomic so the UI thread observes it without a lock. Error paths are
    // exceptional, so the log call here is acceptable off the hot path.
    isRunning_.store (false, std::memory_order_release);

    // RETAIN the message, do not just log it. Without this the status
    // indicator goes dark and the reason exists only in the JUCE log, which no
    // soundman is reading mid-show. A mutex is fine here: this runs on the
    // device thread, never the audio callback, and the function already logs.
    {
        const std::lock_guard<std::mutex> lock (lastDeviceErrorLock_);
        lastDeviceError_ = errorMessage;
    }

    juce::Logger::writeToLog ("AudioEngine device error: " + errorMessage);
}