// src/app/SoundcheckController.cpp
//
// See the header for the thread map, the inv-17 argument and why RunParams is
// frozen. This file is the state machine itself.
#include "app/SoundcheckController.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

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
        case AbortReason::NoiseFloorUnmeasured: return "noise_floor_unmeasured";
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

// Declaration order in the owner is load-bearing here -- this destructor stands
// a live run down through engine_, clock_ AND all three injected lambdas. The
// contract is stated where a caller will actually read it, on the lambda
// members in SoundcheckController.h (I-10/N-2/C-3).

SoundcheckController::~SoundcheckController()
{
    // The lambdas are invoked from this thread; they must not be destroyed
    // (this object's members die right after) while a poll is in flight.
    stop (2000);
}

// ---------------------------------------------------------------- MESSAGE ---

// Truly const, and it caches nothing (S-1): arm() reads its own snapshot, so
// there is no stale number for soundcheck_start to log.
SoundcheckController::Refusal
SoundcheckController::preflight (const std::vector<Target>& targets,
                                 const NotchController::SnapshotBuffer& riskSnapshot) const
{
    const Refusal targetRefusal = validateTargets (targets);
    if (targetRefusal != Refusal::None)
        return targetRefusal;

    return refuseOnRingRisk (riskSnapshot);
}

// Shared by preflight() and arm() so the two cannot drift: everything the
// dialog checked, checked again at the moment the sweep is committed (I-4).
SoundcheckController::Refusal
SoundcheckController::validateTargets (const std::vector<Target>& targets) const
{
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

    return Refusal::None;
}

SoundcheckController::Refusal
SoundcheckController::refuseOnRingRisk (const NotchController::SnapshotBuffer& riskSnapshot) const
{
    // R3-2. BOTH sides are 0..1 products here: ringRiskScore is the same
    // `score` the placement decision compares against kConfirmScore, and
    // ringRiskThreshold IS kConfirmScore. The product is taken from the
    // snapshot rather than written as 0.385 so the GUI RISING band and this
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

SoundcheckController::Refusal
SoundcheckController::arm (std::vector<Target> targets, const RunParams& params,
                           const NotchController::SnapshotBuffer& riskSnapshot)
{
    if (state_.load (std::memory_order_acquire) != State::Idle)
        return Refusal::AlreadyRunning;

    // Everything below is a CALLER BUG, not a room. Refusing here is what
    // stops a bug from becoming either a wrong cut or a plausible-looking
    // "room clean" that nobody questions.
    //
    // Task 3 I-3: a non-finite ceiling reaches SoundcheckCandidates as "unset"
    // and produces marks with no proposals -- never distinguishable from a
    // quiet room downstream. N1: the gate is a PEAKINESS RATIO, and zero is not
    // a gate, it is a field nobody filled in, which would abort every run in
    // every room.
    if (! (params.sampleRate > 0.0)
        || ! std::isfinite (params.ceilingDb)
        || ! (params.noiseFloorGate > 0.0f)
        || params.numInputChannels <= 0 || params.numOutputChannels <= 0)
        return Refusal::InvalidParams;

    // I-4: re-validated HERE, not just in preflight. preflight() ran before the
    // Confirm dialog; a slot can be disabled, or re-routed, while the operator
    // is reading it.
    //
    // ORDER: this comes BEFORE the fade check. "The device is gone" outranks
    // "a fade from the last run is still finishing" -- a stopped engine cannot
    // finish a fade at all, so reporting RampOutPending there would send Task 9
    // to tell the operator to wait for something that will never happen.
    const Refusal targetRefusal = validateTargets (targets);
    if (targetRefusal != Refusal::None)
        return targetRefusal;

    // A fade from the LAST run is still in the air. Arming calls
    // setSoundcheckOutputChannel(), which discards a pending ramp-out (C-2) --
    // so a fast re-arm would turn the previous abort into a hard cut, which is
    // the click the ramp exists to prevent. Wait for the callback to finish it.
    if (engine_.soundcheckIsEmitting() || engine_.isSoundcheckRampOutPending())
        return Refusal::RampOutPending;

    // S-1: and so can the room start ringing. This is the LAST read of a live
    // snapshot before the taps are suspended and ring risk stops updating at
    // all, so it is the one that matters -- and the one soundcheck_start logs.
    const Refusal riskRefusal = refuseOnRingRisk (riskSnapshot);
    if (riskRefusal != Refusal::None)
        return riskRefusal;

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
    sweepLeadInSamples_ = (std::int64_t) std::llround (
        kSweepLeadInMs * params_.sampleRate / 1000.0);
    // 100 ms of slack past kNoiseFloorMs so a poll that arrives late still has
    // a full window to score rather than a truncated one.
    const std::int64_t windowSamples = (std::int64_t) std::llround (
        (kNoiseFloorMs + 100.0) * params_.sampleRate / 1000.0);
    noiseWindow_.assign ((std::size_t) std::max<std::int64_t> (windowSamples, 1), 0.0f);
    capture_.assign (8192, 0.0f);

    SoundcheckSignal::Params sig;
    sig.sampleRate = params_.sampleRate;
    sig.peak       = params_.peak;
    const SoundcheckSignal signal { sig };
    reference_.assign ((std::size_t) std::max<std::int64_t> (signal.totalSamples(), 1), 0.0f);
    for (std::int64_t n = 0; n < signal.totalSamples(); ++n)
        reference_[(std::size_t) n] = signal.sampleAt (n);
    sweepTotalSamples_ = signal.totalSamples();

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
                            riskSnapshot.ringRiskValid
                                ? juce::var (round3sf ((double) riskSnapshot.ringRiskScore))
                                : juce::var());
            o->setProperty ("gate", round3sf ((double) params_.noiseFloorGate));
        }
        logEvent (ev);
    }

    enterTarget (0);
    return Refusal::None;
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

    // I-5: and so do the two engine gates, because both moves are in the SAFE
    // direction and neither needs a poll to be correct. Capture off wastes
    // nothing -- the run is over. Taps lifted gives the detector its signal
    // back; leaving them suspended until some later poll is the one direction
    // that can hurt, since a suspended tap is a deaf feedback killer.
    //
    // Detection itself is NOT re-armed here: setDetectionActiveOnAllSlots is
    // contracted to the lane M thread (I-10), and beginAbort calls it there on
    // the very next runOnce().
    engine_.setSoundcheckCaptureActive (false);
    engine_.setSoundcheckTapsSuspended (false);
}

bool SoundcheckController::abortAndJoin()
{
    // The device-restart path (Task 10). audioDeviceAboutToStart() clears the
    // soundcheck atomics, which is only safe because this has returned first.
    //
    // ORDER (C-2): request, JOIN, then run. Running the abort before the join
    // puts two threads inside one state machine -- and the poll thread, if it
    // happens to be in Gap, calls enterTarget() straight after the caller has
    // torn the run down, arming a whole fresh sweep with the taps still
    // suspended and detection off. The join is what makes the caller the only
    // thread left.
    requestStop (AbortReason::DeviceChanged);
    stop (1000);        // joins, and stands the run down with THAT reason
    runOnce();          // belt and braces on the caller thread; a no-op when Idle

    const bool idle = state_.load (std::memory_order_acquire) == State::Idle;
    jassert (idle);
    return idle;
}

void SoundcheckController::start()
{
    startThread();
}

void SoundcheckController::stop (int timeoutMs)
{
    // C-3. Stopping the THREAD is not stopping the RUN. Without this, a stop()
    // (or a destructor) mid-sweep leaves scOutChannel_ armed, the taps
    // suspended and detection off, with the only thread that could undo any of
    // it already gone: the PA keeps the sweep until the callback runs out of
    // signal, and the feedback killer never wakes up again.
    if (state_.load (std::memory_order_acquire) != State::Idle
        && ! stopRequested_.load (std::memory_order_acquire))
        requestStop (AbortReason::UserStop);

    stopThread (timeoutMs);

    // The poll thread is gone; finish the abort here, synchronously.
    if (state_.load (std::memory_order_acquire) != State::Idle)
        beginAbort (stopReason_.load (std::memory_order_relaxed));
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

            // THE GATE. Nothing is armed yet -- scOutChannel_ is still -1 and
            // no audio clock is running toward zero (C-1) -- so however late
            // this poll is, the room cannot have been swept before the decision.
            //
            // FAIL CLOSED. No frames means no noise floor was measured at all
            // (a device that stopped delivering, a capture channel that never
            // produced), and an unmeasured floor is not a quiet one.
            if (! computeNoiseSpectrum())
            {
                beginAbort (AbortReason::NoiseFloorUnmeasured);
                return;
            }
            if (noiseWindowIsRinging())
            {
                beginAbort (AbortReason::RoomRinging);
                return;
            }

            // ONLY NOW does the sweep exist.
            armSweepForCurrentTarget();
            // RE-STAMPED FROM NOW, not accumulated from the noise-floor
            // deadline (N-1). After C-1 the audio is anchored to THIS INSTANT:
            // the callback starts counting when the channel is published here,
            // so a deadline carried forward from enterTarget() is short by
            // however late this poll was. At L < 700 ms that silently
            // under-measures the tail; past 700 ms the Gap arrives while the
            // sweep is still at full amplitude.
            //
            // Tail and Gap keep accumulating from this value, which is correct
            // precisely because it is now anchored to the audio.
            phaseEndsAtMs_ = clock_.nowMs() + kSweepLeadInMs + kSweepSeconds * 1000.0;
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
        // sample count. The split point is NOT derived from the sweep index
        // (C-1): during the noise floor there is no sweep and no index at all.
        // noiseFloorEndSample_ is INT64_MAX until armSweepForCurrentTarget()
        // stamps it with capturedSamples_, so everything drained before the
        // gate said quiet is N and everything after it is Y, with no flag that
        // could disagree with a clock.
        std::size_t consumed = 0;
        if (capturedSamples_ < noiseFloorEndSample_)
        {
            const std::size_t take = (std::size_t) std::min (
                (std::int64_t) n, noiseFloorEndSample_ - capturedSamples_);
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

bool SoundcheckController::computeNoiseSpectrum()
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
        // NOT a quiet room -- no room at all. The caller FAILS CLOSED on this
        // (AbortReason::NoiseFloorUnmeasured) rather than sweeping a room it
        // never listened to.
        noiseMagnitudes_.fill (0.0f);
        worstPeakiness_.store (0.0f, std::memory_order_relaxed);
        return false;
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
    //
    // ONLY INSIDE THE BAND THIS RUN IS ABOUT (I-2). A max over all 1025 bins
    // makes the gate answer a question nobody asked: an LED driver whining at
    // 17 kHz, a switch-mode supply, or 50 Hz mains and its harmonics are all
    // narrow, all permanent, and all OUTSIDE [kSweepLowHz, kTrustedHighHz] --
    // so they would refuse every run in the venue while saying nothing about
    // whether the room rings where the sweep is going to measure it.
    const int loBin = std::max (0, LoopGainEstimator::hzToBin (kSweepLowHz,
                                                               params_.sampleRate));
    const int hiBin = std::min (LoopGainEstimator::kNumBins - 1,
                                LoopGainEstimator::hzToBin (kTrustedHighHz,
                                                            params_.sampleRate));
    float worst = 0.0f;
    for (int k = loBin; k <= hiBin; ++k)
        worst = std::max (worst, PeakinessAnalyzer::peakinessAt (
                                     noiseMagnitudes_.data(),
                                     LoopGainEstimator::kNumBins, k));
    worstPeakiness_.store (worst, std::memory_order_relaxed);
    return true;
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

    capturedSamples_     = 0;
    noiseWindowFill_     = 0;
    micHotSinceMs_       = -1.0;
    // Everything captured is the noise floor until the gate says otherwise.
    noiseFloorEndSample_ = std::numeric_limits<std::int64_t>::max();

    // Whatever the previous target left behind is not this target's noise
    // floor. Safe as a consumer-side operation: capture is OFF at this point
    // (Gap turned it off, or the run has not started), so nothing is writing.
    engine_.getMicCaptureBuffer().clear();

    // LISTEN ONLY (C-1). The capture gate opens; the OUTPUT CHANNEL DOES NOT.
    // scOutChannel_ stays -1 through the whole noise floor, so there is no
    // audio clock counting toward the first sample and a poll that arrives
    // late -- a loaded message thread, a long GC-like stall, a debugger --
    // cannot lose a race it is not in. The sweep is armed only by
    // armSweepForCurrentTarget(), only after the gate has said quiet.
    engine_.setSoundcheckCaptureChannel (t.inChannel);
    engine_.setSoundcheckCaptureActive (true);

    phaseEndsAtMs_ = clock_.nowMs() + kNoiseFloorMs;
    state_.store (State::NoiseFloor, std::memory_order_release);
    notifyStateChanged();
}

void SoundcheckController::armSweepForCurrentTarget()
{
    Target t {};
    {
        const std::lock_guard<std::mutex> lock (stateMutex_);
        const int index = targetIndex_.load (std::memory_order_relaxed);
        // Unreachable: the gate is only entered from a target this machine put
        // itself on. It is asserted rather than silently skipped because the
        // silent version arms NO sweep, and the run would then sit through a
        // whole Sweep + Tail measuring a channel it never drove and report the
        // result as a room that could not be measured.
        jassert (index >= 0 && index < (int) targets_.size());
        if (index < 0 || index >= (int) targets_.size())
            return;
        t = targets_[(std::size_t) index];
    }

    // Everything captured from here on is Y, the room reply to the sweep.
    noiseFloorEndSample_ = capturedSamples_;

    // ORDER IS THE CONTRACT (N-2). The index is a relaxed store and the channel
    // is a RELEASE store that the callback loads with ACQUIRE, so a callback
    // that sees this channel is guaranteed to see this index -- and the run
    // starts at -sweepLeadInSamples_, i.e. with a short silent run-up, so the
    // first sample it ever emits is index 0 at the foot of the ramp-in
    // whatever buffer size the device uses.
    //
    // setSoundcheckOutputChannel() also discards any stale ramp-out anchor,
    // which is why it must not be called mid-ramp anywhere else (C-2).
    engine_.setSoundcheckSampleIndex (-sweepLeadInSamples_);
    engine_.setSoundcheckOutputChannel (t.outChannel);
}

void SoundcheckController::releaseOutputChannelSafely()
{
    // BY CONSTRUCTION the signal is already zero here: Sweep + Tail is 3.7 s of
    // deadline against 3.0 s of sweep, and sampleAt() returns 0 past
    // totalSamples. Releasing a silent channel is silent.
    //
    // DEFENSIVE ANYWAY (N-1). "By construction" is exactly the kind of claim a
    // late poll breaks, and the failure mode is not a wrong number on a screen:
    // a bare store to scOutChannel_ while the callback still has a non-zero
    // sample to produce is a HARD CUT on a live PA, taken at whatever amplitude
    // the sweep happened to be at. If that is where we are, the fade is handed
    // to the callback exactly as an abort would hand it over (inv 9) and the
    // callback releases the channel itself when the envelope reaches zero.
    const std::int64_t idx = engine_.getSoundcheckSampleIndex();
    if (engine_.soundcheckIsEmitting() && idx >= 0 && idx < sweepTotalSamples_)
    {
        engine_.requestSoundcheckRampOut();
        return;
    }

    engine_.setSoundcheckOutputChannel (-1);
}

void SoundcheckController::enterGap()
{
    engine_.setSoundcheckCaptureActive (false);
    engine_.setSoundcheckCaptureChannel (-1);
    releaseOutputChannelSafely();
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
    releaseOutputChannelSafely();
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

    // A DEAD CALLBACK CANNOT FADE ANYTHING (S-2/I-3). With the device stopped
    // or errored, requestSoundcheckRampOut() sets a flag no block will ever
    // read: the channel stays armed for ever, and the next device to open
    // inherits an armed soundcheck. There is also nothing to click -- no
    // callback is producing samples -- so the backstop is free here.
    if (! engine_.isRunning())
    {
        engine_.setSoundcheckOutputChannel (-1);
        return;
    }

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

//==============================================================================
// MESSAGE THREAD ONLY (spec 4.6e, invariant 17).
//
// setNotch/clearNotch are policy entry points declared message-thread
// (NotchController.h:225-230), and setNotchImpl additionally reads width_
// (NotchController.cpp:199) and the detector's sample rate (:209) OUTSIDE
// modelMutex_. This is a FREE FUNCTION rather than a SoundcheckController
// method so that no route exists by which the lane M thread could reach it:
// that class holds no NotchController pointer at all.
//==============================================================================
namespace
{
// B-1's allocator, keyed by LANE AND INDEX (M-2). Keying it by index alone
// over-reserves across lanes: an INDEP stereo run of six proposals per lane
// would burn twelve of the sixteen indices to place twelve notches that only
// ever needed six, because lane 0 and lane 1 have entirely separate chains.
//
// TOP-DOWN (15, 14, 13 ...) while the detector allocates bottom-up
// (firstFreeIndexLocked scans i = 0..kSlots, NotchController.cpp:1066-1069),
// so the two only meet when the chain is nearly full.
//
// `lane < 0` means "must be free on every DRIVEN lane" -- a LINKED pair, the
// shape firstFreeIndexAllLanesLocked (:1073-1084) produces on the detector
// side. It is bounded by laneCount rather than kChannels: on a mono slot lane
// 1 is never written and never marked, so scanning it would report every index
// free and hand 15 out twice.
//
// Only ACTIVE notches enter the snapshot (NotchController.cpp:577), so "not
// present in snap" is exactly "free".
int firstFreeIndexTopDown (const NotchController::SnapshotBuffer& snap,
                           const std::array<std::array<bool, NotchController::kSlots>,
                                            NotchController::kChannels>& takenThisCall,
                           int lane, int laneCount)
{
    for (int index = NotchController::kSlots - 1; index >= 0; --index)
    {
        // What THIS call has already handed out. The snapshot cannot know:
        // latest_ is republished only inside runOnce()'s drain loop on the
        // DETECTOR thread, about once per hop (~10.7 ms), while this loop runs
        // in microseconds on the message thread.
        bool free = true;
        if (lane < 0)
        {
            for (int l = 0; l < laneCount && free; ++l)
                free = ! takenThisCall[(std::size_t) l][(std::size_t) index];
        }
        else
        {
            free = ! takenThisCall[(std::size_t) lane][(std::size_t) index];
        }
        if (! free)
            continue;

        for (std::uint32_t i = 0; i < snap.notchCount && free; ++i)
        {
            const auto& n = snap.notches[i];
            if ((int) n.index != index)
                continue;
            if (lane < 0 ? (int) n.channel < laneCount : (int) n.channel == lane)
                free = false;
        }
        if (free)
            return index;
    }
    return -1;
}

// Where in a kTotalSlots-wide bitmap a (lane, index) pair lives. NotchController
// has its own slotOf(), but it is private and this file is not a friend; the
// layout only has to be self-consistent within one call.
constexpr std::size_t flatSlot (int lane, int index)
{
    return (std::size_t) (lane * NotchController::kSlots + index);
}

// Invariant 15. Two notches one bin apart are two filters doing one filter's
// job: the second buys almost no extra attenuation, it costs a slot out of
// sixteen that a different howl will need, and where the ceiling allows -24 dB
// a pair of them cascades to about -48 dB over one bin.
//
// The bin ruler is LoopGainEstimator's, which is the detector's
// (Detector::kFftSize, kNumBins) -- the same ruler SoundcheckCandidates would
// have used had Task 6 been able to pass it a live-notch list. It cannot
// (inv 17), so the test lives here.
//
// THREE populations are compared against, not one:
//   * the live notches in the snapshot;
//   * MINUS the ones THIS CALL has already cleared -- they are not live,
//     however long the snapshot goes on listing them (up to one hop). Without
//     this, a second soundcheck of the same room, which finds the same
//     frequencies, would refuse to re-place anything it had just removed;
//   * PLUS what THIS CALL has already placed (C-1). The re-read snapshot
//     cannot contain a notch written microseconds ago on the message thread,
//     and SoundcheckCandidates::pick has no mutual-separation step of its own,
//     so two adjacent-bin proposals from one run would otherwise both land.
//
// A snapshot with no published sample rate cannot be measured against: hzToBin
// answers 0 for everything at rate 0 and would collapse every proposal onto one
// bin. Refusing to compare is the safe direction -- the placement still goes
// through setNotchImpl, which validates the frequency against the detector's
// real rate.
bool withinOneBinOfALiveNotch (const NotchController::SnapshotBuffer& snap,
                               const std::array<bool, NotchController::kTotalSlots>& clearedThisCall,
                               const std::vector<SoundcheckApplyLedger::Entry>& placedThisCall,
                               int lane, double hz)
{
    if (! (snap.sampleRate > 0.0))
        return false;

    const int candidateBin = LoopGainEstimator::hzToBin (hz, snap.sampleRate);

    const auto adjacent = [candidateBin, &snap] (double otherHz)
    {
        return std::abs (LoopGainEstimator::hzToBin (otherHz, snap.sampleRate) - candidateBin) <= 1;
    };

    for (std::uint32_t i = 0; i < snap.notchCount; ++i)
    {
        const auto& n = snap.notches[i];
        if ((int) n.channel != lane)
            continue;
        if (clearedThisCall[flatSlot ((int) n.channel, (int) n.index)])
            continue;
        if (adjacent ((double) n.frequency))
            return true;
    }

    for (const auto& e : placedThisCall)
        if (e.lane == lane && adjacent ((double) e.hz))
            return true;

    return false;
}
} // namespace

SoundcheckApplyStats applySoundcheckResults (
    NotchController& controller,
    int slot,
    const std::vector<SoundcheckController::OutputResult>& results,
    SoundcheckApplyLedger& ledger)
{
    SoundcheckApplyStats stats;

    // THE THREAD RULE, ENFORCED RATHER THAN DOCUMENTED (inv 17, F27). A banner
    // comment cannot stop a future caller reaching this from the lane M thread,
    // and setNotchImpl's unlocked reads of width_ and the detector sample rate
    // make that a data race on the audio path -- which on this app means a
    // filter coefficient written while the callback reads it.
    //
    // NOT a jassert: a Debug-only trap fires long after the damage, and this is
    // a refusal the caller can see and report. When no MessageManager exists at
    // all (a headless unit test that never created one) there is no message
    // thread to be off, so the check does not apply. THE LEDGER IS NOT TOUCHED
    // on this path -- nothing was placed, so nothing it records has changed.
    if (juce::MessageManager::getInstanceWithoutCreating() != nullptr
        && ! juce::MessageManager::existsAndIsCurrentThread())
    {
        for (const auto& result : results)
            if (result.slot == slot && result.measured && ! result.routingInvalid)
                stats.refused += result.candidateCount;
        return stats;
    }

    // I-2: linkedness is the CONTROLLER's own predicate, read ONCE here on the
    // message thread. effectiveLinked() (NotchController.h:223) is
    // isLinked() || width_ < 2 || taps_[1] == nullptr, so it already covers
    // both the operator's switch and the cases where independence is
    // impossible. Deriving it from SnapshotBuffer::linked instead would read a
    // value republished only at hop cadence (NotchController.cpp:641), so an
    // operator who flipped LINK less than ~10.7 ms ago would have their first
    // proposal placed in the OTHER mode.
    const bool linkedNow = controller.effectiveLinked();

    // B-1: indices THIS CALL has handed out, per lane (M-2).
    std::array<std::array<bool, NotchController::kSlots>, NotchController::kChannels> takenThisCall {};
    // ...the notches this call has CLEARED, which the snapshot goes on listing
    // for the same reason, and what it has PLACED, which the snapshot cannot
    // know about yet. Both are read by the invariant-15 test.
    std::array<bool, NotchController::kTotalSlots> clearedThisCall {};
    std::vector<SoundcheckApplyLedger::Entry> placedThisCall;
    placedThisCall.reserve (ledger.entries.size() + 4);

    // The entry snapshot. laneCount comes from here and is read ONCE: it is
    // analysedLanes() (NotchController.h:667, published at .cpp:637), which only
    // moves on setWidth(), and setWidth() requires the detector thread stopped.
    NotchController::SnapshotBuffer entrySnap {};
    controller.copySnapshot (entrySnap);
    const int laneCount = juce::jlimit (1, NotchController::kChannels, (int) entrySnap.laneCount);

    // The two halves of "how many lanes, and together or apart?" come from
    // DIFFERENT clocks, deliberately: linkedNow is LIVE (the controller's own
    // predicate, read microseconds ago) while laneCount is SNAPSHOT-SOURCED and
    // may be up to one hop old. That asymmetry is safe only because laneCount
    // moves solely on setWidth(), which requires the detector thread stopped --
    // so it cannot change under this call the way the atomic LINK switch can.

    // --- (b) replace THIS SLOT's previous proposals, and ONLY those (C-2) ----
    // Origin::Soundcheck has two producers: this function, and the legacy 15 s
    // soundcheck MODE (placeConfirmed, NotchController.cpp:1093, turned on at
    // MainComponent.cpp:802-803). Clearing by origin alone would delete a notch
    // the operator locked in by hand, at the moment they pressed AP DUNG. So
    // the ledger -- what the PREVIOUS apply placed on this slot -- is the
    // authority, and an entry only counts if the notch still sitting at that
    // (lane, index) is still Soundcheck-origin and still within +-1 bin of the
    // frequency the ledger recorded. Anything else at that address is somebody
    // else's notch on a reused slot.
    std::vector<SoundcheckApplyLedger::Entry> carriedForward;

    for (const auto& e : ledger.entries)
    {
        bool matched = false;
        for (std::uint32_t i = 0; i < entrySnap.notchCount; ++i)
        {
            const auto& n = entrySnap.notches[i];
            if ((int) n.channel != e.lane || (int) n.index != e.index)
                continue;
            if (n.origin != NotchController::Origin::Soundcheck)
                break;
            // N-1(a): the address and the origin are the STRONG half of the
            // match; the frequency is the tie-breaker that catches a slot
            // reused by another soundcheck-origin notch. So when the rate is
            // unknown the tie-breaker is treated as PASSED rather than as
            // failed -- refusing to clear on a missing rate would strand a
            // preventive notch that never auto-releases (KD-7), which is the
            // very chain drain this ledger exists to prevent.
            //
            // Unreachable today and kept anyway: Detector::setSampleRate
            // refuses a non-positive rate (Detector.cpp:29-32), so a PUBLISHED
            // snapshot always carries a positive one, and an unpublished
            // snapshot carries no notches for the address match to find. There
            // is therefore no test behind this branch; see the task report.
            const bool rateKnown = entrySnap.sampleRate > 0.0;
            if (rateKnown
                && std::abs (LoopGainEstimator::hzToBin ((double) n.frequency, entrySnap.sampleRate)
                             - LoopGainEstimator::hzToBin ((double) e.hz, entrySnap.sampleRate)) > 1)
                break;

            controller.clearNotch (e.lane, e.index,
                                   NotchController::ClearReason::SoundcheckReplace);
            ++stats.clearedPrevious;
            clearedThisCall[flatSlot (e.lane, e.index)] = true;
            matched = true;

            // N-5: the INDEX is not marked free here, and that is deliberate.
            // takenThisCall starts all-false, so clearing it would be a no-op
            // anyway; and an index freed HERE is not reusable by THIS call,
            // because the snapshot each placement re-reads still lists the
            // cleared notch for up to ~10.7 ms. Conservative is the right side
            // to be on when the alternative is two writers on one index.
            break;
        }

        // N-1(b): an entry this pass could not match is NOT dropped. The
        // commonest reason it fails to match is that the snapshot is one hop
        // stale -- press AP DUNG twice inside ~10.7 ms and the first run's
        // notches are not in it yet. Dropping the entry there would orphan a
        // preventive notch that never auto-releases, with nothing left that
        // knows its address: the chain drains one soundcheck at a time. Carried
        // forward, a later apply reclaims it. If it is unmatched because the
        // notch really is gone (CLEAR ALL, a width change), carrying it costs
        // one struct and it simply never matches again.
        if (! matched)
            carriedForward.push_back (e);
    }

    for (const auto& result : results)
    {
        // I-1: a result belonging to another slot must never reach THIS slot's
        // chain. copyResultsForSlot() already filters, but this function is
        // also reachable with copyResults(), and a leak here places one slot's
        // proposals on another's filters.
        if (result.slot != slot)
        {
            ++stats.skippedOtherSlot;
            continue;
        }

        // result.lane is about to index takenThisCall and reach setNotch, and
        // it arrives from a Target the GUI built -- so it is checked here
        // rather than trusted. setNotch would itself refuse a lane >= width_
        // (NotchController.cpp:199), but takenThisCall is indexed BEFORE that,
        // and a lane 1 result on a mono slot is a routing fault worth naming
        // rather than a silent no-op.
        if (result.lane < 0 || result.lane >= laneCount)
        {
            ++stats.skippedBadLane;
            continue;
        }

        if (! result.measured || result.routingInvalid)
            continue;                        // a VALID result that places nothing (Q8, F26)

        for (int c = 0; c < result.candidateCount; ++c)
        {
            const auto& cand = result.candidates[(std::size_t) c];

            // (a) RE-READ before EVERY setNotch. DEFENCE IN DEPTH, not the
            // allocator (I-3): it is here to catch a DETECTOR placement that
            // landed since entry, and the detector runs on its own thread at
            // >= 300 ms placement cadence, so no test in this suite can force
            // that interleaving -- there is no harness hook for it. What makes
            // six proposals land on six indices is takenThisCall, which the
            // snapshot cannot know about at all.
            NotchController::SnapshotBuffer snap {};
            controller.copySnapshot (snap);

            // --- invariant 15, tested BEFORE an index is spent on it ---------
            bool sitsOnALiveNotch = false;
            if (linkedNow)
            {
                for (int lane = 0; lane < laneCount && ! sitsOnALiveNotch; ++lane)
                    sitsOnALiveNotch = withinOneBinOfALiveNotch (snap, clearedThisCall,
                                                                 placedThisCall, lane,
                                                                 (double) cand.hz);
            }
            else
            {
                sitsOnALiveNotch = withinOneBinOfALiveNotch (snap, clearedThisCall,
                                                             placedThisCall, result.lane,
                                                             (double) cand.hz);
            }

            if (sitsOnALiveNotch)
            {
                ++stats.skippedLive;
                ++stats.refused;   // nothing was placed for this candidate
                continue;          // SKIP, not stop: the next candidate is a
                                   // different frequency and may be fine.
            }

            const int index = firstFreeIndexTopDown (snap, takenThisCall,
                                                     linkedNow ? -1 : result.lane, laneCount);
            if (index < 0) { ++stats.refused; break; }       // the chain is full

            // M-5, lane G's M-B lesson: RunParams froze the depth at Arm, and
            // an operator can pull the depth slider up while the run or the
            // 20 s results window is on screen. Placing what was frozen would
            // then put a notch on a live PA deeper than the number the slider
            // is showing. Clamp to the slider at the moment of placement --
            // deeper is more negative, so std::max is the shallow direction,
            // and nothing here can ever make a proposal DEEPER than it asked
            // for.
            const double depth = std::max ((double) cand.depthDb, controller.getNotchDepthDb());

            if (linkedNow)
            {
                // ALL-OR-NOTHING (N4): one lane protected while the GUI claims
                // both is worse than placing nothing -- the same reasoning
                // placeConfirmed (NotchController.cpp:1265) and adoptPreset
                // (:530) already follow.
                int  placedLanes = 0;
                bool ok = true;
                for (int lane = 0; lane < laneCount && ok; ++lane)
                {
                    if (controller.setNotch (lane, index, cand.hz, cand.q, depth,
                                             NotchController::Origin::Soundcheck))
                    {
                        takenThisCall[(std::size_t) lane][(std::size_t) index] = true;
                        placedThisCall.push_back ({ lane, index, cand.hz });
                        ++placedLanes;
                    }
                    else
                    {
                        ok = false;
                    }
                }

                if (! ok)
                {
                    for (int lane = 0; lane < placedLanes; ++lane)
                        controller.clearNotch (lane, index,
                                               NotchController::ClearReason::PartialApplyUnwind);
                    // The unwound lanes are struck from BOTH records: the index
                    // stays reserved for this call (the snapshot cannot show it
                    // gone yet), but a frequency that is no longer on the chain
                    // must not go on blocking a later proposal, and it must not
                    // reach the ledger as if it had survived.
                    placedThisCall.resize (placedThisCall.size() - (std::size_t) placedLanes);
                    ++stats.refused;
                    break;                                   // stop this result
                }
                stats.placed += placedLanes;
            }
            else
            {
                if (! controller.setNotch (result.lane, index, cand.hz, cand.q, depth,
                                           NotchController::Origin::Soundcheck))
                {
                    // INDEP: STOP, do NOT roll back. A notch already placed is
                    // real protection, and pulling it because the NEXT one
                    // failed is strictly worse (N4, F10).
                    ++stats.refused;
                    break;
                }
                takenThisCall[(std::size_t) result.lane][(std::size_t) index] = true;
                placedThisCall.push_back ({ result.lane, index, cand.hz });
                ++stats.placed;
            }
        }
    }

    // N-1(b): the entries this call could not match join what it placed. They
    // are appended HERE, after the placement loop, and never earlier:
    // placedThisCall doubles as invariant 15's "already placed on this lane"
    // list, and a carried entry may name a notch that is no longer on the
    // chain at all -- letting it block a proposal would be refusing to protect
    // the room on the strength of a notch nobody can find.
    //
    // An entry whose (lane, index) THIS call has just reused is dropped: two
    // ledger rows for one address would have the next apply clear it twice and
    // report the second as a real removal.
    for (const auto& e : carriedForward)
    {
        bool reused = false;
        for (const auto& p : placedThisCall)
            reused = reused || (p.lane == e.lane && p.index == e.index);
        if (! reused)
            placedThisCall.push_back (e);
    }

    // C-2: the ledger is what THIS call placed, replacing what the last one
    // did. It is overwritten even when nothing was placed -- the previous
    // entries were cleared above, so remembering them would make the NEXT
    // apply try to clear notches that are already gone.
    ledger.entries = std::move (placedThisCall);
    return stats;
}
