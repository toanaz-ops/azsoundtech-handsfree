#include "app/AudioEngine.h"

// Lane M. Included from the .cpp ONLY, never from AudioEngine.h (m-20):
// the header needs nothing from it, and keeping it out stops a DSP header
// from riding into every TU that already pulls juce_audio_devices.
#include "dsp/SoundcheckSignal.h"

#include <cmath>

namespace
{
constexpr int kNumChannels = 2;  // stereo in/out

// Hard ceiling on every sample handed to the driver. Values beyond full scale
// would be clipped by the DAC conversion anyway; the clamp only makes that
// bound explicit and bounds what multi-slot summing can stack onto one output
// channel. Non-finite values must never reach the driver at all -- their
// conversion is undefined -- so the final pass maps them to 0 rather than
// "clamping" them (any comparison with NaN picks an arbitrary bound).
constexpr float kMaxOutputLevel = 1.0f;
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

namespace
{
// Clamped, never OOB: tests and future callers get slot 0 / lane 0 for bad
// input, matching the getNotchChainForTest convention.
int clampSlot (int slot) { return (slot < 0 || slot >= kMaxSlots) ? 0 : slot; }
int clampLane (int lane) { return (lane < 0 || lane >= kMaxSlotLanes) ? 0 : lane; }
}

LockFreeRingBuffer<float>& AudioEngine::getTapBuffer()
{
    return getTapBuffer (0, 0);
}

LockFreeRingBuffer<float>& AudioEngine::getTapBuffer (int slot)
{
    return getTapBuffer (slot, 0);
}

LockFreeRingBuffer<float>& AudioEngine::getTapBuffer (int slot, int lane)
{
    return tapBuffers_[(std::size_t) clampSlot (slot)][(std::size_t) clampLane (lane)];
}

std::uint64_t AudioEngine::getTapDropCount() const
{
    return getTapDropCount (0, 0);
}

std::uint64_t AudioEngine::getTapDropCount (int slot) const
{
    return getTapDropCount (slot, 0);
}

std::uint64_t AudioEngine::getTapDropCount (int slot, int lane) const
{
    return tapDropCounts_[(std::size_t) clampSlot (slot)][(std::size_t) clampLane (lane)]
        .load (std::memory_order_relaxed);
}


// ---- Lane M: active soundcheck (spec 2026-09-15 §4.1) ------------------
//
// Every setter here is a single relaxed atomic store on the message thread.
// None of them restarts the device, allocates, or takes a lock; the audio
// callback reads all of them exactly once per block.

void AudioEngine::setSoundcheckOutputChannel (int channel)
{
    // Not range-checked here on purpose: the channel count can change between
    // this store and the callback that reads it, so the ONLY check that means
    // anything is the one the callback makes against its OWN channel count
    // (invariant 3).
    //
    // A pending ramp-out is discarded, whatever the new value (C-2). Arming a
    // channel while an anchor from the previous channel is still set would
    // truncate the new run mid-sweep; releasing one while a request is pending
    // leaves a request that only an emitting callback could ever clear, and
    // after an abort there is no emitting callback.
    //
    // ORDERING (N-2). Three relaxed stores to three distinct atomics carry no
    // ordering between them in the C++ model -- "cleared first" is a statement
    // about source order, not about what the audio thread can observe. The
    // channel store is therefore a RELEASE and the callback's matching load is
    // an ACQUIRE: a callback that sees this channel is then guaranteed to see
    // the two clears above it, which is the property the comment used to claim
    // and not have. The clears themselves stay relaxed -- the release orders
    // them.
    scRampOutAtSample_.store  (-1,    std::memory_order_relaxed);
    scRampOutRequested_.store (false, std::memory_order_relaxed);
    scOutChannel_.store (channel, std::memory_order_release);
}

void AudioEngine::setSoundcheckCaptureChannel (int channel)
{
    scCaptureInChannel_.store (channel, std::memory_order_relaxed);
}

void AudioEngine::setSoundcheckCaptureActive (bool active)
{
    scCaptureActive_.store (active, std::memory_order_relaxed);
}

void AudioEngine::setSoundcheckTapsSuspended (bool suspended)
{
    scSuspendTaps_.store (suspended, std::memory_order_relaxed);
}

void AudioEngine::setSoundcheckSampleIndex (std::int64_t n)
{
    scSampleIndex_.store (n, std::memory_order_relaxed);
}

void AudioEngine::setSoundcheckPeak (float peak)
{
    // Clamped HERE and again inside SoundcheckSignal::sampleAt. Two clamps for
    // one value is deliberate: this number has two routes in (this setter and
    // the test seam below), and a value with two routes to becoming wrong
    // needs both clamped -- one clamp is half a clamp (lane G M-B).
    scPeak_.store (SoundcheckSignal::clampPeak (peak), std::memory_order_relaxed);
}

void AudioEngine::requestSoundcheckRampOut()
{
    // A FLAG, not an anchor (I-2). The anchor is a sample index, so latching
    // it here means latching whatever index the message thread happens to
    // read -- and the audio thread may already have advanced past it, by up to
    // one full buffer. The first ramped block would then evaluate the envelope
    // at 0.5*(1 + cos(pi*N/R)) instead of 1.0, and for any buffer N >= R
    // (1440 samples at 48 kHz, i.e. every buffer a 192 kHz device is likely to
    // hand us) SoundcheckSignal::rampOut returns EXACTLY 0: the sweep stops
    // dead in one sample. A hard cut on a PA is the click this ramp exists to
    // prevent.
    //
    // The callback latches the anchor from the same snapshot it indexes the
    // sweep with, so the envelope starts at exactly 1.0 at any buffer size.
    scRampOutRequested_.store (true, std::memory_order_relaxed);
}

int AudioEngine::getSoundcheckOutputChannel() const
{
    return scOutChannel_.load (std::memory_order_relaxed);
}

std::int64_t AudioEngine::getSoundcheckSampleIndex() const
{
    return scSampleIndex_.load (std::memory_order_relaxed);
}

bool AudioEngine::soundcheckIsEmitting() const
{
    return scOutChannel_.load (std::memory_order_relaxed) >= 0;
}

LockFreeRingBuffer<float>& AudioEngine::getMicCaptureBuffer()
{
    return micCapture_;
}

std::uint64_t AudioEngine::getMicCaptureDropCount() const
{
    return micCaptureDrops_.load (std::memory_order_relaxed);
}

void AudioEngine::setRunningForTest (bool running)
{
    // TEST SEAM ONLY (B-4). isRunning_ is otherwise written only by start()
    // and audioDeviceError(); audioDeviceAboutToStart() does not touch it, so
    // a headless test can never reach a state SoundcheckController::preflight
    // accepts. It stores the same atomic start() stores.
    isRunning_.store (running, std::memory_order_release);
}

void AudioEngine::setSoundcheckGainUnclampedForTest (float gain)
{
    // TEST SEAM ONLY (B-5, F17). Applied at insertion point 3 to the sample
    // AFTER SoundcheckSignal has clamped it, because a seam on the PEAK
    // achieves nothing -- the signal constructor and sampleAt both clamp, so
    // the amplitude is already bounded twice before the +-1.0f output clamp
    // and invariant 4 could not be turned red by any test. Default 1.0f in
    // every shipping path.
    scGainUnclampedForTest_.store (gain, std::memory_order_relaxed);
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

    // Lane M (spec §4.1, invariant 7): TEN values are read HERE, once -- the
    // eight soundcheck atomics, the sample rate (I-9) and the test-only gain
    // seam (B-5) -- for the same reason the mode and the mapping are: a flip
    // mid-callback between the mute decision (point 2) and the tap decision
    // (point 4) would produce a block that BOTH injects the sweep and taps it
    // back into the detector.
    //
    // Nothing below this point reads ANY of the ten again. That includes
    // currentSampleRate_: a rate change landing between two reads would build
    // the sweep with one T and index it with another.
    // ACQUIRE (N-2), matching the release stores in setSoundcheckOutputChannel
    // and audioDeviceAboutToStart: seeing a channel value means also seeing the
    // ramp-out state that was cleared before it was published.
    const int          scOutChannel    = scOutChannel_.load       (std::memory_order_acquire);
    const bool         scSuspendTaps   = scSuspendTaps_.load      (std::memory_order_relaxed);
    const int          scCaptureIn     = scCaptureInChannel_.load (std::memory_order_relaxed);
    const bool         scCaptureActive = scCaptureActive_.load    (std::memory_order_relaxed);
    const std::int64_t scSampleIndex   = scSampleIndex_.load      (std::memory_order_relaxed);
    const float        scPeak          = scPeak_.load             (std::memory_order_relaxed);
    const std::int64_t scRampOutAt     = scRampOutAtSample_.load  (std::memory_order_relaxed);
    const bool         scRampOutReq    = scRampOutRequested_.load (std::memory_order_relaxed);
    // ACQUIRE, matching the release store in audioDeviceAboutToStart() and
    // every other reader of this value (M-5). Relaxed would be enough for the
    // arithmetic -- a double is a double -- but the rate is published together
    // with the retuned notch chains, and reading it with weaker ordering than
    // the code that publishes it is the kind of asymmetry nobody re-derives
    // correctly two years later. It costs nothing on x86.
    const double       scSampleRate    = currentSampleRate_.load  (std::memory_order_acquire);
    // B-5, TEST SEAM. 1.0f in every shipping path.
    const float        scGainUnclamped = scGainUnclampedForTest_.load (std::memory_order_relaxed);

    // Invariant 3: re-checked against THIS callback's counts, exactly as the
    // lane loop re-checks every channel index below. A device restart onto
    // fewer channels without this is an out-of-bounds write on the audio
    // thread.
    const int scOut  = (scOutChannel >= 0 && scOutChannel < numOutputChannels
                        && outputChannelData != nullptr) ? scOutChannel : -1;
    const int scInCh = (scCaptureIn  >= 0 && scCaptureIn  < numInputChannels
                        && inputChannelData  != nullptr) ? scCaptureIn  : -1;

    // Lane M point 1 (Q13, B-6): the RAW mic, before any DSP, taken straight
    // off the callback's input pointers.
    //
    // This must NOT live inside the lane loop. A measurement mic is normally
    // not in the routing table at all -- that is what "raw mic" means -- so a
    // capture that only fired when an ENABLED, correctly-routed lane happened
    // to read from scCaptureInChannel_ would capture NOTHING in the common
    // case, and the lane M thread would report every channel as "could not
    // measure" with no clue why. scInCh is already bounds-checked against THIS
    // callback's numInputChannels (invariant 3).
    const float* capSource = (scInCh >= 0) ? inputChannelData[scInCh] : nullptr;

    struct LaneRef { const float* in; float* out; NotchChain* chain; };

    // Stack-resident lane table -- no heap, no lock. Upper bound: every slot
    // stereo = 8 x 2 lanes.
    LaneRef lanes[kMaxSlots * kMaxSlotLanes];
    int numLanes = 0;

    // Per-slot, per-lane tap source: slot s, lane l taps its output post-DSP
    // when that lane exists and is valid. Null means "do not tap this lane" --
    // disabled slot, invalid width, or no valid channel pair for that lane.
    const float* tapSource[kMaxSlots][kMaxSlotLanes] {};

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

            // Lane M point 2 (Q15 relitigated, F2): mute EVERY lane routed to
            // the channel being measured -- not one (slot, lane) pair. Several
            // slots sum onto one output channel (the clear below, then the DSP
            // accumulating), so muting a pair would leave that channel's
            // feedback loop CLOSED. A muted lane enters neither lanes[] nor
            // tapSource, so it is also not fed back to the detector.
            //
            // The chain's STATE is cleared on every muted block (C-3). Muting
            // by skipping the lane freezes the biquads' persistent Direct Form
            // I state for as long as the mute lasts -- up to 4.5 s per channel
            // -- and on un-mute that stale state discharges as a free response
            // on top of live programme: for a high-Q notch roughly 5-6x the
            // level at the moment of the mute, i.e. an audible click on every
            // channel the soundcheck touches.
            //
            // clearState(), NOT reset() (N-1). reset() also zeroes
            // rampRemaining_, so across a mute that can last 4.5 s it would
            // cancel an in-flight lane-G depth ramp on EVERY block -- and
            // command draining is not suspended during a run, so a retune
            // landing mid-mute is entirely normal. The filter would strand at
            // the intermediate depth while NotchInfo.depthDB already read the
            // target: the rare NaN-heal GAP turned into routine behaviour. A
            // mute is a discontinuity in this lane's INPUT, which says nothing
            // about a ramp that is still the right thing to finish.
            // clearState() is equally allocation-free and lock-free.
            //
            // EXPECTED LEVEL CHANGE: mute onset = declared silence (spec §3);
            // un-mute = programme resumes with the notch chains at zero state,
            // so no free response from stale state; NO depth ramp is stranded
            // -- an in-flight lane-G ramp is frozen for the duration of the
            // mute (the lane is not processed at all) and completes normally
            // once the lane is un-muted.
            if (scOut >= 0 && outIdx == scOut)
            {
                notchChains_[(std::size_t) slot][(std::size_t) lane].clearState();
                continue;
            }

            const float* in = (inputChannelData != nullptr)
                                  ? inputChannelData[inIdx] : nullptr;
            float* out      = (outputChannelData != nullptr)
                                  ? outputChannelData[outIdx] : nullptr;

            if (in == nullptr || out == nullptr)
                continue;

            lanes[numLanes++] = { in, out,
                                  &notchChains_[(std::size_t) slot][(std::size_t) lane] };

            tapSource[slot][lane] = out;
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
            {
                // A non-finite INPUT sample (driver glitch, hot-plug spike) is
                // treated as 0 rather than fed onward: the biquads keep
                // persistent Direct Form I state, and ONE NaN/Inf through them
                // poisons that state until reset() -- which otherwise happens
                // only on device restart.
                const double x = std::isfinite (in[n])
                                     ? static_cast<double> (in[n]) : 0.0;

                double v = chain.processSample (x);

                // A non-finite chain OUTPUT means the filter state itself is
                // already poisoned. Reset the chain -- allocation-free, the
                // same call the device-restart path makes -- and emit silence
                // for this sample: the filter self-heals within one sample
                // instead of screaming NaN until someone restarts the device.
                if (! std::isfinite (v))
                {
                    chain.reset();
                    v = 0.0;
                }

                out[n] += static_cast<float> (v);
            }
        }
    }


    // Lane M point 3 (spec §4.1). MUST stay ABOVE the +-kMaxOutputLevel clamp
    // below: past it, the sweep would reach the driver unclamped
    // (invariant 4). Writes exactly ONE channel (invariant 5), accumulating
    // like the DSP above. No lock, no allocation, no logging (invariant 16):
    // SoundcheckSignal holds nothing but doubles and lives on the stack.
    if (scOut >= 0)
    {
        SoundcheckSignal::Params params;
        params.sampleRate = scSampleRate;           // I-9: from the snapshot, not re-read
        params.peak       = scPeak;
        const SoundcheckSignal signal { params };

        const std::int64_t rampLen = SoundcheckSignal::rampOutSamples (scSampleRate);

        // I-2: the anchor is latched HERE, on the audio thread, out of the SAME
        // snapshot the sweep is indexed with -- so the first ramped sample is
        // evaluated at rampOut(anchor, anchor, R) == exactly 1.0, whatever the
        // buffer size. The message thread only ever sets the request flag; if
        // it set the anchor, the anchor could be up to one buffer behind this
        // block and the envelope would open at 0.5*(1 + cos(pi*N/R)) -- exactly
        // 0, a hard cut, for any buffer >= R.
        std::int64_t rampAt = scRampOutAt;
        if (rampAt < 0 && scRampOutReq)
        {
            rampAt = scSampleIndex;
            scRampOutAtSample_.store (rampAt, std::memory_order_relaxed);
        }

        float* out = outputChannelData[scOut];

        if (out != nullptr)
        {
            for (int n = 0; n < numSamples; ++n)
            {
                const std::int64_t idx = scSampleIndex + n;
                float v = signal.sampleAt (idx);            // 0 while idx < 0 (NoiseFloor)
                if (rampAt >= 0)
                    v *= SoundcheckSignal::rampOut (idx, rampAt, rampLen);
                // B-5: 1.0f in every shipping path. The ONLY route past the
                // signal own clamps, and it exists so the +-1.0f output clamp
                // below can be SHOWN to still cover this path.
                out[n] += v * scGainUnclamped;
            }
        }

        scSampleIndex_.store (scSampleIndex + numSamples, std::memory_order_relaxed);

        // The callback ENDS the run itself once the ramp-out has reached zero
        // (F8, invariant 9): no other thread needs to still be alive for the
        // sound to stop.
        if (rampAt >= 0 && scSampleIndex + numSamples >= rampAt + rampLen)
        {
            scOutChannel_.store       (-1,    std::memory_order_relaxed);
            scRampOutAtSample_.store  (-1,    std::memory_order_relaxed);
            scRampOutRequested_.store (false, std::memory_order_relaxed);
        }
    }

    // Final output guard: every sample actually handed to the driver is forced
    // finite and clamped to +-kMaxOutputLevel (rationale at the constant).
    // This is the one pass that also covers Bypass copies and multi-slot sums,
    // and it runs BEFORE the tap writes below so the detector sees exactly
    // what left the app.
    if (outputChannelData != nullptr)
    {
        for (int ch = 0; ch < numOutputChannels; ++ch)
        {
            float* out = outputChannelData[ch];
            if (out == nullptr)
                continue;

            for (int n = 0; n < numSamples; ++n)
            {
                const float v = out[n];
                out[n] = std::isfinite (v)
                             ? juce::jlimit (-kMaxOutputLevel, kMaxOutputLevel, v)
                             : 0.0f;
            }
        }
    }

    // Tap EVERY enabled slot's post-DSP output on EVERY lane -- the signal
    // actually leaving that lane -- in every mode, including Bypass (the
    // detector must still see the signal when bypassed). Written straight
    // from the already-processed output buffer in ONE bulk write() per ring
    // per callback: no heap buffer, no per-sample writes. The detector may be
    // slow or absent: a short write (or 0) is expected and silently tolerated
    // -- never block, never spin, never log. A lane with no valid
    // input/output pair is not tapped at all.
    //
    // Lane M point 4 (spec §4.1, invariant 10). Keyed on scSuspendTaps_ and
    // NOT on scOutChannel_: that index returns to -1 at every Gap, so keying
    // on it would un-suspend the taps for 300 ms between channels and restart
    // lane G's ~420 ms tapAlive window PER CHANNEL -- ~11.5 s over 16
    // channels, past kReleaseStepMs = 10 s (N3). Held for the whole run, the
    // true figure is ~0.42 s, once.
    //
    // This is a SKIP, not a drop: tapDropCounts_ must not move, or every
    // reader of the session log sees a burst of false positives that never
    // happened. The raw mic goes to its own ring instead.
    if (scSuspendTaps)
    {
        if (scCaptureActive && capSource != nullptr)
        {
            const std::size_t requested = (std::size_t) numSamples;
            const std::size_t written   = micCapture_.write (capSource, requested);
            if (written < requested)
                micCaptureDrops_.fetch_add (requested - written, std::memory_order_relaxed);
        }
    }
    else
    for (int slot = 0; slot < kMaxSlots; ++slot)
        for (int lane = 0; lane < kMaxSlotLanes; ++lane)
        {
            const float* src = tapSource[slot][lane];
            if (src == nullptr)
                continue;

            const size_t requested = static_cast<size_t> (numSamples);
            const size_t written   =
                tapBuffers_[(std::size_t) slot][(std::size_t) lane].write (src, requested);

            // Dropping is the right BEHAVIOUR here -- blocking or spinning on
            // the audio thread is not an option -- but discarding the FACT is
            // not. A truncated write leaves no gap for the detector to
            // notice; it leaves a SPLICE, sample N followed immediately by
            // sample N+k. Through a 1024-point Hann window that step is
            // broadband energy in every bin, which is exactly the shape the
            // peakiness scorer is built to react to, and a notch would get
            // placed on a frequency that never fed back.
            //
            // One relaxed read-modify-write: lock-free, allocation-free, and
            // no ordering relationship with anything else, since the count is
            // only ever read for display.
            if (written < requested)
                tapDropCounts_[(std::size_t) slot][(std::size_t) lane]
                    .fetch_add (requested - written, std::memory_order_relaxed);
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
    for (auto& slotTaps : tapBuffers_)
        for (auto& tap : slotTaps)
            tap.clear();

    // Lane M: the same precondition and the same reason. Capture taken at the
    // PREVIOUS device's rate would be spliced onto the front of the next run's
    // first analysis windows, and every bin-to-Hz conversion would be wrong.
    // MainComponent aborts and joins the SoundcheckController before a restart
    // reaches here (spec §4.3), so neither producer nor consumer is running.
    micCapture_.clear();

    // C-1: and put the soundcheck itself back to idle. None of these eight
    // atomics belongs to a device, so nothing else clears them across a stop:
    // MainComponent aborts and JOINS the SoundcheckController before a restart
    // reaches here (spec §4.3), which means the one thread that would have
    // stood them down is already gone. A run interrupted by a device stop would
    // otherwise leave scOutChannel_ armed, and the next device to open -- a
    // different interface, a different channel map -- would have sweep emitted
    // on channel N and every lane routed there muted, with nobody left to stop
    // it but the ramp-out that was never requested.
    //
    // Same precondition as every clear above: JUCE inserts the callback into
    // its dispatch list only after this function returns.
    // The channel store is LAST and is a RELEASE (N-2), for the same reason as
    // in setSoundcheckOutputChannel: it is the one value the callback gates on,
    // so publishing it last under a release is what makes the other five
    // visible to any callback that sees it. The rest stay relaxed.
    scRampOutAtSample_.store  (-1,    std::memory_order_relaxed);
    scRampOutRequested_.store (false, std::memory_order_relaxed);
    scCaptureActive_.store    (false, std::memory_order_relaxed);
    scSuspendTaps_.store      (false, std::memory_order_relaxed);
    scCaptureInChannel_.store (-1,    std::memory_order_relaxed);
    scOutChannel_.store       (-1,    std::memory_order_release);
    // scSampleIndex_ and scPeak_ are deliberately NOT touched: neither can
    // produce a sample without scOutChannel_, and the controller sets both at
    // Arm. Clearing them would only add two stores nothing reads.

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