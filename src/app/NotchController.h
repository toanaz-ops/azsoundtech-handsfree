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
//   - runOnce(): called from the detector thread loop (and directly from
//     tests). Flushes the outbox into the command ring.
//   - Everything shared between those sides sits under modelMutex_. Neither
//     side is real-time, so an ordinary mutex is correct here; lock-free
//     machinery is reserved for the two channels touching the audio thread.
//
// Validation-before-send (bridge design §3, amended): the detector validates
// every command with the same predicates Biquad::setNotchFilter applies --
// sampleRate > 0, Q > 0, 0 < freq < sampleRate/2, depthDB <= 0 -- so the
// biquad's silent-rejection path is unreachable in practice.

#pragma once

#include "dsp/ClockSource.h"
#include "dsp/Detector.h"
#include "dsp/LockFreeRingBuffer.h"
#include "dsp/NotchCommand.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

class NotchController
{
public:
    // Recorded because D-05's rejected alternative ("detector leaves preset
    // notches alone") becomes a one-line policy change if origin is kept,
    // and impossible to add later if it is not.
    enum class Origin { Detector, Preset, Manual };

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

    NotchController (LockFreeRingBuffer<float>& tap,
                     LockFreeRingBuffer<NotchCommand>& commands,
                     ClockSource& clock);

    // Policy entry points. Message thread. setNotch validates BEFORE touching
    // anything; false means nothing changed anywhere.
    bool setNotch (int channel, int index,
                   double frequency, double Q, double depthDB,
                   Origin origin);
    void clearNotch (int channel, int index);
    void clearAll();

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

    LockFreeRingBuffer<float>&      tap_;
    LockFreeRingBuffer<NotchCommand>& commands_;
    ClockSource&                    clock_;

    Detector detector_;

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
