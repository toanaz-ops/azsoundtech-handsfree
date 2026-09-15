// src/app/SoundcheckController.cpp
//
// See the header for the thread map, the inv-17 argument and why RunParams is
// frozen. This file is the state machine itself.
#include "app/SoundcheckController.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
// juce::JSON prints a double to 18 significant digits, so every number that
// reaches a var is rounded first -- otherwise one release ladder rung reads
// "-12.000000000000002" in the session log and logstats has to parse noise
// (memory/data-loop-lessons-2026-09-05.md).
double round3sf (double v)
{
    if (! std::isfinite (v) || v == 0.0)
        return std::isfinite (v) ? v : 0.0;

    const double mag   = std::floor (std::log10 (std::abs (v)));
    const double scale = std::pow (10.0, 2.0 - mag);
    return std::round (v * scale) / scale;
}

const char* modeName (AudioEngine::Mode m)
{
    switch (m)
    {
        case AudioEngine::Mode::Bypass:     return "bypass";
        case AudioEngine::Mode::Auto:       return "auto";
        case AudioEngine::Mode::Soundcheck: return "soundcheck";
    }
    return "unknown";
}

juce::DynamicObject* objectOf (juce::var& v)
{
    return v.getDynamicObject();
}
} // namespace

const char* SoundcheckController::abortReasonNameForTest (AbortReason r)
{
    switch (r)
    {
        case AbortReason::UserStop:      return "user_stop";
        case AbortReason::Esc:           return "esc";
        case AbortReason::EngineStopped: return "engine_stopped";
        case AbortReason::DeviceError:   return "device_error";
        case AbortReason::DeviceChanged: return "device_changed";
        case AbortReason::MicHot:        return "mic_hot";
        case AbortReason::RoomRinging:   return "room_ringing";
        case AbortReason::CaptureDrop:   return "capture_drop";
    }
    return "unknown";
}

double SoundcheckController::roundToThreeSignificantFiguresForTest (double v)
{
    return round3sf (v);
}

SoundcheckController::SoundcheckController (AudioEngine& engine, ClockSource& clock)
    : juce::Thread ("AZSoundcheck"),
      engine_ (engine),
      clock_ (clock)
{
    // MUST be 2 * kFftSize floats: performFrequencyOnlyForwardTransform reads
    // and writes the whole array even though only kNumBins entries come back
    // (Detector.h:34-35). Sizing it kFftSize overruns by 8 KB.
    fftScratch_.assign ((std::size_t) Detector::kFftSize * 2, 0.0f);
}

SoundcheckController::~SoundcheckController()
{
    // The lambdas are invoked from this thread; they must not be destroyed
    // (this object's members die right after) while a poll is in flight.
    stop (2000);
}

// ---------------------------------------------------------------- MESSAGE ---

SoundcheckController::Refusal
SoundcheckController::preflight (const std::vector<Target>& targets,
                                 const NotchController::SnapshotBuffer& riskSnapshot) const
{
    // Recorded for soundcheck_start even when the answer is None: arm() does
    // not take a snapshot, and "what did RING RISK say when the operator
    // pressed the button" is the one thing a later post-mortem needs.
    lastRiskValid_.store (riskSnapshot.ringRiskValid, std::memory_order_relaxed);
    lastRiskScore_.store (riskSnapshot.ringRiskScore, std::memory_order_relaxed);

    if (! engine_.isRunning())
        return Refusal::EngineNotRunning;

    const int ins  = engine_.getNumInputChannels();
    const int outs = engine_.getNumOutputChannels();
    if (ins <= 0 || outs <= 0 || targets.empty())
        return Refusal::NoChannels;

    for (const auto& t : targets)
    {
        if (t.slot < 0 || t.slot >= kMaxSlots)
            return Refusal::SlotDisabled;

        const auto cfg = engine_.getSlotConfig (t.slot);
        if (! cfg.enabled || (cfg.width != 1 && cfg.width != 2))
            return Refusal::SlotDisabled;

        // F26: the lane loop merely `continue`s past a bad pair
        // (AudioEngine.cpp:546-548), so a routing fault would otherwise read as
        // "could not measure" -- a very different message for the operator.
        if (t.lane < 0 || t.lane >= cfg.width
            || t.inChannel  < 0 || t.inChannel  >= ins
            || t.outChannel < 0 || t.outChannel >= outs
            || cfg.inputChannels[t.lane]  != t.inChannel
            || cfg.outputChannels[t.lane] != t.outChannel)
            return Refusal::InvalidChannelPair;
    }

    // R3-2. BOTH sides are 0..1 products here: ringRiskScore is the same
    // `score` the placement decision compares against kConfirmScore, and
    // ringRiskThreshold IS kConfirmScore. The product is taken from the
    // snapshot rather than written as 0.385 so the GUI's RISING band and this
    // gate cannot drift apart.
    //
    // ringRiskValid == false does NOT refuse: it means "no frame scored yet",
    // which is true in Bypass and after every reset. It is logged as null.
    if (riskSnapshot.ringRiskValid
        && riskSnapshot.ringRiskScore
             >= NotchController::kRiskFreezeFraction * riskSnapshot.ringRiskThreshold)
        return Refusal::RingRiskRising;

    return Refusal::None;
}

bool SoundcheckController::arm (std::vector<Target> targets, const RunParams& params)
{
    if (state_.load (std::memory_order_acquire) != State::Idle)
        return false;

    // Everything below is a CALLER BUG, not a room. Refusing here is what
    // stops a bug from becoming either a wrong cut or a plausible-looking
    // "room clean" that nobody questions.
    if (targets.empty())
        return false;
    if (! (params.sampleRate > 0.0))
        return false;
    // Task 3 I-3: a non-finite ceiling reaches SoundcheckCandidates as "unset"
    // and produces marks with no proposals. It must never look like a quiet
    // room, and the cheapest place to make sure is before a single sample.
    if (! std::isfinite (params.ceilingDb))
        return false;
    // N1: the gate is a PEAKINESS RATIO. Zero is not a gate, it is a field
    // nobody filled in -- and it would abort every run in every room.
    if (! (params.noiseFloorGate > 0.0f))
        return false;
    if (params.numInputChannels <= 0 || params.numOutputChannels <= 0)
        return false;

    params_      = params;
    params_.peak = SoundcheckSignal::clampPeak (params.peak);

    {
        const std::lock_guard<std::mutex> lock (stateMutex_);
        targets_ = std::move (targets);
        results_.clear();
        results_.reserve (targets_.size());
    }
    targetCount_.store ((int) targets_.size(), std::memory_order_relaxed);
    targetIndex_.store (0, std::memory_order_relaxed);

    // --- every allocation this run will make happens HERE, on the message
    // thread. The lane M thread allocates nothing per poll. ---
    noiseFloorSamples_ = (std::int64_t) std::llround (
        (kNoiseFloorMs + kNoiseFloorGuardMs) * params_.sampleRate / 1000.0);
    noiseWindow_.assign ((std::size_t) std::max<std::int64_t> (noiseFloorSamples_, 1), 0.0f);
    capture_.assign (8192, 0.0f);

    SoundcheckSignal::Params sig;
    sig.sampleRate = params_.sampleRate;
    sig.peak       = params_.peak;
    const SoundcheckSignal signal { sig };
    reference_.assign ((std::size_t) std::max<std::int64_t> (signal.totalSamples(), 1), 0.0f);
    for (std::int64_t n = 0; n < signal.totalSamples(); ++n)
        reference_[(std::size_t) n] = signal.sampleAt (n);

    dropsAtArm_    = engine_.getMicCaptureDropCount();
    micHotSinceMs_ = -1.0;
    stopRequested_.store (false, std::memory_order_relaxed);
    worstPeakiness_.store (0.0f, std::memory_order_relaxed);
    runStartedAtMs_.store (clock_.nowMs(), std::memory_order_relaxed);

    engine_.setSoundcheckPeak (params_.peak);
    // Held for the WHOLE run, across every Gap (inv 10): keying suspension on
    // the output channel would un-suspend the taps for 300 ms per channel and
    // walk lane G's release clock down a rung.
    engine_.setSoundcheckTapsSuspended (true);
    if (setDetectionActiveOnAllSlots)
        setDetectionActiveOnAllSlots (false);

    if (logEvent)
    {
        auto ev = SessionLogger::makeEvent ("soundcheck_start");
        if (auto* o = objectOf (ev))
        {
            o->setProperty ("outputs",  (int) targetCount_.load (std::memory_order_relaxed));
            o->setProperty ("peak_dbfs",
                            round3sf (20.0 * std::log10 (std::max ((double) params_.peak, 1.0e-9))));
            o->setProperty ("sweep_ms", round3sf (kSweepSeconds * 1000.0));
            o->setProperty ("total_ms",
                            round3sf (kPerTargetMs * targetCount_.load (std::memory_order_relaxed)));
            o->setProperty ("mode_before", modeName (engine_.getMode()));
            // null, NOT 0.0: "no frame scored yet" is a different statement
            // from "the room scored zero", and a soundman reading the log a
            // week later must be able to tell them apart (R3-2).
            o->setProperty ("ring_risk",
                            lastRiskValid_.load (std::memory_order_relaxed)
                                ? juce::var (round3sf ((double) lastRiskScore_.load (
                                                 std::memory_order_relaxed)))
                                : juce::var());
            o->setProperty ("gate", round3sf ((double) params_.noiseFloorGate));
        }
        logEvent (ev);
    }

    enterTarget (0);
    return true;
}

void SoundcheckController::applyRequested()
{
    if (state_.load (std::memory_order_acquire) != State::Results)
        return;
    // The soundcheck_apply event carries placed/refused/cleared_previous, which
    // only applySoundcheckResults() can count (Task 7). It is logged by the
    // owner after that call, not here.
    state_.store (State::Idle, std::memory_order_release);
    notifyStateChanged();
}

void SoundcheckController::dismissRequested()
{
    if (state_.load (std::memory_order_acquire) != State::Results)
        return;
    state_.store (State::Idle, std::memory_order_release);
    notifyStateChanged();
}

void SoundcheckController::requestStop (AbortReason reason)
{
    stopReason_.store (reason, std::memory_order_relaxed);
    stopRequested_.store (true, std::memory_order_release);

    // inv 9, F8: the SOUND stops here, on the message thread, without waiting
    // for the lane M thread to poll. Everything the callback needs to finish
    // the ramp-out and release the channel is already in the atomics; the
    // state machine catches up at its own pace.
    stopEmissionSafely();
}

void SoundcheckController::abortAndJoin()
{
    // The device-restart path (Task 10). audioDeviceAboutToStart() clears the
    // soundcheck atomics, which is only safe because this has returned first.
    requestStop (AbortReason::DeviceChanged);
    runOnce();          // execute the abort, whether or not the thread is running
    stop (1000);
}

void SoundcheckController::start()
{
    startThread();
}

void SoundcheckController::stop (int timeoutMs)
{
    stopThread (timeoutMs);
}

void SoundcheckController::run()
{
    while (! threadShouldExit())
    {
        runOnce();
        wait ((int) kPollMs);
    }
}

// ------------------------------------------------------------- ANY THREAD ---

SoundcheckController::State SoundcheckController::getState() const
{
    return state_.load (std::memory_order_acquire);
}

int SoundcheckController::getCurrentTargetIndex() const
{
    return targetIndex_.load (std::memory_order_relaxed);
}

int SoundcheckController::getTargetCount() const
{
    return targetCount_.load (std::memory_order_relaxed);
}

double SoundcheckController::getElapsedMsInRun() const
{
    if (state_.load (std::memory_order_acquire) == State::Idle)
        return 0.0;
    return std::max (0.0, clock_.nowMs() - runStartedAtMs_.load (std::memory_order_relaxed));
}

double SoundcheckController::getRemainingMsInRun() const
{
    const double total = kPerTargetMs * (double) targetCount_.load (std::memory_order_relaxed);
    return std::max (0.0, total - getElapsedMsInRun());
}

std::vector<SoundcheckController::OutputResult> SoundcheckController::copyResults() const
{
    const std::lock_guard<std::mutex> lock (stateMutex_);
    return results_;
}

std::vector<SoundcheckController::OutputResult>
SoundcheckController::copyResultsForSlot (int slot) const
{
    const std::lock_guard<std::mutex> lock (stateMutex_);
    std::vector<OutputResult> out;
    for (const auto& r : results_)
        if (r.slot == slot)
            out.push_back (r);
    return out;
}

float SoundcheckController::worstPeakinessForTest() const
{
    return worstPeakiness_.load (std::memory_order_relaxed);
}

// ------------------------------------------------------------ LANE M POLL ---

void SoundcheckController::runOnce()
{
    switch (state_.load (std::memory_order_acquire))
    {
        case State::Idle:
        case State::Preflight:
        case State::Confirm:
        case State::Arm:
        case State::Abort:
            // Nothing is in flight, so nothing can be aborted. Clearing the
            // flag here is what makes abortAndJoin() safe to call at any time.
            stopRequested_.store (false, std::memory_order_relaxed);
            return;

        case State::NoiseFloor:
        {
            if (! serviceEmittingPhase())
                return;
            if (clock_.nowMs() < phaseEndsAtMs_)
                return;

            // THE GATE. It runs while the index is still negative -- i.e. while
            // the callback is emitting nothing at all -- so a ringing room is
            // refused BEFORE the first sample, not one buffer after it.
            computeNoiseSpectrum();
            if (noiseWindowIsRinging())
            {
                beginAbort (AbortReason::RoomRinging);
                return;
            }

            // The GUARD is added back HERE, not to the gate deadline: the
            // sweep's first sample leaves kNoiseFloorGuardMs from now (the
            // index is still that far short of 0), so the Sweep phase has to
            // end that much later or Tail and Analyse would both run a guard
            // early and clip the end of the measured tail.
            phaseEndsAtMs_ += kNoiseFloorGuardMs + kSweepSeconds * 1000.0;
            state_.store (State::Sweep, std::memory_order_release);
            notifyStateChanged();
            return;
        }

        case State::Sweep:
        {
            if (! serviceEmittingPhase())
                return;
            if (clock_.nowMs() < phaseEndsAtMs_)
                return;

            phaseEndsAtMs_ += kTailSeconds * 1000.0;
            state_.store (State::Tail, std::memory_order_release);
            notifyStateChanged();
            return;
        }

        case State::Tail:
        {
            if (! serviceEmittingPhase())
                return;
            if (clock_.nowMs() < phaseEndsAtMs_)
                return;

            state_.store (State::Analyse, std::memory_order_release);
            notifyStateChanged();
            analyseCurrentTarget();
            enterGap();
            return;
        }

        case State::Analyse:
            // Analyse never survives a poll boundary: Tail enters it, runs it
            // and leaves for Gap inside one runOnce(). Reaching it here would
            // mean an earlier poll returned mid-analysis.
            analyseCurrentTarget();
            enterGap();
            return;

        case State::Gap:
        {
            // The taps are STILL suspended here (inv 10) and the device checks
            // still apply, so the gap uses the same service step.
            if (! serviceEmittingPhase())
                return;
            if (clock_.nowMs() < phaseEndsAtMs_)
                return;

            const int next = targetIndex_.load (std::memory_order_relaxed) + 1;
            if (next < targetCount_.load (std::memory_order_relaxed))
                enterTarget (next);
            else
                finishRun();
            return;
        }

        case State::Results:
        {
            if (stopRequested_.exchange (false, std::memory_order_acq_rel))
            {
                // Nothing is emitting and detection is already back on, so this
                // is a plain dismissal that happens to carry a reason.
                logAbort (stopReason_.load (std::memory_order_relaxed));
                state_.store (State::Idle, std::memory_order_release);
                notifyStateChanged();
                return;
            }
            if (clock_.nowMs() >= phaseEndsAtMs_)
            {
                state_.store (State::Idle, std::memory_order_release);
                notifyStateChanged();
            }
            return;
        }
    }
}

bool SoundcheckController::serviceEmittingPhase()
{
    // FIRST, always: whatever the outcome, the ring must not be left to
    // overflow while the checks below run.
    drainCapture();

    if (stopRequested_.load (std::memory_order_acquire))
    {
        beginAbort (stopReason_.load (std::memory_order_relaxed));
        return false;
    }
    if (! engine_.isRunning())
    {
        beginAbort (AbortReason::EngineStopped);
        return false;
    }
    if (engine_.getLastDeviceError().isNotEmpty())
    {
        beginAbort (AbortReason::DeviceError);
        return false;
    }
    // inv 20, F15. A rate change invalidates T, the reference spectrum X and
    // every bin-to-Hz mapping at once, and getLastDeviceError() reports NOTHING
    // in that case.
    if (! checkDeviceUnchanged())
    {
        beginAbort (AbortReason::DeviceChanged);
        return false;
    }
    // F20. A drop splices sample N onto N+k, which through the Hann window is
    // broadband energy in EVERY bin: the measurement is worthless and must not
    // be reported as a measurement.
    if (engine_.getMicCaptureDropCount() != dropsAtArm_)
    {
        beginAbort (AbortReason::CaptureDrop);
        return false;
    }
    if (micIsHot())
    {
        beginAbort (AbortReason::MicHot);
        return false;
    }
    return true;
}

bool SoundcheckController::checkDeviceUnchanged() const
{
    if (std::abs (engine_.getCurrentSampleRateHz() - params_.sampleRate) > 1.0e-6)
        return false;
    if (engine_.getNumInputChannels()  != params_.numInputChannels)
        return false;
    if (engine_.getNumOutputChannels() != params_.numOutputChannels)
        return false;
    return true;
}

bool SoundcheckController::micIsHot()
{
    // Q3. Computed HERE, on the lane M thread, from the raw capture ring --
    // never from anything of the detector's, which is disarmed for the whole
    // run and could not answer.
    //
    // A poll that drained nothing carries no information: the hold timer is
    // neither armed nor cleared, or a device that delivers late would look
    // like a mic that had gone quiet.
    if (micCount_ == 0)
        return false;

    const double rms = std::sqrt (micSumSq_ / (double) micCount_);
    const double threshold = std::pow (10.0, kMicAbortDbfs / 20.0);   // -6 dBFS = 0.501
    const double now = clock_.nowMs();

    if (rms < threshold)
    {
        micHotSinceMs_ = -1.0;
        return false;
    }

    // THE HOLD. One block over the threshold is a transient -- a dropped mic, a
    // cough, a snare -- and killing a 72 s run on one of those is its own
    // failure. Only kMicAbortHoldMs of continuous overs is a hot mic.
    if (micHotSinceMs_ < 0.0)
    {
        micHotSinceMs_ = now;
        return false;
    }
    return (now - micHotSinceMs_) >= kMicAbortHoldMs;
}

void SoundcheckController::drainCapture()
{
    micSumSq_ = 0.0;
    micCount_ = 0;

    auto& ring = engine_.getMicCaptureBuffer();
    for (;;)
    {
        const std::size_t n = ring.read (capture_.data(), capture_.size());
        if (n == 0)
            break;

        for (std::size_t i = 0; i < n; ++i)
        {
            const double v = (double) capture_[i];
            micSumSq_ += v * v;
        }
        micCount_ += n;

        // The NOISE FLOOR and the CAPTURE are one continuous stream split by a
        // sample count, not by a flag: capture goes live at the same instant
        // scSampleIndex_ is set to -noiseFloorSamples_, so sample j of this
        // target's capture is sweep index (j - noiseFloorSamples_). One flag
        // fewer is one flag that cannot disagree with the index.
        std::size_t consumed = 0;
        if (capturedSamples_ < noiseFloorSamples_)
        {
            const std::size_t take = (std::size_t) std::min (
                (std::int64_t) n, noiseFloorSamples_ - capturedSamples_);
            estimator_.pushNoiseFloor (capture_.data(), (int) take);

            const std::size_t room = noiseWindow_.size() - noiseWindowFill_;
            const std::size_t copy = std::min (room, take);
            if (copy > 0)
            {
                std::memcpy (noiseWindow_.data() + noiseWindowFill_,
                             capture_.data(), copy * sizeof (float));
                noiseWindowFill_ += copy;
            }
            consumed = take;
        }
        if (consumed < n)
            estimator_.pushCapture (capture_.data() + consumed, (int) (n - consumed));

        capturedSamples_ += (std::int64_t) n;

        if (n < capture_.size())
            break;
    }
}

void SoundcheckController::computeNoiseSpectrum()
{
    noisePower_.fill (0.0);
    int frames = 0;

    const std::size_t have = noiseWindowFill_;
    for (std::size_t start = 0;
         start + (std::size_t) Detector::kFftSize <= have;
         start += (std::size_t) Detector::kHopSize)
    {
        std::memcpy (fftScratch_.data(), noiseWindow_.data() + start,
                     (std::size_t) Detector::kFftSize * sizeof (float));
        std::fill (fftScratch_.begin() + Detector::kFftSize, fftScratch_.end(), 0.0f);

        hann_.multiplyWithWindowingTable (fftScratch_.data(),
                                          (std::size_t) Detector::kFftSize);
        fft_.performFrequencyOnlyForwardTransform (fftScratch_.data(), true);

        for (int k = 0; k < LoopGainEstimator::kNumBins; ++k)
        {
            const double mag = (double) fftScratch_[(std::size_t) k];
            noisePower_[(std::size_t) k] += mag * mag;
        }
        ++frames;
    }

    if (frames == 0)
    {
        noiseMagnitudes_.fill (0.0f);
        worstPeakiness_.store (0.0f, std::memory_order_relaxed);
        return;
    }

    // The MEAN power spectrum of the window, not one frame of it. The question
    // the gate asks is "is this room RINGING", and a ring is by definition
    // sustained: it survives averaging, while a one-frame noise excursion does
    // not. The 7.35 single-frame figure (PeakinessAnalyzer.h:60-65) is
    // therefore an upper bound on what a quiet room can read here, and dense
    // continuous sampling crossing 10.0 once at 13.99
    // (memory/peakiness-sweep-2048-2026-09-04.md) is exactly the excursion this
    // average removes.
    for (int k = 0; k < LoopGainEstimator::kNumBins; ++k)
        noiseMagnitudes_[(std::size_t) k] =
            (float) std::sqrt (noisePower_[(std::size_t) k] / (double) frames);

    // analyse() hides the distribution below its own threshold, so the maximum
    // is taken directly from peakinessAt, bin by bin
    // (memory/peakiness-sweep-2048-2026-09-04.md).
    float worst = 0.0f;
    for (int k = 0; k < LoopGainEstimator::kNumBins; ++k)
        worst = std::max (worst, PeakinessAnalyzer::peakinessAt (
                                     noiseMagnitudes_.data(),
                                     LoopGainEstimator::kNumBins, k));
    worstPeakiness_.store (worst, std::memory_order_relaxed);
}

bool SoundcheckController::noiseWindowIsRinging() const
{
    // Both sides are PEAKINESS RATIOS, unbounded above: peakinessAt returns a
    // ratio (measured on the rig: worst noise bin 7.35, a 1 kHz tone 131.70 --
    // PeakinessAnalyzer.h:60-65), and params_.noiseFloorGate is the detector's
    // OWN live peakiness threshold, read at Arm. It is NEVER kConfirmScore,
    // which is the threshold of a 0..1 product -- that comparison aborts every
    // run in every room (N1, and memory/ring-risk-lane-r-2026-09-06.md).
    return worstPeakiness_.load (std::memory_order_relaxed) >= params_.noiseFloorGate;
}

void SoundcheckController::enterTarget (int index)
{
    Target t {};
    {
        const std::lock_guard<std::mutex> lock (stateMutex_);
        if (index < 0 || index >= (int) targets_.size())
            return;
        t = targets_[(std::size_t) index];
    }
    targetIndex_.store (index, std::memory_order_relaxed);

    estimator_.reset (params_.sampleRate);
    // X, the reference: the EXACT sequence the callback will emit, regenerated
    // from the same SoundcheckSignal params. Pushed once per target because
    // reset() clears every stream.
    estimator_.pushReference (reference_.data(), (int) reference_.size());

    capturedSamples_ = 0;
    noiseWindowFill_ = 0;
    micHotSinceMs_   = -1.0;

    // Whatever the previous target left behind is not this target's noise
    // floor. Safe as a consumer-side operation: capture is OFF at this point
    // (Gap turned it off, or the run has not started), so nothing is writing.
    engine_.getMicCaptureBuffer().clear();

    engine_.setSoundcheckCaptureChannel (t.inChannel);
    engine_.setSoundcheckSampleIndex (-noiseFloorSamples_);
    engine_.setSoundcheckCaptureActive (true);
    // LAST, and it is the release store the callback acquires (N-2). It also
    // discards any stale ramp-out anchor, which is why it must not be called
    // mid-ramp anywhere else (C-2).
    engine_.setSoundcheckOutputChannel (t.outChannel);

    // kNoiseFloorMs, NOT kNoiseFloorMs + kNoiseFloorGuardMs: the whole point
    // of the guard is that the GATE decides one guard BEFORE the index reaches
    // 0. Adding it here too would put the decision back level with the first
    // sample, which is the bug this constant exists to prevent.
    phaseEndsAtMs_ = clock_.nowMs() + kNoiseFloorMs;
    state_.store (State::NoiseFloor, std::memory_order_release);
    notifyStateChanged();
}

void SoundcheckController::enterGap()
{
    // The sweep and its tail are over, so the signal is already zero
    // (sampleAt returns 0 past totalSamples): releasing the channel here is
    // silent, and it un-mutes every lane routed to it for the gap.
    engine_.setSoundcheckCaptureActive (false);
    engine_.setSoundcheckCaptureChannel (-1);
    engine_.setSoundcheckOutputChannel (-1);
    // scSuspendTaps_ STAYS true (inv 10).

    phaseEndsAtMs_ += kGapMs;
    state_.store (State::Gap, std::memory_order_release);
    notifyStateChanged();
}

void SoundcheckController::analyseCurrentTarget()
{
    drainCapture();

    Target t {};
    {
        const std::lock_guard<std::mutex> lock (stateMutex_);
        const int index = targetIndex_.load (std::memory_order_relaxed);
        if (index < 0 || index >= (int) targets_.size())
            return;
        t = targets_[(std::size_t) index];
    }

    const auto est = estimator_.finish();

    SoundcheckCandidates::Ladder ladder;
    ladder.rungsDb    = NotchController::kDepthLadderDb;
    ladder.count      = NotchController::kDepthLadderSize;
    ladder.maxDepthDb = NotchController::kMaxDepthDb;

    SoundcheckCandidates::Input in;
    in.hDb        = est.hDb.data();
    // Passed VERBATIM: finish() has already cleared the whole array when the
    // run was unusable, and `measured` OVERRIDES `trusted`. Rebuilding it from
    // per-bin SNR reintroduces the hole Task 2's review closed.
    in.trusted    = est.trusted.data();
    in.sampleRate = params_.sampleRate;
    in.ceilingDb  = params_.ceilingDb;      // never left at the NaN default
    in.notchQ     = params_.notchQ;
    // inv 15 (a proposal must not land on a bin a notch already holds) cannot
    // be enforced here: reading the live notch list means calling
    // NotchController, which this thread may not do (inv 17). It is enforced at
    // APPLY time instead, on the message thread, by Task 7's
    // applySoundcheckResults -- which has the controller and the live model.
    in.liveNotchHz    = nullptr;
    in.liveNotchCount = 0;
    in.ladder         = ladder;

    const auto pick = SoundcheckCandidates::pick (in);

    OutputResult r;
    r.slot = t.slot; r.lane = t.lane; r.outChannel = t.outChannel; r.inChannel = t.inChannel;
    r.measured = est.measured;
    r.snrDb    = est.bandSnrDb;
    r.trusted  = est.trusted;
    r.marked   = pick.marked;                       // I-3: the PER-BIN flags
    // F26: a channel whose routing went out of range mid-run is a ROUTING
    // fault, not a quiet room. (A pair that was invalid at the start never gets
    // here -- preflight refuses it.)
    r.routingInvalid = (t.outChannel < 0 || t.outChannel >= params_.numOutputChannels
                        || t.inChannel < 0 || t.inChannel >= params_.numInputChannels);
    // Neither of these is "the room is clean" -- both say the caller handed
    // pick() something it could not propose from (Task 3 I-3).
    r.ceilingMissing = pick.ceilingMissing;
    r.ladderMissing  = (ladder.rungsDb == nullptr || ladder.count <= 0);
    for (int k = 0; k < LoopGainEstimator::kNumBins; ++k)
        r.marginDb[(std::size_t) k] = -est.hDb[(std::size_t) k];   // margin == -H_dB, ONE place
    r.markedCount    = pick.markedCount;
    r.candidateCount = pick.candidateCount;
    r.saturatedBins  = pick.saturatedBins;
    // m-22: SoundcheckCandidates::Candidate and OutputResult::Candidate are two
    // identical-looking types in two layers and nothing converts them.
    for (int c = 0; c < pick.candidateCount && c < kMaxPreventivePerLane; ++c)
    {
        const auto& src = pick.candidates[(std::size_t) c];
        auto&       dst = r.candidates[(std::size_t) c];
        dst.hz = src.hz; dst.marginDb = src.marginDb; dst.depthDb = src.depthDb;
        dst.q = src.q; dst.residualDb = src.residualDb; dst.bin = src.bin;
    }

    if (logEvent)
    {
        auto ev = SessionLogger::makeEvent ("soundcheck_output");
        if (auto* o = objectOf (ev))
        {
            o->setProperty ("slot", r.slot);
            o->setProperty ("lane", r.lane);
            o->setProperty ("out_ch", r.outChannel);
            o->setProperty ("in_ch", r.inChannel);
            o->setProperty ("ok", r.measured);
            o->setProperty ("routing_invalid", r.routingInvalid);
            o->setProperty ("snr_db", round3sf ((double) r.snrDb));
            o->setProperty ("saturated", r.saturatedBins > 0);
            o->setProperty ("marked", r.markedCount);
            o->setProperty ("ceiling_missing", r.ceilingMissing);
            o->setProperty ("ladder_missing", r.ladderMissing);

            juce::Array<juce::var> cands;
            for (int c = 0; c < r.candidateCount; ++c)
            {
                auto* co = new juce::DynamicObject();
                const auto& src = r.candidates[(std::size_t) c];
                co->setProperty ("hz", round3sf ((double) src.hz));
                co->setProperty ("margin_db", round3sf ((double) src.marginDb));
                co->setProperty ("depth_db", round3sf ((double) src.depthDb));
                co->setProperty ("residual_db", round3sf ((double) src.residualDb));
                cands.add (juce::var (co));
            }
            o->setProperty ("candidates", cands);
        }
        logEvent (ev);
    }

    const std::lock_guard<std::mutex> lock (stateMutex_);
    results_.push_back (r);
}

void SoundcheckController::finishRun()
{
    // THE ORDER IS THE INVARIANT (inv 12). Detection must be back on BEFORE the
    // state changes, or a GUI polling getState() sees Results while the
    // detector is still disarmed -- and lane G's release ladder would walk a
    // -24 notch up two rungs during the 20 s results window while spec §3
    // declares 0 dB.
    engine_.setSoundcheckCaptureActive (false);
    engine_.setSoundcheckCaptureChannel (-1);
    engine_.setSoundcheckOutputChannel (-1);
    engine_.setSoundcheckTapsSuspended (false);
    if (setDetectionActiveOnAllSlots)
        setDetectionActiveOnAllSlots (true);

    if (logEvent)
    {
        int ok = 0, failed = 0, marked = 0, candidates = 0;
        {
            const std::lock_guard<std::mutex> lock (stateMutex_);
            for (const auto& r : results_)
            {
                if (r.measured) ++ok; else ++failed;
                marked     += r.markedCount;
                candidates += r.candidateCount;
            }
        }
        auto ev = SessionLogger::makeEvent ("soundcheck_result");
        if (auto* o = objectOf (ev))
        {
            o->setProperty ("outputs_ok", ok);
            o->setProperty ("outputs_failed", failed);
            o->setProperty ("marked_total", marked);
            o->setProperty ("candidates_total", candidates);
        }
        logEvent (ev);
    }

    phaseEndsAtMs_ = clock_.nowMs() + kResultsTimeoutMs;
    state_.store (State::Results, std::memory_order_release);
    notifyStateChanged();
}

void SoundcheckController::stopEmissionSafely()
{
    // Nothing armed: no request to leave behind. requestSoundcheckRampOut()
    // with the channel already released would set a flag only an EMITTING
    // callback can clear, and after an abort there is no emitting callback.
    if (engine_.getSoundcheckOutputChannel() < 0)
        return;

    if (engine_.getSoundcheckSampleIndex() >= 0)
    {
        // Sound may be leaving right now: the callback owns the stop from
        // here. It latches the anchor out of its own snapshot (so the envelope
        // opens at exactly 1.0 at any buffer size), generates the ramp and
        // releases the channel when it reaches zero. No other thread has to
        // still be alive (inv 9). The channel is deliberately NOT touched --
        // setSoundcheckOutputChannel() would discard the pending ramp-out and
        // turn the fade into a hard cut (C-2).
        engine_.requestSoundcheckRampOut();
        return;
    }

    // NOISE FLOOR: scSampleIndex_ < 0, so sampleAt() is returning 0 and there
    // is nothing to fade. A ramp-out request here would be latched with a
    // NEGATIVE anchor, which reads as the "no anchor" sentinel, and the abort
    // would sit unhonoured until the index reached 0 -- i.e. until the sweep
    // was about to start (tests/test_audioengine.cpp
    // AbortDuringTheNoiseFloorIsDeferredUntilTheSweepStarts). THE BACKSTOP is
    // one store that puts the soundcheck side of the engine back to idle, and
    // it is silent precisely because nothing is being emitted.
    engine_.setSoundcheckOutputChannel (-1);
}

void SoundcheckController::beginAbort (AbortReason reason)
{
    state_.store (State::Abort, std::memory_order_release);
    stopRequested_.store (false, std::memory_order_relaxed);

    // THE SOUND FIRST, and it never waits for the ramp: the callback finishes
    // it alone (inv 9).
    stopEmissionSafely();

    engine_.setSoundcheckCaptureActive (false);
    engine_.setSoundcheckCaptureChannel (-1);
    engine_.setSoundcheckTapsSuspended (false);
    if (setDetectionActiveOnAllSlots)
        setDetectionActiveOnAllSlots (true);

    logAbort (reason);

    state_.store (State::Idle, std::memory_order_release);
    notifyStateChanged();
}

void SoundcheckController::logAbort (AbortReason reason) const
{
    if (! logEvent)
        return;

    auto ev = SessionLogger::makeEvent ("soundcheck_abort");
    if (auto* o = objectOf (ev))
    {
        o->setProperty ("reason", abortReasonNameForTest (reason));
        o->setProperty ("at_output", targetIndex_.load (std::memory_order_relaxed));
        o->setProperty ("elapsed_ms", round3sf (getElapsedMsInRun()));
    }
    logEvent (ev);
}

void SoundcheckController::notifyStateChanged() const
{
    if (onStateChanged)
        onStateChanged();
}
