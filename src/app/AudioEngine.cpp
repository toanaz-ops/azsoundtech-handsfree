#include "app/AudioEngine.h"

namespace
{
constexpr int kNumChannels = 2;  // stereo in/out
}

AudioEngine::AudioEngine()
    : notchChains_ { { NotchChain (48000.0), NotchChain (48000.0) } }
{
    // The chains are pre-built at the nominal 48 kHz rate.
    // audioDeviceAboutToStart() retargets each chain to the device's actual
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
        juce::Logger::writeToLog ("AudioEngine: failed to open audio device: " + error);
        return;
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
    return tapBuffer_;
}

std::uint64_t AudioEngine::getTapDropCount() const
{
    return tapDropCount_.load (std::memory_order_relaxed);
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

    // Real-time thread: no allocation, no locking. The only shared state read
    // here is the mode, via a lock-free atomic.
    const bool bypass = (currentMode_.load (std::memory_order_acquire) == Mode::Bypass);

    // JUCE requires every output channel to be written or cleared, and the
    // output buffers are NOT pre-cleared for us.
    for (int ch = 0; ch < numOutputChannels; ++ch)
    {
        float* output = outputChannelData[ch];

        if (output == nullptr)
            continue;

        const float* input = (ch < numInputChannels) ? inputChannelData[ch] : nullptr;

        // Bypass, missing input, or a channel beyond the stereo DSP pair:
        // pass the input straight through. A null input leaves the output
        // cleared (silence).
        if (bypass || input == nullptr || ch >= 2)
        {
            if (input != nullptr)
                juce::FloatVectorOperations::copy (output, input, numSamples);
            else
                juce::FloatVectorOperations::clear (output, numSamples);

            continue;
        }

        // Sample-by-sample processing through the per-channel notch chain.
        // NotchChain::processSample() never allocates and never locks.
        NotchChain& chain = notchChains_[static_cast<size_t> (ch)];

        for (int n = 0; n < numSamples; ++n)
            output[n] = static_cast<float> (chain.processSample (static_cast<double> (input[n])));
    }

    // Tap the LEFT channel POST-notch -- the signal actually leaving the app
    // -- in every mode, including Bypass (the detector must still see the
    // signal when bypassed). Written straight from the already-processed
    // output buffer in ONE bulk write() per callback: no heap buffer, no
    // per-sample writes. The detector may be slow or absent: a short write
    // (or 0) is expected and silently tolerated -- never block, never spin,
    // never log. If there is no input channel 0, write nothing.
    if (inputChannelData != nullptr && numInputChannels > 0
        && inputChannelData[0] != nullptr
        && outputChannelData != nullptr && outputChannelData[0] != nullptr)
    {
        const size_t requested = static_cast<size_t> (numSamples);
        const size_t written   = tapBuffer_.write (outputChannelData[0], requested);

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
            tapDropCount_.fetch_add (requested - written, std::memory_order_relaxed);
    }
}

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    if (device != nullptr)
    {
        const double sampleRate = device->getCurrentSampleRate();
        currentSampleRate_.store (sampleRate, std::memory_order_release);
        currentBufferSize_.store (device->getCurrentBufferSizeSamples(), std::memory_order_release);

        // Retarget BOTH notch chains to the device's actual rate. JUCE calls
        // audioDeviceAboutToStart BEFORE inserting the callback into its
        // dispatch list, so this runs before the audio thread can touch the
        // chains -- no synchronization needed. setSampleRate() never
        // allocates: it only rewrites pre-allocated coefficient slots and
        // clears filter state. A non-positive rate (should not happen on a
        // running device) is safely ignored inside setSampleRate().
        for (auto& chain : notchChains_)
            chain.setSampleRate (sampleRate);
    }

    // Clear any filter state left over from a previous device session so the
    // DSP starts from a clean slate.
    for (auto& chain : notchChains_)
        chain.reset();

    // Drain the tap. Whatever is still in the ring was captured by the
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
    tapBuffer_.clear();
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
    juce::Logger::writeToLog ("AudioEngine device error: " + errorMessage);
}