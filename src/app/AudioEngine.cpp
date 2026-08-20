#include "app/AudioEngine.h"

namespace
{
constexpr int kNumChannels = 2;  // stereo in/out
}

AudioEngine::AudioEngine()
    : notchChains_ { { NotchChain (48000.0), NotchChain (48000.0) } }
{
    // The chains are pre-built at the nominal 48 kHz rate. NotchChain does
    // not yet expose a way to retarget its sample rate, so if the device
    // opens at a different rate the coefficients produced by a later
    // setNotch() call would be computed for 48 kHz. No notches are set in
    // this task; the fix belongs in NotchChain (see Task 8 report).
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

//==============================================================================
// AudioIODeviceCallback -- real-time audio thread.

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                                    int numInputChannels,
                                                    float* const* outputChannelData,
                                                    int numOutputChannels,
                                                    int numSamples,
                                                    const juce::AudioIODeviceCallbackContext&)
{
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
}

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    if (device != nullptr)
    {
        currentSampleRate_.store (device->getCurrentSampleRate(), std::memory_order_release);
        currentBufferSize_.store (device->getCurrentBufferSizeSamples(), std::memory_order_release);
    }

    // Clear any filter state left over from a previous device session so the
    // DSP starts from a clean slate.
    for (auto& chain : notchChains_)
        chain.reset();
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