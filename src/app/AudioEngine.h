// AudioEngine: bridges JUCE audio I/O with the NotchChain DSP core.
//
// Owns a juce::AudioDeviceManager, registers itself as the sole
// juce::AudioIODeviceCallback, and routes every incoming audio block through
// the per-channel NotchChain cascade. In Bypass mode the DSP is skipped and
// the input is passed straight through.
//
// Threading model:
//   - start()/stop()/setMode()/configuration and query methods: UI/message
//     thread.
//   - audioDeviceIOCallback(): real-time audio thread. It performs NO heap
//     allocation and takes NO locks. All cross-thread state (mode, running
//     flag, sample rate, buffer size) is exchanged via std::atomic only, so
//     the audio thread never blocks on a mutex.
//
// The post-notch left-channel tap (Task 9) is written into tapBuffer_ -- an
// SPSC lock-free ring buffer -- once per callback, in every mode including
// Bypass. The audio thread is the sole producer; the detector thread (Task
// 10) is the sole consumer and reads 1024-sample blocks with a 512-sample
// hop from it.

#pragma once

// Module include rather than <JuceHeader.h>: JuceHeader.h is generated only
// for targets created with a juce_add_* function, so including it here made
// AudioEngine impossible to compile into the plain add_executable test target.
// juce_audio_devices pulls in juce_audio_basics, juce_events and juce_core.
#include <juce_audio_devices/juce_audio_devices.h>

#include "dsp/LockFreeRingBuffer.h"
#include "dsp/NotchChain.h"

#include <array>
#include <atomic>
#include <cstdint>

class AudioEngine : public juce::AudioIODeviceCallback
{
public:
    enum class Mode
    {
        Bypass,
        Auto,
        Soundcheck
    };

    AudioEngine();
    ~AudioEngine() override;

    // Device management
    void start();
    void stop();
    bool isRunning() const;

    // ASIO device control
    void setAudioDeviceType(const juce::String& typeName);
    void setAudioDevice(const juce::String& deviceName);
    juce::String getCurrentDeviceName() const;
    juce::String getCurrentSampleRate() const;
    int getCurrentBufferSize() const;
    double getCurrentLatency() const;

    // Mode control
    void setMode(Mode mode);
    Mode getMode() const;

    // Tap: post-notch LEFT channel, written once per callback into an SPSC
    // ring buffer. Read side belongs to the detector thread (Task 10).
    //
    // *** CALLER CONTRACT: read() only, and from exactly one thread. ***
    // This hands out a non-const reference, so nothing in the type system
    // stops a caller from calling write() and breaking the single-producer
    // invariant the whole design rests on -- the audio callback is and must
    // remain the sole producer. A consumer-only wrapper was considered and
    // rejected for now: Detector::processLatestBlock() takes
    // LockFreeRingBuffer<float>& directly, so introducing a view type would
    // mean changing the detector's signature too, which is more churn than
    // the risk currently justifies. If a second caller ever appears, add the
    // view then.
    LockFreeRingBuffer<float>& getTapBuffer();

    // Number of tap samples dropped because the ring was full, accumulated
    // over the lifetime of the engine. Dropping is the correct behaviour on
    // the audio thread -- blocking or spinning is not -- but a drop splices
    // sample N onto sample N+k, and through the detector's Hann window that
    // step is broadband energy in every bin. Without this count nothing
    // downstream can distinguish a drop-induced false peak from a real howl.
    // Incremented with memory_order_relaxed: one lock-free RMW per short
    // write, no allocation, no ordering dependency on anything else.
    std::uint64_t getTapDropCount() const;

    // AudioIODeviceCallback interface
    // (JUCE 9 replaced the legacy 5-arg audioDeviceIOCallback with
    // audioDeviceIOCallbackWithContext; see Task 8 report.)
    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                          int numInputChannels,
                                          float* const* outputChannelData,
                                          int numOutputChannels,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override;

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceError(const juce::String& errorMessage) override;

private:
    juce::AudioDeviceManager deviceManager_;
    std::array<NotchChain, 2> notchChains_;  // L=0, R=1

    // Post-notch left-channel tap (SPSC ring buffer, lock-free). The audio
    // callback is the single producer; the detector thread is the single
    // consumer. Pre-allocated at construction; write() never blocks and
    // silently drops whatever does not fit.
    static constexpr size_t kTapCapacity = 8192;  // ~170 ms @ 48 kHz, power of 2
    LockFreeRingBuffer<float> tapBuffer_ { kTapCapacity };
    std::atomic<std::uint64_t> tapDropCount_ { 0 };

    // Cross-thread state. std::atomic keeps the audio callback lock-free and
    // allocation-free while still letting the UI thread observe/change mode,
    // the running flag and the device parameters.
    std::atomic<Mode>   currentMode_      { Mode::Bypass };
    std::atomic<double> currentSampleRate_ { 48000.0 };
    std::atomic<int>    currentBufferSize_ { 64 };
    std::atomic<bool>   isRunning_        { false };

    // Preferences applied on the next start(). Default type is ASIO; if the
    // driver is not registered, JUCE silently keeps the current type.
    juce::String desiredDeviceType_ { "ASIO" };
    juce::String desiredDeviceName_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};