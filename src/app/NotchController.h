// NotchController: the detector side of the audio<->detector bridge.
//
// Owns the AUTHORITATIVE notch model (owner decision D-05: the detector is
// the sole author of every Set/Clear) and is the sole producer of
// NotchCommands into the SPSC command ring that AudioEngine owns and drains.
//
// A sole author does not need to read back what it wrote -- it remembers what
// it commanded. The model is richer than NotchChain::NotchInfo because
// auto-release needs per-notch clocks (lockedAtMs, lastDetectedMs), which an
// audio-thread struct cannot carry.
//
// Threading (final form, arrived at over successive tasks):
//   - Policy entry points (setNotch/clearNotch/clearAll/adoptPreset):
//     message thread.
//   - run(): the detector thread loop -- runOnce() then wait(5). Polling,
//     never signalled from the audio thread: a WaitableEvent signal is a
//     kernel transition on a thread whose contract here is no locks/
//     allocation/logging; polling costs one wake per 5 ms against a ~170 ms
//     tap margin (design §4).
//   - Everything shared between those sides sits under modelMutex_. Neither
//     side is real-time, so an ordinary mutex is correct here; lock-free
//     machinery is reserved for the two channels touching the audio thread.
//
// Validation-before-send (bridge design §3, amended): the detector validates
// every command with the same predicates Biquad::setNotchFilter applies --
// sampleRate > 0, Q > 0, 0 < freq < sampleRate/2, depthDB <= 0 -- so the
// biquad's silent-rejection path is unreachable in practice.

#pragma once

#include "app/PresetManager.h"
#include "dsp/CandidateScorer.h"
#include "dsp/ClockSource.h"
#include "dsp/Detector.h"
#include "dsp/LockFreeRingBuffer.h"
#include "dsp/NotchCommand.h"
#include "dsp/PeakinessAnalyzer.h"

#include <juce_events/juce_events.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

// private inheritance: the thread is an implementation detail; nothing
// external should see Thread's interface.
class NotchController : private juce::Thread
{
public:
    // Recorded because D-05's rejected alternative ("detector leaves preset
    // notches alone") becomes a one-line policy change if origin is kept,
    // and impossible to add later if it is not.
    // KD-7: Soundcheck-origin notches are EXEMPT from auto-release -- they
    // clear only via an explicit clearNotch/clearAll.
    enum class Origin { Detector, Preset, Manual, Soundcheck };

    static constexpr int kChannels   = 2;
    static constexpr int kSlots      = 16;
    static constexpr int kTotalSlots = kChannels * kSlots;

    // Auto-release measures real elapsed time but ONLY while the tap is
    // delivering audio (owner decision D-06). The gate is "the tap delivered
    // RECENTLY", not "it delivered this poll" -- see the design doc §4 for
    // why the naive form halves the clock rate. 250 ms exceeds the longest
    // legitimate gap between tap writes (2048 samples @ 44.1 kHz = 46.4 ms)
    // with >5x margin. Bound assumed: buffer sizes up to ~2730 @ 44.1 kHz.
    static constexpr double kTapSilenceTimeoutMs = 250.0;
    // Spec §5.2 step 7: 30 s without peakiness releases a notch.
    static constexpr double kAutoReleaseMs       = 30000.0;

    static constexpr double kSoundcheckDurationMs = 15000.0;
    // Spec 5.2 step 6: a candidate must persist this many consecutive blocks
    // before a Set is emitted (~30 ms at a 10.67 ms hop).
    static constexpr int kPersistenceBlocks       = 3;

    NotchController (LockFreeRingBuffer<float>& tap,
                     LockFreeRingBuffer<NotchCommand>& commands,
                     ClockSource& clock);

    ~NotchController() override;

    // Lifecycle. start() launches the poll loop; stop(timeoutMs) joins the
    // thread. MainComponent MUST stop() this before any device restart can
    // clear the rings (bridge design §6.5 -- clear()'s precondition).
    void start();
    void stop (int timeoutMs);

    // Policy entry points. Message thread. setNotch validates BEFORE touching
    // anything; false means nothing changed anywhere.
    bool setNotch (int channel, int index,
                   double frequency, double Q, double depthDB,
                   Origin origin);
    void clearNotch (int channel, int index);
    void clearAll();

    // Owner decision D-05: a preset loaded mid-show is ADOPTED -- its notches
    // enter the model with Origin::Preset and auto-release treats them like
    // any other notch (30 s un-reinforced -> released). Each preset notch is
    // installed on BOTH channels (design §2 sizes the command burst as
    // 2 x 16). Returns how many preset notches were adopted; a notch whose
    // parameters fail validation on either channel is skipped entirely.
    int adoptPreset (const std::vector<PresetNotch>& notches);

    // KD-9: detection gating (Bypass must never place notches). Snapshot
    // publication is NOT affected by this flag.
    void setDetectionActive (bool active);

    // Spec 5.3: soundcheck detects for 15 s of LIVE time and its notches do
    // not auto-release. Sets detection active for the duration.
    void startSoundcheck();
    bool soundcheckActive() const;
    double getSoundcheckRemainingMs() const;   // 0 when inactive

    // One synchronous pump step: drain the spectrum, advance the live clock,
    // apply auto-release, flush the outbox.
    void runOnce();

    // TEST ACCESSOR ONLY -- like Detector::getAnalysisWindowForTest().
    double liveMsForTest() const;

    // One consistent frame for the GUI: the spectrum and the notch list are
    // captured under one lock at one instant (bridge design §5). Caller owns
    // the destination -- returning a container would allocate every paint.
    struct SnapshotNotch
    {
        float frequency = 0.0f;
        float Q         = 0.0f;
        float depthDB   = 0.0f;
        std::uint8_t channel = 0;
        std::uint8_t index   = 0;
    };

    struct SnapshotBuffer
    {
        std::array<float, Detector::kNumBins> magnitudes {};
        std::uint32_t magnitudeCount = 0;
        double sampleRate = 0.0;
        std::array<SnapshotNotch, kTotalSlots> notches {};
        std::uint32_t notchCount = 0;
        std::uint64_t sequence = 0;   // increments on every published frame
    };

    void copySnapshot (SnapshotBuffer& destOwnedByCaller) const;

    // Commands that had to be retried because the command ring was full.
    // Sustained growth means the audio callback stopped draining -- a real
    // fault the UI should be able to surface.
    std::uint64_t retryCount() const;

    void setSampleRate (double sampleRate);

private:
    void run() override;

    struct ModelNotch
    {
        double frequency    = 0.0;
        double Q            = 0.0;
        double depthDB      = 0.0;
        double lockedAtMs   = 0.0;
        double lastDetectedMs = 0.0;
        Origin origin       = Origin::Detector;
        bool   active       = false;
    };

    static constexpr int slotOf (int channel, int index) { return channel * kSlots + index; }

    void flushOutbox();
    void pushClearLocked (int channel, int index);

    void processSpectrumForDetection (const Detector::Spectrum& block, double blockNowMs);
    int  firstFreeSlotLocked() const;
    double remainingSoundcheckMs() const;

    LockFreeRingBuffer<float>&      tap_;
    LockFreeRingBuffer<NotchCommand>& commands_;
    ClockSource&                    clock_;

    Detector detector_;

    // Detection policy (KD-5..KD-9): analyzer + scorer live entirely on the
    // detector thread; persistence_ is per-bin consecutive-confirm counters.
    PeakinessAnalyzer analyzer_;
    CandidateScorer   scorer_;
    std::atomic<bool> detectionActive_ { false };
    // Atomic is belt-and-braces only: ALWAYS accessed under modelMutex_
    // together with liveMs_, which is what actually serialises it.
    std::atomic<double> soundcheckEndsAtLiveMs_ { -1.0 };
    std::vector<std::uint32_t> persistence_;   // per bin, sized kBins on first use
    // Wall-clock reading of the last drained spectrum block, for the scorer's
    // inter-block gap. <= 0 means "no previous block yet" -> elapsed 0.
    double previousBlockNowMs_ = 0.0;

    mutable std::mutex modelMutex_;               // guards model_ and outbox_ (mutable: liveMsForTest() is const)
    std::array<ModelNotch, kTotalSlots> model_;
    std::vector<NotchCommand> outbox_;

    std::atomic<std::uint64_t> retryCount_ { 0 };

    // Live-clock state, all under modelMutex_. liveMs_ advances only while
    // the tap is alive; EVERY spec timer (30 ms candidate persistence,
    // 15 s soundcheck, 30 s auto-release) reads liveMs_, never the wall clock.
    double liveMs_     = 0.0;
    double lastPollMs_ = 0.0;
    // Sentinel far below any real time: "the tap has never delivered".
    double lastDataMs_ = -1.0e9;

    // One mutex, one struct (design §5): spectrum and notch list are always
    // paired to the same instant. mutable: copySnapshot is const.
    mutable std::mutex snapshotMutex_;
    SnapshotBuffer    latest_;
};
