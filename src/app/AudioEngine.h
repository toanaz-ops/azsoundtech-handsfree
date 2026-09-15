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
// The per-slot post-DSP lane-0 taps (multi-slot routing, spec §3) are written
// into one SPSC lock-free ring buffer PER SLOT -- once per callback, in every
// mode including Bypass. The audio thread is the sole producer of each ring;
// the detector side is the sole consumer and reads 2048-sample blocks with a
// 512-sample hop (Detector::kFftSize / kHopSize).

#pragma once

// Module include rather than <JuceHeader.h>: JuceHeader.h is generated only
// for targets created with a juce_add_* function, so including it here made
// AudioEngine impossible to compile into the plain add_executable test target.
// juce_audio_devices pulls in juce_audio_basics, juce_events and juce_core.
#include <juce_audio_devices/juce_audio_devices.h>

#include "app/SlotConfig.h"
#include "dsp/LockFreeRingBuffer.h"
#include "dsp/NotchChain.h"
#include "dsp/NotchCommand.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

inline const char* defaultDeviceTypeName()
{
#if JUCE_MAC
    return "CoreAudio";
#elif JUCE_WINDOWS
    return "ASIO";
#else
    return "";
#endif
}

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
    // Share of the audio callback's budget currently being used, 0..1.
    //
    // Straight from juce::AudioDeviceManager, which measures the callback
    // itself -- so it is the figure that matters (is the DSP about to
    // overrun?) rather than the process's share of the whole CPU. Safe from
    // the message thread; the manager keeps it as a plain double updated in
    // the callback.
    [[nodiscard]] double getCpuUsage() const;

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

    // Tap: post-notch output of every lane of a slot, written once per
    // callback into an SPSC ring buffer -- one ring PER SLOT PER LANE. Read
    // side belongs to the detector thread (Task 10).
    //
    // *** CALLER CONTRACT: read() only, and from exactly one thread per ring.
    // ***
    // This hands out a non-const reference, so nothing in the type system
    // stops a caller from calling write() and breaking the single-producer
    // invariant the whole design rests on -- the audio callback is and must
    // remain the sole producer of every slot's tap ring. A consumer-only
    // wrapper was considered and rejected for now: Detector::processLatestBlock()
    // takes LockFreeRingBuffer<float>& directly, so introducing a view type
    // would mean changing the detector's signature too, which is more churn
    // than the risk currently justifies. If a second caller ever appears, add
    // the view then.
    //
    // The no-argument overloads are the LEGACY surface (slot 0, lane 0):
    // everything that existed before multi-slot routing keeps working
    // unchanged. The (slot, lane) form reaches either ring of the 8x2 grid;
    // both slot and lane clamp into range for bad input.
    LockFreeRingBuffer<float>& getTapBuffer();
    LockFreeRingBuffer<float>& getTapBuffer (int slot);
    LockFreeRingBuffer<float>& getTapBuffer (int slot, int lane);

    // Number of tap samples dropped because a slot's ring was full, accumulated
    // over the lifetime of the engine -- tracked PER SLOT PER LANE; the
    // no-argument overload reads slot 0 lane 0. Dropping is the correct
    // behaviour on the audio thread -- blocking or spinning is not -- but a
    // drop splices sample N onto sample N+k, and through the detector's Hann
    // window that step is broadband energy in every bin. Without this count
    // nothing downstream can distinguish a drop-induced false peak from a real
    // howl. Incremented with memory_order_relaxed: one lock-free RMW per short
    // write, no allocation, no ordering dependency on anything else.
    std::uint64_t getTapDropCount() const;
    std::uint64_t getTapDropCount (int slot) const;
    std::uint64_t getTapDropCount (int slot, int lane) const;

    // ---- Lane M: active soundcheck (spec 2026-09-15 §4.1) --------------
    //
    // The engine's whole share of the soundcheck is these plain atomic stores
    // plus one lock-free capture ring. NONE of them restarts the device, and
    // the audio callback reads every one of them EXACTLY ONCE, at the top of
    // the block beside the mode snapshot (invariant 7): a flip landing between
    // the mute decision and the tap decision would give a callback that both
    // injects the sweep AND taps it back into the detector.
    //
    // Thread: message thread (SoundcheckController) for every setter below.

    // The output channel being measured, or -1 when no run is in flight. This
    // IS the mute key: EVERY lane routed to this channel is silenced, not one
    // (slot, lane) pair -- several slots sum onto one output channel, so
    // muting a pair would leave that channel's feedback loop closed. Returns
    // to -1 at every Gap, so it is NOT usable as the tap-suspension key: see
    // setSoundcheckTapsSuspended.
    //
    // ANY call also discards a pending ramp-out (C-2): arming a channel with a
    // stale anchor would truncate the new run mid-sweep, and releasing one with
    // a stale anchor leaves an anchor nothing will ever clear. This is also the
    // controller's abort backstop -- setSoundcheckOutputChannel(-1) puts the
    // soundcheck side of the engine back to idle in one call.
    void setSoundcheckOutputChannel (int channel);

    // Input channel the raw measurement mic arrives on, or -1. Independent of
    // the routing table: a measurement mic is normally not routed at all.
    void setSoundcheckCaptureChannel (int channel);

    // Capture gate, held across NoiseFloor + Sweep + Tail. A SEPARATE flag
    // from the channel index so a safety property is never inferred from the
    // sign of an index. Capture happens only while this is true AND the taps
    // are suspended -- the two live in the same branch of the callback, so an
    // active gate with the taps running captures nothing.
    void setSoundcheckCaptureActive (bool active);

    // Held for the WHOLE run (Arm to the end of the last channel's tail, or
    // Abort), across every Gap. Keying suspension on the output channel
    // instead would un-suspend the taps for 300 ms per channel and walk lane
    // G's release clock down a rung.
    void setSoundcheckTapsSuspended (bool suspended);

    // Sweep sample index. NEGATIVE during the noise-floor phase, which is how
    // "not emitting yet" is expressed without a second flag that could
    // disagree. The audio callback OWNS this value once a run is in flight --
    // it advances it by numSamples per block.
    void setSoundcheckSampleIndex (std::int64_t n);

    // Peak amplitude, CLAMPED to kSoundcheckMaxPeak here and again inside
    // SoundcheckSignal.
    void setSoundcheckPeak (float peak);

    // Request the emergency ramp-out. This sets a FLAG only; the audio
    // callback latches the anchor from its OWN snapshot of the sample index on
    // the first block that sees the flag (I-2). Latching it here instead would
    // anchor the envelope at whatever index the message thread happened to
    // read, which is up to one buffer behind the block that first applies it:
    // the first ramped sample would then start at 0.5*(1 + cos(pi*N/R))
    // instead of 1.0, and for any buffer N >= R (1440 samples at 48 kHz) that
    // is EXACTLY ZERO -- a hard cut, which is the click this ramp exists to
    // avoid. The callback also generates the envelope and releases the channel
    // when it reaches zero, so no other thread has to still be alive for the
    // sound to stop.
    void requestSoundcheckRampOut();

    [[nodiscard]] int          getSoundcheckOutputChannel() const;
    [[nodiscard]] std::int64_t getSoundcheckSampleIndex()   const;
    [[nodiscard]] bool         soundcheckIsEmitting()       const;   // scOutChannel_ >= 0

    // Raw mic capture, before ANY DSP, written once per callback while
    // scCaptureActive_. *** CALLER CONTRACT: read() only, one consumer
    // thread. *** Same reasoning as getTapBuffer(): the audio callback is and
    // must remain the sole producer.
    LockFreeRingBuffer<float>& getMicCaptureBuffer();

    // Samples the capture ring could not accept because the lane M thread fell
    // behind. Non-zero is a self-abort condition for the run (spec §4.3): a
    // splice in the captured signal makes the loop-gain estimate wrong in
    // every bin, exactly as a tap drop does for the detector.
    [[nodiscard]] std::uint64_t getMicCaptureDropCount() const;

    // TEST SEAM ONLY (B-4). isRunning_ is otherwise written only by start()
    // and audioDeviceError(); audioDeviceAboutToStart() does not touch it, so
    // no headless test can reach a state SoundcheckController::preflight
    // accepts. Stores the same atomic start() stores.
    void setRunningForTest (bool running);

    // TEST SEAM ONLY (B-5, F17). Multiplied into the injected sample AFTER
    // SoundcheckSignal has clamped it. 1.0f in every shipping path. It exists
    // because a seam on the PEAK achieves nothing -- the signal's constructor
    // and sampleAt both clamp -- so this is the only route by which the
    // +-1.0f output clamp can be SHOWN to still cover the sweep path
    // (invariant 4). Same shape as lane G's setRingRiskOverrideForTest.
    void setSoundcheckGainUnclampedForTest (float gain);

    // Command channel FROM the detector threads INTO the audio thread (bridge
    // design §2), one queue PER SLOT: the audio callback drains the queues
    // under ONE SHARED budget of kMaxCommandsPerCallback commands per callback
    // ACROSS ALL eight queues and applies each command to its slot's notch
    // chains. The no-argument overload is the legacy surface (slot 0).
    //
    // *** CALLER CONTRACT: write() only, and from exactly one thread per queue.
    // ***
    // Same reasoning as getTapBuffer(): the type system cannot stop a caller
    // from breaking the single-producer invariant -- the detector side is and
    // must remain the sole producer (owner decision D-05). A command's `slot`
    // field must match the queue it is written to; mismatched pairs are
    // skipped by the drain, never applied.
    LockFreeRingBuffer<NotchCommand>& getCommandQueue();
    LockFreeRingBuffer<NotchCommand>& getCommandQueue (int slot);

    // Slot routing configuration. Plain relaxed atomics: safe WITHOUT any
    // restart because the audio callback snapshots them at the top of each
    // block and re-validates every lane against the live channel counts -- a
    // mapping change landing mid-callback degrades to at most one block with
    // a valid-but-mixed route, never an out-of-bounds access. Thread:
    // message thread.
    void       setSlotConfig (int slotIndex, const SlotConfig& config);
    SlotConfig getSlotConfig (int slotIndex) const;

    // Channel names as the OPEN device reports them. Empty when no device is
    // open, which is the state the routing GUI is in while it populates its
    // channel selectors -- the nullptr guard is the contract, same as every
    // other device accessor above. Thread: message thread.
    juce::StringArray getInputChannelNames();
    juce::StringArray getOutputChannelNames();

    // TEST ACCESSOR ONLY -- lets tests assert chain state without an audio
    // device. Do not build product behaviour on this. The (channel) form is
    // the legacy surface: slot 0, lane = channel. The (slot, lane) form reaches
    // any chain in the 8x2 grid.
    const NotchChain& getNotchChainForTest (int channel) const;
    const NotchChain& getNotchChainForTest (int slot, int lane) const;

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

    // Multi-slot DSP grid (spec §3): 8 slots x up to 2 lanes each. Slot 0 is
    // enabled stereo {0,1} -> {0,1} by default, which is EXACTLY today's
    // behaviour, so every pre-existing caller and test sees no change. The
    // chains are pre-built at the nominal 48 kHz rate; audioDeviceAboutToStart()
    // retargets all of them to the device's actual rate before any notch is
    // set (Task 9 Part C).
    std::array<std::array<NotchChain, kMaxSlotLanes>, kMaxSlots> notchChains_
    {{
        {{ NotchChain (48000.0), NotchChain (48000.0) }},
        {{ NotchChain (48000.0), NotchChain (48000.0) }},
        {{ NotchChain (48000.0), NotchChain (48000.0) }},
        {{ NotchChain (48000.0), NotchChain (48000.0) }},
        {{ NotchChain (48000.0), NotchChain (48000.0) }},
        {{ NotchChain (48000.0), NotchChain (48000.0) }},
        {{ NotchChain (48000.0), NotchChain (48000.0) }},
        {{ NotchChain (48000.0), NotchChain (48000.0) }}
    }};

    // Per-slot, PER-LANE post-DSP taps (lane S): [slot][lane]. The audio
    // callback is the single producer of every ring; the slot's detector
    // thread is the single consumer of BOTH of its rings.
    static constexpr size_t kTapCapacity = 8192;  // ~170 ms @ 48 kHz, power of 2
    using TapPair = std::array<LockFreeRingBuffer<float>, kMaxSlotLanes>;
    std::array<TapPair, kMaxSlots> tapBuffers_
    {{
        {{ LockFreeRingBuffer<float> (kTapCapacity), LockFreeRingBuffer<float> (kTapCapacity) }},
        {{ LockFreeRingBuffer<float> (kTapCapacity), LockFreeRingBuffer<float> (kTapCapacity) }},
        {{ LockFreeRingBuffer<float> (kTapCapacity), LockFreeRingBuffer<float> (kTapCapacity) }},
        {{ LockFreeRingBuffer<float> (kTapCapacity), LockFreeRingBuffer<float> (kTapCapacity) }},
        {{ LockFreeRingBuffer<float> (kTapCapacity), LockFreeRingBuffer<float> (kTapCapacity) }},
        {{ LockFreeRingBuffer<float> (kTapCapacity), LockFreeRingBuffer<float> (kTapCapacity) }},
        {{ LockFreeRingBuffer<float> (kTapCapacity), LockFreeRingBuffer<float> (kTapCapacity) }},
        {{ LockFreeRingBuffer<float> (kTapCapacity), LockFreeRingBuffer<float> (kTapCapacity) }}
    }};
    std::array<std::array<std::atomic<std::uint64_t>, kMaxSlotLanes>, kMaxSlots> tapDropCounts_ {};

    // Per-slot detector -> audio command rings (bridge design §2). Capacity
    // 1024 gives 4x headroom over the worst legitimate burst per spec §3:
    // 16 notches x 2 lanes x 8 slots = 256 commands at once.
    static constexpr size_t kCommandCapacity = 1024;
    std::array<LockFreeRingBuffer<NotchCommand>, kMaxSlots> commandQueues_
    {
        { LockFreeRingBuffer<NotchCommand> (kCommandCapacity), LockFreeRingBuffer<NotchCommand> (kCommandCapacity),
          LockFreeRingBuffer<NotchCommand> (kCommandCapacity), LockFreeRingBuffer<NotchCommand> (kCommandCapacity),
          LockFreeRingBuffer<NotchCommand> (kCommandCapacity), LockFreeRingBuffer<NotchCommand> (kCommandCapacity),
          LockFreeRingBuffer<NotchCommand> (kCommandCapacity), LockFreeRingBuffer<NotchCommand> (kCommandCapacity) }
    };

    // Bound on worst-case callback time: 256 recomputes ~52 us against a
    // 670 us budget at a 32-sample buffer / 48 kHz (<10%). This is the TOTAL
    // shared across all eight rings per callback -- without that, eight
    // independently-capped rings would multiply the worst case by 8. Sized so
    // the full worst-case burst (256 commands) still applies within ONE
    // callback; anything beyond waits for the next one -- delayed, never
    // dropped.
    static constexpr int kMaxCommandsPerCallback = 256;

    // Audio-thread only: consumes up to maxCommands commands from ONE slot's
    // ring into that slot's chains and RETURNS how many were consumed, so the
    // caller can charge them against the shared per-callback budget. Called
    // for all 8 rings, first thing in the callback.
    int drainCommandsFrom (LockFreeRingBuffer<NotchCommand>& ring, int slot,
                           int maxCommands);

    // Slot routing state. Read relaxed by the audio callback. No lock and no
    // restart are needed: the callback snapshots into locals at the TOP of
    // every block AND bounds-checks every lane index against the device's
    // actual channel count before use, so a mapping change landing mid-
    // callback degrades to at most ONE block running a valid-but-mixed route
    // -- never an out-of-bounds access. width 0 = slot empty/disabled.
    std::array<std::atomic<bool>, kMaxSlots> slotEnabled_ {};
    std::array<std::atomic<int>,  kMaxSlots> slotWidth_   {};
    std::array<std::array<std::atomic<int>, kMaxSlotLanes>, kMaxSlots> slotInCh_  {};
    std::array<std::array<std::atomic<int>, kMaxSlotLanes>, kMaxSlots> slotOutCh_ {};

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

    // ---- Lane M: active soundcheck state (spec §4.1) -------------------
    //
    // EIGHT atomics, one counter, one ring. Every one is read by the audio
    // callback EXACTLY ONCE per block, into stack locals, beside the mode
    // snapshot; nothing below that point reads any of them again
    // (invariant 7). With the sample rate and the test gain seam that is TEN
    // loads in the snapshot block.
    //
    // All eight are reset to idle in audioDeviceAboutToStart() (C-1). They are
    // NOT owned by any device, so without that a run interrupted by a device
    // stop would leave scOutChannel_ armed and the next device to open would
    // emit sweep on channel N and mute every lane routed there, with the
    // controller long since aborted and joined.

    // Output channel under measurement, -1 = no run. The MUTE key: every lane
    // whose outputChannels[lane] equals this is silenced (Q15 relitigated).
    // Back to -1 at every Gap -- not usable as the tap-suspension key, see
    // scSuspendTaps_ below.
    std::atomic<int>          scOutChannel_       { -1 };
    // Tap suspension, held for the WHOLE run including every Gap (N3).
    std::atomic<bool>         scSuspendTaps_      { false };
    std::atomic<int>          scCaptureInChannel_ { -1 };
    // Capture gate, on across NoiseFloor + Sweep + Tail (F5). Separate from
    // scOutChannel_ so a safety property is not inferred from a sign. Capture
    // happens only while this is true AND scSuspendTaps_ is true: both live in
    // the same branch of the callback.
    std::atomic<bool>         scCaptureActive_    { false };
    // Sweep sample index, NEGATIVE during NoiseFloor. Audio thread OWNS it.
    std::atomic<std::int64_t> scSampleIndex_      { 0 };
    // Peak amplitude, already clamped to kSoundcheckMaxPeak before storing.
    std::atomic<float>        scPeak_             { 0.0f };
    // Ramp-out anchor, -1 = none. LATCHED BY THE CALLBACK from its own
    // snapshot (I-2), never by the message thread, so the envelope always
    // starts at exactly 1.0 whatever the buffer size. Cleared by the callback
    // when the ramp reaches zero, and by setSoundcheckOutputChannel().
    std::atomic<std::int64_t> scRampOutAtSample_  { -1 };
    // The REQUEST, which is all the message thread sets (C-2). Separating the
    // request from the anchor is what lets the anchor be latched on the audio
    // thread, and what gives a stale request somewhere to be cleared from.
    std::atomic<bool>         scRampOutRequested_ { false };

    // TEST SEAM (B-5): 1.0f in every shipping path. See the setter.
    std::atomic<float>        scGainUnclampedForTest_ { 1.0f };

    // Raw mic capture. NOT "how much audio the run needs" -- micCapture_ is a
    // stream the lane M thread drains every 5 ms, exactly like the detector's
    // taps. It is the maximum tolerable drain latency: 65536 samples = 1.37 s
    // @ 48 kHz, 0.68 s @ 96 kHz, 0.34 s @ 192 kHz -- still 68x the drain
    // period at the highest rate.
    static constexpr std::size_t kCaptureCapacity = 65536;
    LockFreeRingBuffer<float>   micCapture_      { kCaptureCapacity };
    // Same meaning, and the same reason for existing, as tapDropCounts_: a
    // short write leaves a SPLICE, not a gap, and a splice poisons every bin
    // of the loop-gain estimate. Non-zero aborts the run.
    std::atomic<std::uint64_t>  micCaptureDrops_ { 0 };

    // Last device error. A juce::String is reference-counted and NOT safe to
    // write on one thread while another reads it, so it takes a mutex rather
    // than an atomic. That is affordable here precisely because neither side
    // is the audio thread: audioDeviceError() is called from the device thread
    // and already logs, and the reader is the message thread. The audio
    // callback never touches this.
    mutable std::mutex lastDeviceErrorLock_;
    juce::String       lastDeviceError_;

    // Preferences applied on the next start(). Default type is the platform's
    // low-latency driver type (ASIO on Windows, CoreAudio on macOS); if that
    // driver is not registered, JUCE silently keeps the current type.
    juce::String desiredDeviceType_ { defaultDeviceTypeName() };
    juce::String desiredDeviceName_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};