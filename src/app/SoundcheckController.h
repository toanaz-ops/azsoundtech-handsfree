// src/app/SoundcheckController.h
//
// Lane M, spec §4.3. The state machine that decides WHEN the active soundcheck
// emits, for how long, on which output channel, and when it stops. Everything
// audible in lane M is downstream of this class: AudioEngine only obeys eight
// atomics, and this is the only thing that writes them.
//
// THREAD MAP -- every public method is labelled below, and the labels are the
// contract, not documentation of an accident:
//
//   MESSAGE THREAD   preflight(), arm(), applyRequested(), dismissRequested(),
//                    requestStop(), abortAndJoin(), start(), stop(), and the
//                    assignment of the three std::function members.
//   LANE M THREAD    run() -> runOnce() in a wait(kPollMs) loop. It drains the
//                    engine's mic-capture ring, runs LoopGainEstimator and
//                    SoundcheckCandidates, writes the eight engine atomics and
//                    calls the injected lambdas. It NEVER calls NotchController
//                    (inv 17) -- this class holds no pointer to one.
//   AUDIO CALLBACK   not entered here at all. The callback reads the atomics
//                    this class writes, owns scSampleIndex_ once a run is in
//                    flight, latches the ramp-out anchor and releases the
//                    channel when the envelope reaches zero (inv 9).
//   ANY THREAD       getState(), getCurrentTargetIndex(), getTargetCount(),
//                    getElapsedMsInRun(), getRemainingMsInRun(), copyResults(),
//                    copyResultsForSlot(), worstPeakinessForTest().
//
// NO NotchController POINTER (inv 17). Detection has to be back ON before the
// machine enters Results (inv 12) -- lane G's release ladder would otherwise
// walk a -24 notch up two rungs during the 20 s results window while spec §3
// declares 0 dB. A MessageManager::callAsync hop returns immediately, so the
// state would reach Results first; holding a NotchController* would break inv
// 17's letter. So the owner injects setDetectionActiveOnAllSlots, whose entire
// body is one relaxed atomic store per slot, and this class calls it
// synchronously on its own thread.
//
// RunParams IS FROZEN FOR THE RUN. Read once at Arm on the message thread and
// never re-read. A threshold that moved mid-run would score channel 1 and
// channel 9 of the SAME measurement on two different rulers, and nobody
// reading the log could tell. The sample rate and the channel counts are
// frozen for a second reason: they are the reference the run compares the live
// device against every poll, so a device change is DETECTED (and aborts)
// instead of silently changing the arithmetic.
//
// runOnce() IS THE WHOLE STATE MACHINE. run() is nothing but a loop around it
// with wait(kPollMs), exactly as NotchController::runOnce/run are. Every test
// drives runOnce() by hand on a fake clock, so no test starts a thread.
#pragma once

#include "app/AudioEngine.h"
#include "app/NotchController.h"
#include "app/SessionLogger.h"
#include "dsp/ClockSource.h"
#include "dsp/LoopGainEstimator.h"
#include "dsp/PeakinessAnalyzer.h"
#include "dsp/SoundcheckCandidates.h"
#include "dsp/SoundcheckSignal.h"

#include <juce_dsp/juce_dsp.h>
#include <juce_events/juce_events.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

// private inheritance, as NotchController does it: the thread is an
// implementation detail and nothing outside should see Thread's interface.
class SoundcheckController : private juce::Thread
{
public:
    static constexpr double kTailSeconds      = 0.7;
    static constexpr double kGapMs            = 300.0;
    static constexpr double kNoiseFloorMs     = 500.0;
    static constexpr double kMicAbortDbfs     = -6.0;
    static constexpr double kMicAbortHoldMs   = 20.0;
    static constexpr double kResultsTimeoutMs = 20000.0;
    static constexpr double kPollMs           = 5.0;

    // NOT IN THE BRIEF -- added here, and it is a safety property, not a
    // convenience. The noise-floor GATE has to decide BEFORE the first sample
    // leaves, and the callback starts the sweep the instant scSampleIndex_
    // reaches 0 with nothing else to ask. If the index were set to exactly
    // -(kNoiseFloorMs worth), the controller's gate poll would land at index
    // ~0 and a ringing room could get one buffer of sweep before the abort.
    // The index therefore starts one guard EARLIER, and the gate is evaluated
    // at kNoiseFloorMs -- so the decision is always at least kNoiseFloorGuardMs
    // of SILENCE ahead of the first sample. 20 ms covers one poll (5 ms) plus
    // a 512-sample buffer at 44.1 kHz (11.6 ms).
    //
    // Cost: the declared 0.5 s of silence per channel becomes 0.52 s, i.e.
    // 4.52 s per channel instead of 4.5 s (+0.3 s over 16 channels).
    static constexpr double kNoiseFloorGuardMs = 20.0;

    // Aliases -- one definition each, no second literal (lane G m-D):
    static constexpr double kSweepLowHz        = SoundcheckSignal::kSweepLowHz;
    static constexpr double kSweepHighHz       = SoundcheckSignal::kSweepHighHz;
    static constexpr double kSweepSeconds      = SoundcheckSignal::kSweepSeconds;
    static constexpr double kRampMs            = SoundcheckSignal::kRampMs;
    static constexpr double kRampOutMs         = SoundcheckSignal::kRampOutMs;
    static constexpr float  kSoundcheckMaxPeak = SoundcheckSignal::kSoundcheckMaxPeak;
    static constexpr float  kSoundcheckMinPeak = SoundcheckSignal::kSoundcheckMinPeak;
    static constexpr double kTrustedHighHz     = LoopGainEstimator::kTrustedHighHz;
    static constexpr double kMinBandSnrDb      = LoopGainEstimator::kMinBandSnrDb;
    static constexpr double kMinBinSnrDb       = LoopGainEstimator::kMinBinSnrDb;
    static constexpr double kCandidateMarginDb = SoundcheckCandidates::kCandidateMarginDb;
    static constexpr double kMinUsefulCutDb    = SoundcheckCandidates::kMinUsefulCutDb;
    static constexpr double kMinProminenceDb   = SoundcheckCandidates::kMinProminenceDb;
    static constexpr double kTargetMarginDb    = SoundcheckCandidates::kTargetMarginDb;
    static constexpr int    kMaxPreventivePerLane = SoundcheckCandidates::kMaxPreventivePerLane;

    // Wall-clock cost of one output channel, and of a whole run. Derived from
    // the phase constants above so the Confirm dialog's "~72 s" and the
    // machine's own deadlines can never disagree.
    static constexpr double kPerTargetMs =
        kNoiseFloorMs + kNoiseFloorGuardMs + kSweepSeconds * 1000.0
        + kTailSeconds * 1000.0 + kGapMs;

    enum class State  { Idle, Preflight, Confirm, Arm, NoiseFloor, Sweep, Tail,
                        Analyse, Gap, Results, Abort };
    enum class Refusal { None, EngineNotRunning, NoChannels, SlotDisabled,
                         InvalidChannelPair, RingRiskRising };
    enum class AbortReason { UserStop, Esc, EngineStopped, DeviceError, DeviceChanged,
                             MicHot, RoomRinging, CaptureDrop };

    struct Target { int slot = 0, lane = 0, outChannel = 0, inChannel = 0; };

    struct RunParams
    {
        float  noiseFloorGate   = 0.0f;   // = getPeakinessThreshold() at Arm. A PEAKINESS
                                          // RATIO, never a 0..1 score (N1).
        float  peak             = 0.0f;   // already clamped <= kSoundcheckMaxPeak
        double sampleRate       = 0.0;
        int    numInputChannels = 0, numOutputChannels = 0;
        // The running preset's ceiling -- the SHALLOWEST cut this run is
        // allowed to propose. arm() REFUSES a non-finite value rather than
        // letting it reach SoundcheckCandidates::Input::ceilingDb, whose NaN
        // default means "unset" and produces marks with no proposals.
        double ceilingDb        = 0.0;
        double notchQ           = 0.0;
    };

    struct OutputResult
    {
        int   slot = 0, lane = 0, outChannel = 0, inChannel = 0;
        bool  measured = false;         // false = "could not measure" (Q8) -- a VALID result
        bool  routingInvalid = false;   // different from measured == false (F26)
        // Neither of these is "the room is clean". Both mean the CALLER handed
        // pick() something it could not propose from, and the operator must be
        // told that rather than shown an empty proposal list (Task 3 I-3).
        bool  ceilingMissing = false;
        bool  ladderMissing  = false;
        float snrDb = 0.0f;
        std::array<float, LoopGainEstimator::kNumBins> marginDb {};   // = -H_dB
        std::array<bool,  LoopGainEstimator::kNumBins> trusted {};
        // I-3: the PER-BIN flags, not just the count. SpectrumView's overlay
        // draws a marker per marked bin, and SoundcheckCandidates::Output
        // already produces the array.
        std::array<bool,  LoopGainEstimator::kNumBins> marked {};
        int   markedCount = 0, candidateCount = 0, saturatedBins = 0;
        struct Candidate { float hz = 0.0f, marginDb = 0.0f, depthDb = 0.0f, q = 0.0f,
                           residualDb = 0.0f; int bin = 0; };
        std::array<Candidate, kMaxPreventivePerLane> candidates {};
    };

    SoundcheckController (AudioEngine& engine, ClockSource& clock);
    ~SoundcheckController() override;

    // --- MESSAGE THREAD ---
    [[nodiscard]] Refusal preflight (const std::vector<Target>& targets,
                                     const NotchController::SnapshotBuffer& risk) const;
    // Returns false without touching the engine when params are unusable
    // (no targets, sampleRate <= 0, non-finite ceiling) or a run is already in
    // flight. A refusal here emits NOT ONE SAMPLE (inv 19).
    bool  arm (std::vector<Target> targets, const RunParams& params);
    void  applyRequested();          // Results -> Idle, after Task 7 has placed
    void  dismissRequested();        // BO
    void  requestStop (AbortReason reason);
    void  abortAndJoin();            // device restart path; blocks until Idle

    // --- ANY THREAD (reads) ---
    [[nodiscard]] State  getState() const;
    [[nodiscard]] int    getCurrentTargetIndex() const;
    [[nodiscard]] int    getTargetCount() const;
    [[nodiscard]] double getElapsedMsInRun() const;
    [[nodiscard]] double getRemainingMsInRun() const;
    [[nodiscard]] std::vector<OutputResult> copyResults() const;
    // I-4: consumed by MainComponent's APPLY lambda (Task 10), which walks the
    // slots and calls applySoundcheckResults once per slot's NotchController.
    [[nodiscard]] std::vector<OutputResult> copyResultsForSlot (int slot) const;

    // Injected so this class holds NO NotchController pointer (inv 17). The
    // owner's lambda does nothing but relaxed atomic stores
    // (NotchController::setDetectionActive).
    //
    // *** LIFETIME CONTRACT (I-10) ***
    // All three are ASSIGNED ONCE, BEFORE start(), AND NEVER AFTER. They are
    // invoked from the lane M thread; a std::function assigned while it is
    // being invoked is a data race, and these are bare public members with no
    // lock. Before any reassignment or destruction, abortAndJoin() or
    // stop() must have RETURNED. MainComponent assigns them in its
    // constructor (Task 10 Step 2), before the controller is ever started.
    std::function<void (bool)>             setDetectionActiveOnAllSlots;
    std::function<void (const juce::var&)> logEvent;      // message- or lane-M thread
    std::function<void()>                  onStateChanged;   // GUI repaint request

    // One poll step. run() is this in a loop with wait(kPollMs); every test
    // drives it directly, so no test starts a thread.
    void runOnce();

    void start();
    void stop (int timeoutMs);

    // TEST ACCESSORS ONLY.
    [[nodiscard]] float worstPeakinessForTest() const;   // last noise window's max
    // I-7: the enum -> string mapper, so a test can loop the ENUMERATORS
    // instead of asserting that eight string literals differ.
    [[nodiscard]] static const char* abortReasonNameForTest (AbortReason r);
    // I-8: forwards to the file-local round3sf so the log-shape test can call
    // the same rounding the writer uses, qualified.
    [[nodiscard]] static double roundToThreeSignificantFiguresForTest (double v);

private:
    void run() override;

    // --- phase helpers, all on the lane M thread (enterTarget(0) is also
    // reached from arm() on the message thread, before the thread exists) ---
    void enterTarget (int index);
    void enterGap();
    void finishRun();
    void beginAbort (AbortReason reason);
    void stopEmissionSafely();            // ramp-out while emitting, backstop while silent
    bool checkDeviceUnchanged() const;     // sample rate + channel counts vs RunParams
    bool micIsHot();                       // advances the hold timer, so NOT const
    bool noiseWindowIsRinging() const;     // peakinessAt vs params_.noiseFloorGate
    void drainCapture();
    void computeNoiseSpectrum();
    void analyseCurrentTarget();
    bool serviceEmittingPhase();           // the common abort checks; false == aborted
    void notifyStateChanged() const;
    void logAbort (AbortReason reason) const;

    AudioEngine& engine_;
    ClockSource& clock_;

    mutable std::mutex stateMutex_;       // guards targets_, results_
    std::atomic<State> state_ { State::Idle };
    std::vector<Target>       targets_;
    std::vector<OutputResult> results_;
    RunParams params_ {};

    std::atomic<int> targetIndex_ { 0 };
    std::atomic<int> targetCount_ { 0 };
    double phaseEndsAtMs_  = 0.0;          // lane M thread only
    std::atomic<double> runStartedAtMs_ { 0.0 };   // read by getElapsedMsInRun from any thread
    double micHotSinceMs_  = -1.0;
    double micSumSq_       = 0.0;          // this poll's drained energy
    std::size_t micCount_  = 0;
    std::uint64_t dropsAtArm_ = 0;
    std::atomic<bool> stopRequested_ { false };
    std::atomic<AbortReason> stopReason_ { AbortReason::UserStop };

    // What RING RISK said at the LAST preflight(). arm() takes no snapshot --
    // by design, the message thread calls preflight() again immediately before
    // arming -- so this is how the number reaches soundcheck_start. `valid`
    // false is logged as null, never as 0.0 (R3-2).
    mutable std::atomic<bool>  lastRiskValid_ { false };
    mutable std::atomic<float> lastRiskScore_ { 0.0f };

    // Sample bookkeeping for the CURRENT target, lane M thread only.
    std::int64_t noiseFloorSamples_ = 0;   // includes kNoiseFloorGuardMs
    std::int64_t capturedSamples_   = 0;
    std::size_t  noiseWindowFill_   = 0;

    LoopGainEstimator estimator_ { 48000.0 };
    std::vector<float> capture_;          // drain scratch, sized once at Arm
    std::vector<float> noiseWindow_;      // the whole noise-floor phase
    std::vector<float> reference_;        // the regenerated sweep, built once at Arm

    // The magnitude spectrum of the noise window, so noiseWindowIsRinging()
    // can run PeakinessAnalyzer::peakinessAt over it without allocating.
    std::array<float, LoopGainEstimator::kNumBins> noiseMagnitudes_ {};
    // The worst peakiness the last noise window produced. Published through
    // worstPeakinessForTest() so a fixture that drifts over the gate fails as a
    // FIXTURE problem rather than as a false gate failure.
    std::atomic<float> worstPeakiness_ { 0.0f };

    // The noise-window transform. Same geometry as the detector's (2048-point,
    // Hann, hop 512) so a bin means the same frequency on both rulers.
    juce::dsp::FFT fft_ { Detector::kFftOrder };
    juce::dsp::WindowingFunction<float> hann_
        { (std::size_t) Detector::kFftSize, juce::dsp::WindowingFunction<float>::hann, false };
    std::vector<float> fftScratch_;       // 2 * kFftSize, allocated in the ctor
    std::array<double, LoopGainEstimator::kNumBins> noisePower_ {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SoundcheckController)
};

// MESSAGE THREAD ONLY. Defined by Task 7 -- declared here so Task 7 adds a
// definition rather than a second surface.
struct SoundcheckApplyStats { int placed = 0, refused = 0, clearedPrevious = 0; };
SoundcheckApplyStats applySoundcheckResults (
    NotchController& controller,
    const std::vector<SoundcheckController::OutputResult>& results);
