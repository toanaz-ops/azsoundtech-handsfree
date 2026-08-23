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
//     allocation and takes NO locks. Every piece of state the AUDIO THREAD
//     touches (mode, running flag, sample rate, buffer size, channel counts,
//     tap drop count) is exchanged via std::atomic, so it never blocks on a
//     mutex.
//
//     One member is mutex-guarded rather than atomic: lastDeviceError_, a
//     juce::String. juce::String is reference-counted and cannot be exchanged
//     atomically. That is affordable precisely because the audio thread never
//     touches it -- it is written by audioDeviceError() and start() on the
//     device thread and read by the GUI on the message thread. Do not reach
//     for it from the callback.
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
#include "dsp/NotchCommand.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

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

    //==========================================================================
    // Device enumeration, configuration and status (GUI interface audit, 3).
    //
    // ALL of these are safe to call with no device open, which is the state the
    // GUI is in while it populates its combo boxes at startup.
    // juce::AudioDeviceManager returns nullptr from getCurrentAudioDevice()
    // until start() succeeds, so every one of these guards it.
    //
    // Thread: message thread. They are not lock-free and must not be called
    // from the audio callback.
    //
    // Non-const where JUCE forces it: getAvailableDeviceTypes() and
    // AudioIODevice::getAvailableSampleRates() are non-const in JUCE, and
    // enumerating device names additionally requires scanForDevices() first,
    // which mutates the type object.

    // Registered driver types, e.g. "Windows Audio", "DirectSound", "ASIO".
    // ASIO appears only if the SDK was present at build time.
    juce::StringArray getAvailableDeviceTypeNames();

    // Device names for the CURRENT type. Empty when no type is selected.
    juce::StringArray getAvailableDeviceNames();

    // The type actually in use, which is NOT necessarily desiredDeviceType_:
    // start() asks JUCE for the desired type, and JUCE silently keeps the
    // current one when the requested type is not registered -- exactly what
    // happens when ASIO is requested on a machine with no ASIO driver. A GUI
    // that echoed the desired value would show the user "ASIO" while WASAPI
    // was running.
    juce::String getCurrentDeviceType() const;

    // What the OPEN device supports. Both empty when no device is open.
    juce::Array<double> getAvailableSampleRates();
    juce::Array<int>    getAvailableBufferSizes();

    // Return false when the change did not happen -- no device open, a value
    // the hardware refuses, or a non-positive argument. A void setter would
    // leave the GUI unable to tell a successful change from a silent no-op,
    // and it would then display a value the hardware is not running.
    // On success the device restarts, so audioDeviceAboutToStart() re-reads
    // the rate and retargets both notch chains.
    bool setSampleRate(double newRate);
    bool setBufferSize(int newSize);

    // Numeric companion to getCurrentSampleRate(), which returns the DISPLAY
    // string "48000 Hz". Task 17's ComboBox needs to select a matching item and
    // Task 25's preset JSON stores the value numerically; neither can use the
    // formatted form without parsing our own formatting back out.
    double getCurrentSampleRateHz() const;

    // Channel counts as the LAST CALLBACK actually received them -- not what a
    // device advertises, which can differ. Both 0 until audio has flowed, which
    // is honest: "2 in / 2 out" before anything has been delivered is a claim
    // the engine cannot support.
    int getNumInputChannels() const;
    int getNumOutputChannels() const;

    // The most recent audioDeviceError() message, or empty when healthy.
    // Previously the message was logged and discarded, so a device failure
    // darkened the status indicator with the reason available nowhere in the
    // UI. Cleared by a successful start().
    juce::String getLastDeviceError() const;

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

    // Command channel FROM the detector thread INTO the audio thread
    // (bridge design §2): the audio callback drains up to 64 commands per
    // callback and applies them to the notch chains.
    //
    // *** CALLER CONTRACT: write() only, and from exactly one thread. ***
    // Same reasoning as getTapBuffer(): the type system cannot stop a caller
    // from breaking the single-producer invariant -- the detector thread is
    // and must remain the sole producer (owner decision D-05).
    LockFreeRingBuffer<NotchCommand>& getCommandQueue();

    // TEST ACCESSOR ONLY -- lets tests assert chain state without an audio
    // device. Do not build product behaviour on this.
    const NotchChain& getNotchChainForTest (int channel) const;

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

    // Detector -> audio command ring (bridge design §2). Capacity 128 gives
    // 4x headroom over the worst legitimate burst: a preset load installs up
    // to 16 notches on each of 2 channels = 32 commands at once.
    static constexpr size_t kCommandCapacity = 128;
    LockFreeRingBuffer<NotchCommand> commandQueue_ { kCommandCapacity };

    // Bound on worst-case callback time: 64 recomputes ~13 us against a
    // 0.67 ms budget at a 32-sample buffer / 48 kHz (<2%). Chosen over 32 so
    // a full 32-command preset applies within ONE callback -- a preset that
    // half-applies across two callbacks is audible as two distinct changes.
    static constexpr int kMaxCommandsPerCallback = 64;

    // Audio-thread only: drains up to kMaxCommandsPerCallback commands from
    // commandQueue_ into the notch chains. Called first in the callback.
    void drainCommandQueue();

    // Cross-thread state. std::atomic keeps the audio callback lock-free and
    // allocation-free while still letting the UI thread observe/change mode,
    // the running flag and the device parameters.
    std::atomic<Mode>   currentMode_      { Mode::Bypass };
    std::atomic<double> currentSampleRate_ { 48000.0 };
    std::atomic<int>    currentBufferSize_ { 64 };
    std::atomic<bool>   isRunning_        { false };

    // Channel counts observed by the last callback. Relaxed atomics: two
    // stores per callback, no ordering relationship with anything else, and
    // they are only ever read for display.
    std::atomic<int> numInputChannels_  { 0 };
    std::atomic<int> numOutputChannels_ { 0 };

    // Last device error. A juce::String is reference-counted and NOT safe to
    // write on one thread while another reads it, so it takes a mutex rather
    // than an atomic. That is affordable here precisely because neither side
    // is the audio thread: audioDeviceError() is called from the device thread
    // and already logs, and the reader is the message thread. The audio
    // callback never touches this.
    mutable std::mutex lastDeviceErrorLock_;
    juce::String       lastDeviceError_;

    // Preferences applied on the next start(). Default type is ASIO; if the
    // driver is not registered, JUCE silently keeps the current type.
    juce::String desiredDeviceType_ { "ASIO" };
    juce::String desiredDeviceName_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};