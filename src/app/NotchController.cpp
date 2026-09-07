#include "app/NotchController.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

NotchController::NotchController (LockFreeRingBuffer<float>& tapLane0,
                                  LockFreeRingBuffer<float>* tapLane1,
                                  LockFreeRingBuffer<NotchCommand>& commands,
                                  ClockSource& clock, int slotId)
    : juce::Thread ("AZNotchDetector")
    , commands_ (commands), clock_ (clock), slotId_ (slotId)
    , lastPollMs_ (clock.nowMs())
{
    taps_[0] = &tapLane0;
    taps_[1] = tapLane1;
    eventOutbox_.reserve ((std::size_t) kMaxPendingEvents);
    eventScratch_.reserve ((std::size_t) kMaxPendingEvents);
}

NotchController::NotchController (LockFreeRingBuffer<float>& tap,
                                  LockFreeRingBuffer<NotchCommand>& commands,
                                  ClockSource& clock, int slotId)
    : NotchController (tap, nullptr, commands, clock, slotId) {}

NotchController::~NotchController()
{
    // Safety net only: normal shutdown goes through MainComponent's explicit
    // stop(). 2 s is generous for a loop whose step is ~5 ms.
    stop (2000);
}

void NotchController::start()
{
    if (isThreadRunning())
        return;
    startThread();
}

void NotchController::stop (int timeoutMs)
{
    if (isThreadRunning())
        stopThread (timeoutMs);
    // Whatever queued since the last poll -- or ever, if the thread never
    // ran -- reaches the sink before this returns (spec test 11). A sink
    // that re-enters clearNotch()/setNotch() from THIS flush queues more
    // events that nothing drains afterwards, so loop until the outbox is
    // actually empty; capped so a sink that never stops re-entering cannot
    // hang shutdown forever (review round 1).
    for (int i = 0; i < 8 && flushEventOutbox(); ++i) {}
}

void NotchController::setEventSink (EventSink sink)
{
    // Precondition: thread STOPPED (see header). Same contract as setWidth().
    eventSink_ = std::move (sink);
}

std::uint64_t NotchController::droppedEvents() const { return droppedEvents_.load (std::memory_order_relaxed); }

int NotchController::pendingEventsForTest() const
{
    const std::lock_guard<std::mutex> lock (modelMutex_);
    return (int) eventOutbox_.size();
}

bool NotchController::modelMutexIsFreeForTest()
{
    if (! modelMutex_.try_lock())
        return false;
    modelMutex_.unlock();
    return true;
}

void NotchController::failNextSetNotchOnLaneForTest (int lane)
{
    failSetNotchLaneForTest_.store (lane, std::memory_order_relaxed);
}

void NotchController::setWidth (int lanes)
{
    // Precondition: the detector thread is STOPPED (see header comment).
    // No lock: width_ has no concurrent reader while run() is not running.
    const int newWidth = std::clamp (lanes, 1, 2);
    if (newWidth < width_)
    {
        // Lanes leaving the slot take their notches with them: an active model
        // entry on a lane nobody analyses would never auto-release and would
        // keep a real filter running on a chain the operator thinks is idle.
        const std::lock_guard<std::mutex> lock (modelMutex_);
        for (int c = newWidth; c < kChannels; ++c)
            for (int i = 0; i < kSlots; ++i)
                pushClearLocked (c, i, ClearReason::WidthChange);
    }
    // No widening branch here (review round 1): resetting only the Detector
    // leaves CandidateScorer's rise history and baseline EMA holding
    // pre-mono audio, and once the zeroed frames age past
    // 0.45 x riseReferenceMs they become the reference and saturate the
    // rise/novelty axes (max(ref, 1e-12) floor) for ~100 ms -- a MORE
    // permissive detection window than the shipped behaviour. Lane D may not
    // change detection behaviour. Whether a re-entering lane should be gated
    // for riseReferenceMs after a widen is an OPEN OWNER DECISION for lane S
    // -- see SDD ledger 2026-09-05-data-loop, Task 3 ruling.
    width_ = newWidth;

    // A-R4 reset boundary, and the one that actually fires in the shipping
    // app: MainComponent's onAfterRestart hook calls setWidth() on every slot
    // after EVERY engine restart -- device change, sample-rate change, buffer
    // change (DevicePanel::onBeforeRestart/onAfterRestart) -- so this covers
    // the device/SR case even though nothing outside tests calls
    // setSampleRate(). Unconditional for that reason: a restart that keeps
    // the same width still invalidated the scorer's view of the room. A brief
    // N/A on the chip is honest; a stale number is not. Readout only -- the
    // scorer, the persistence streaks and the model are untouched here.
    for (auto& l : lanes_)
        l.blocksSinceReset = 0;
}

// --- Lane G ladder arithmetic (spec 4.1, Q13). Pure: no state, no locks. ---

double NotchController::nextDeeperRungDb (double currentDb, double ceilingDb)
{
    // A NaN ceiling means "unresolved" -- callers must pass ceilingDbFor()'s
    // result, never a raw ModelNotch::ceilingDb (which is NaN for Detector).
    jassert (! std::isnan (ceilingDb));

    // The shallowest FIXED rung strictly deeper than `currentDb` ...
    double next = kDepthLadderDb[kDepthLadderSize - 1];
    for (int i = 0; i < kDepthLadderSize; ++i)
        if (kDepthLadderDb[i] < currentDb)
        {
            next = kDepthLadderDb[i];
            break;
        }
    // ... capped at the ceiling, which is the effective ladder's LAST rung
    // (Q13). std::max picks the SHALLOWER of the two, because deeper is more
    // negative: ceiling -10 turns "-6 -> -12" into "-6 -> -10".
    //
    // Fix-round 1 (review finding "Important 2"): the true contract, past
    // what the comment used to say. AT the ceiling (currentDb == ceilingDb)
    // this returns currentDb unchanged. DEEPER than the ceiling (currentDb <
    // ceilingDb -- e.g. the slider moved after a Detector notch already stood
    // past the new ceiling) this returns the ceiling itself, which is
    // SHALLOWER than currentDb -- a step in the wrong direction for a caller
    // that only ever deepens. Such a caller MUST compare the result against
    // currentDb before sending it as a command.
    return std::max (next, ceilingDb);
}

double NotchController::nextShallowerRungDb (double currentDb)
{
    // The deepest rung strictly shallower than `currentDb`, saturating at the
    // ladder top. An odd Preset/Manual depth resolves upward.
    for (int i = kDepthLadderSize - 1; i >= 0; --i)
        if (kDepthLadderDb[i] > currentDb)
            return kDepthLadderDb[i];
    return kDepthLadderDb[0];
}

// The `ceilingRung` of spec 4.1 -- under Q13 that IS the ceiling value, so
// there is nothing to quantise. A Detector notch (ceilingDb == NaN) follows
// the LIVE slider; everything else carries its own (Q8).
double NotchController::ceilingDbFor (const ModelNotch& n) const
{
    return std::isnan (n.ceilingDb) ? notchDepthDb_.load (std::memory_order_relaxed)
                                    : n.ceilingDb;
}

void NotchController::run()
{
    while (! threadShouldExit())
    {
        runOnce();
        wait (5);
    }
}

bool NotchController::setNotch (int channel, int index,
                                double frequency, double Q, double depthDB,
                                Origin origin)
{
    return setNotchImpl (channel, index, frequency, Q, depthDB, origin, nullptr);
}

bool NotchController::setNotchImpl (int channel, int index, double frequency, double Q, double depthDB,
                                    Origin origin, const NotchEvent* scored)
{
    // width gates the policy surface; internal fan-out loops never exceed it.
    if (channel < 0 || channel >= width_ || index < 0 || index >= kSlots)
        return false;

    // Q12 / invariant 1: nothing deeper than the ladder floor leaves this
    // controller, whatever asked for it -- a preset file is not a trusted
    // source, and -24 dB already kills any howl this app can hear. Applied
    // BEFORE validation, so a positive depth is still refused below.
    const double depth = std::max (depthDB, kMaxDepthDb);

    // Same predicates Biquad::setNotchFilter applies (see header comment).
    const double sampleRate = lanes_[0].detector.getSampleRate();
    if (! (sampleRate > 0.0))                       return false;
    if (! (Q > 0.0))                                return false;
    if (! (frequency > 0.0 && frequency < sampleRate * 0.5))
                                                    return false;
    if (! (depth <= 0.0))                           return false;  // positive depth would BOOST

    if (failSetNotchLaneForTest_.load (std::memory_order_relaxed) == channel)
    {
        failSetNotchLaneForTest_.store (-1, std::memory_order_relaxed);
        return false;   // TEST ONLY: forces the partial-apply unwind path
    }

    const NotchCommand cmd { NotchCommandType::Set,
                             (std::uint8_t) channel, (std::uint8_t) index,
                             (float) frequency, (float) Q, (float) depth,
                             slotId_ };

    {
        const std::lock_guard<std::mutex> lock (modelMutex_);
        auto& n = model_[slotOf (channel, index)];
        n.frequency      = frequency;
        n.Q              = Q;
        n.depthDB        = depth;
        n.origin         = origin;
        n.active         = true;
        n.lockedAtMs     = liveMs_;
        n.lastDetectedMs = liveMs_;
        // Lane G (B-2): the one place a slot becomes active is the one place
        // the ladder can be trusted to start clean.
        n.deepestDb        = depth;
        n.stageChangedAtMs = liveMs_;
        n.quietMs          = 0.0;
        n.releasedSteps    = 0;
        n.ceilingDb        = (origin == Origin::Detector)
                                 ? std::numeric_limits<double>::quiet_NaN()
                                 : depth;
        outbox_.push_back (cmd);

        NotchEvent ev = scored != nullptr ? *scored : NotchEvent {};
        ev.kind = NotchEvent::Kind::Set;
        ev.slot = slotId_; ev.lane = channel; ev.index = index;
        ev.hz = (float) frequency; ev.q = (float) Q; ev.depthDb = (float) depth;
        ev.origin = origin;
        pushEventLocked (std::move (ev));
    }
    return true;
}

void NotchController::pushEventLocked (NotchEvent&& event)
{
    if (eventSink_ == nullptr)
        return;   // no sink: nothing is ever queued (spec test 10)
    if ((int) eventOutbox_.size() >= kMaxPendingEvents)
    {
        droppedEvents_.fetch_add (1, std::memory_order_relaxed);
        return;
    }
    eventOutbox_.push_back (std::move (event));
}

void NotchController::pushClearLocked (int channel, int index, ClearReason reason)
{
    auto& n = model_[slotOf (channel, index)];
    if (! n.active)
        return;
    n.active = false;
    outbox_.push_back ({ NotchCommandType::Clear,
                         (std::uint8_t) channel, (std::uint8_t) index,
                         0.0f, 0.0f, 0.0f, slotId_ });

    NotchEvent ev;
    ev.kind = NotchEvent::Kind::Clear;
    ev.slot = slotId_; ev.lane = channel; ev.index = index;
    ev.hz = (float) n.frequency; ev.q = (float) n.Q; ev.depthDb = (float) n.depthDB;
    ev.origin = n.origin; ev.reason = reason;
    ev.ageMs = liveMs_ - n.lockedAtMs;
    pushEventLocked (std::move (ev));
}

bool NotchController::pushRetuneLocked (int channel, int index, double newDepthDb,
                                        RetuneReason reason)
{
    // modelMutex_ HELD (the caller's contract, see the header). Documents the
    // precondition every caller already relies on -- slotOf() below would
    // otherwise index out of `model_` silently in a Release build.
    jassert (channel >= 0 && channel < kChannels && index >= 0 && index < kSlots);

    auto& n = model_[slotOf (channel, index)];
    if (! n.active)
        return false;

    // Validate-before-send: the same five predicates as setNotchImpl plus the
    // ladder floor (spec 4.4). Nothing is touched when any of them fails -- a
    // refused retune leaves the model AND the chain on the depth they already
    // agreed on, which is the only state where they cannot disagree.
    const double sampleRate = lanes_[0].detector.getSampleRate();
    if (! (sampleRate > 0.0))                                    return false;
    if (! (n.Q > 0.0))                                           return false;
    if (! (n.frequency > 0.0 && n.frequency < sampleRate * 0.5)) return false;
    if (! (newDepthDb <= 0.0))                                   return false;
    if (! (newDepthDb >= kMaxDepthDb))                           return false;

    // freq and Q come from the STORED notch, never from notchQ_ (m-2): a Q
    // differing by one bit sends NotchChain::setNotch down the reset path, and
    // a reset mid-signal is the click this whole lane exists to avoid.
    outbox_.push_back ({ NotchCommandType::Set,
                         (std::uint8_t) channel, (std::uint8_t) index,
                         (float) n.frequency, (float) n.Q, (float) newDepthDb,
                         slotId_ });

    NotchEvent ev;
    ev.kind = NotchEvent::Kind::Retune;
    ev.slot = slotId_; ev.lane = channel; ev.index = index;
    ev.hz = (float) n.frequency; ev.q = (float) n.Q;
    ev.fromDepthDb = (float) n.depthDB;
    ev.depthDb     = (float) newDepthDb;
    ev.origin = n.origin;
    ev.retuneReason = reason;
    // Age from PLACEMENT, like a Clear's. lockedAtMs, origin and
    // lastDetectedMs are deliberately left alone: this is the same notch, and
    // lane D's labels are keyed to when it was placed (B-1).
    ev.ageMs = liveMs_ - n.lockedAtMs;
    pushEventLocked (std::move (ev));

    n.depthDB = newDepthDb;
    return true;
}

void NotchController::clearNotch (int channel, int index, ClearReason reason)
{
    // width gates the policy surface; internal fan-out loops never exceed it.
    if (channel < 0 || channel >= width_ || index < 0 || index >= kSlots)
        return;
    const std::lock_guard<std::mutex> lock (modelMutex_);
    pushClearLocked (channel, index, reason);
}

void NotchController::clearAll (ClearReason reason)
{
    const std::lock_guard<std::mutex> lock (modelMutex_);
    for (int c = 0; c < kChannels; ++c)
        for (int i = 0; i < kSlots; ++i)
            pushClearLocked (c, i, reason);
}

int NotchController::adoptPreset (const std::vector<PresetNotch>& notches, int* skippedOut)
{
    int adopted = 0, skipped = 0, clamped = 0;
    for (const auto& p : notches)
    {
        // S-8: the file's index, always. A named lane lands on that lane; an
        // unnamed one lands on every lane, all-or-nothing, whatever linked_ says
        // (linked_ only steers where the DETECTOR places new notches -- it has
        // no say over what a preset file explicitly names).
        const int firstLane = (p.lane < 0) ? 0 : p.lane;
        const int lastLane  = (p.lane < 0) ? width_ - 1 : p.lane;
        if (firstLane >= width_)
        {
            ++skipped;   // lane 1 named on a mono slot: not adoptable here
            continue;
        }

        int applied = 0;
        for (int lane = firstLane; lane <= lastLane; ++lane)
            if (setNotch (lane, p.index, p.freq, p.Q, p.depthDB, Origin::Preset))
            {
                ++applied;
                // Q12: setNotchImpl does the clamping; count it HERE so the
                // operator is told a hand-edited file asked for more than the
                // app allows, instead of quietly getting a different filter
                // than the file names.
                //
                // M-5: counted only when the notch was ACTUALLY adopted and
                // the clamp actually moved the value. Counting at the top of
                // the loop body -- above the `firstLane >= width_` skip and
                // above setNotchImpl's own validation -- reports clamps on
                // notches that were never applied at all: a lane-1 notch on a
                // mono slot, a freq past Nyquist, a positive depth.
                // `lane == firstLane` makes it one count per PresetNotch, not
                // one per lane of a mirrored stereo pair.
                if (lane == firstLane && p.depthDB < kMaxDepthDb)
                    ++clamped;
            }
        const int wanted = lastLane - firstLane + 1;
        if (applied == wanted)
            ++adopted;
        else if (applied > 0)
            for (int lane = firstLane; lane <= lastLane; ++lane)
                clearNotch (lane, p.index, ClearReason::PartialApplyUnwind);
    }
    if (clamped > 0)
        juce::Logger::writeToLog ("preset: clamped " + juce::String (clamped)
                                  + " notch depth(s) to " + juce::String (kMaxDepthDb, 1)
                                  + " dB (slot " + juce::String (slotId_) + ")");
    if (skippedOut != nullptr)
        *skippedOut = skipped;
    return adopted;
}

void NotchController::runOnce()
{
    // 1. Drain every block the tap already holds, so a large audio callback
    //    delivering several hops at once never leaves the detector behind.
    //    Each fresh block is published to the GUI snapshot IMMEDIATELY,
    //    while its magnitudes pointer is still valid (it dies at the next
    //    processLatestBlock call).
    const double now = clock_.nowMs();
    const int lanesToRead = analysedLanes();
    for (;;)
    {
        std::array<Detector::Spectrum, kChannels> spec {};
        bool any = false;
        for (int l = 0; l < lanesToRead; ++l)
        {
            spec[(std::size_t) l] = lanes_[(std::size_t) l].detector.processLatestBlock (*taps_[(std::size_t) l]);
            any = any || spec[(std::size_t) l].magnitudes != nullptr;
        }
        if (! any)
            break;

        // --- the frame the GUI will see -------------------------------
        // The notch list is gathered HERE, before this frame is scored, so
        // a notch this frame places still appears in the NEXT snapshot --
        // exactly as it did before the publish moved (spec acceptance 2: the
        // risk chip must reach Critical BEFORE the notch shows up in ACTIVE
        // NOTCHES, not with it).
        std::array<SnapshotNotch, kTotalSlots> notchList {};
        std::uint32_t notchCount = 0;
        {
            const std::lock_guard<std::mutex> lock (modelMutex_);
            lastDataMs_ = now;
            for (int c = 0; c < kChannels; ++c)
                for (int i = 0; i < kSlots; ++i)
                {
                    const auto& n = model_[slotOf (c, i)];
                    if (! n.active) continue;
                    notchList[notchCount++] = { (float) n.frequency, (float) n.Q,
                                                (float) n.depthDB, (float) n.deepestDb,
                                                (std::uint8_t) c, (std::uint8_t) i };
                }
        }

        // One tick per lockstep drain iteration, BEFORE the per-lane dispatch
        // below, so every lane processed in this pass reads the same number.
        // Skipping 0 on wrap keeps the never-placed sentinel in
        // linkedPlacedAt_ unambiguous (the wrap is ~1.4 years of continuous
        // 10.7 ms drains away, but a stale stamp matching costs a missed
        // notch, so it is not left to luck).
        if (++drainIteration_ == 0)
            ++drainIteration_;

        // RING RISK accumulator for THIS frame (A-R2/A-R8): cleared before
        // the pass, filled by every lane in it, published just after it.
        frameMaxScore_   = 0.0f;
        frameScoreValid_ = false;

        // The cross-lane comparison (§4.4) compares magnitudes that are, per
        // the invariant above, never more than one hop apart: `other` is
        // handed over only when the opposite lane produced a block in THIS
        // drain iteration, and is nullptr otherwise.
        for (int l = 0; l < lanesToRead; ++l)
            if (spec[(std::size_t) l].magnitudes != nullptr)
            {
                const float* other = (lanesToRead == 2 && spec[(std::size_t) (1 - l)].magnitudes != nullptr)
                                         ? spec[(std::size_t) (1 - l)].magnitudes : nullptr;
                processSpectrumForDetection (l, spec[(std::size_t) l], other, now);
            }

        // --- snapshot publish (A-R2) ---------------------------------
        // Below the detection pass, not above it: the RING RISK score only
        // exists once this frame has been scored, and score, magnitudes and
        // notch list must leave under ONE lock as one instant. Nothing else
        // moved with it -- no new lock, no new thread, and spec[] is still
        // alive here (it dies at the next processLatestBlock, one iteration
        // from now).
        {
            const std::lock_guard<std::mutex> lock (snapshotMutex_);
            // magnitudeCount/laneCount below are set unconditionally even
            // though a lane with spec[l].magnitudes == nullptr skips its
            // copy and so keeps its PREVIOUS bins in latest_.magnitudes[l].
            // That is accepted by design, not an oversight: both rings of a
            // slot are written by the same audio callback with the same
            // sample count (bridge design), so both lanes' Detectors yield a
            // fresh block in the same drain iteration. A lane can only lag
            // the other if its ring dropped samples the other one did not --
            // equal ring capacities and one writer preclude that. So
            // magnitudes[l] is never more than one hop stale here, and there
            // is no need for a per-lane freshness flag in the snapshot.
            for (int l = 0; l < lanesToRead; ++l)
                if (spec[(std::size_t) l].magnitudes != nullptr)
                    std::copy (spec[(std::size_t) l].magnitudes,
                               spec[(std::size_t) l].magnitudes + Detector::kNumBins,
                               latest_.magnitudes[(std::size_t) l].begin());
            latest_.magnitudeCount = (std::uint32_t) Detector::kNumBins;
            latest_.laneCount      = (std::uint32_t) lanesToRead;
            // The operator's own switch, not effectiveLinked(): the panel
            // must show what was ASKED for, so a slot that is forced linked
            // by width or a missing tap still reads back INDEP.
            latest_.linked         = linked_.load (std::memory_order_relaxed);
            latest_.sampleRate     = spec[0].magnitudes != nullptr ? spec[0].sampleRate : spec[1].sampleRate;
            latest_.notches        = notchList;
            latest_.notchCount     = notchCount;
            // RING RISK: the frame just scored, straight from the placement
            // path -- 0 with valid=false when nothing scored this frame.
            latest_.ringRiskScore     = frameMaxScore_;
            latest_.ringRiskValid     = frameScoreValid_;
            latest_.ringRiskThreshold = CandidateScorer::kConfirmScore;
            ++latest_.sequence;
        }
    }

    // 2. Advance the LIVE clock. Wall-clock dt, gated by tap liveness --
    //    NOT by whether THIS poll delivered data (that halves the rate).
    const double nowPolled = clock_.nowMs();
    const double dt        = nowPolled - lastPollMs_;
    lastPollMs_            = nowPolled;

    bool tapAlive = false;
    {
        const std::lock_guard<std::mutex> lock (modelMutex_);
        tapAlive = (nowPolled - lastDataMs_) < kTapSilenceTimeoutMs;
        if (tapAlive)
            liveMs_ += dt;

        // Spec §5.3: soundcheck detects FOR THE DURATION -- nothing clears
        // detectionActive_ after the window, so without this the controller
        // detects forever after a single Soundcheck. The expiry is judged in
        // the SAME live time the countdown reports, immediately after the
        // same advance, so it cannot drift from getSoundcheckRemainingMs().
        // Clearing the deadline here is what makes the disarm happen exactly
        // once: later polls see no deadline and store nothing.
        const double endsAt = soundcheckEndsAtLiveMs_.load (std::memory_order_relaxed);
        if (endsAt > 0.0 && liveMs_ >= endsAt)
        {
            soundcheckEndsAtLiveMs_.store (-1.0, std::memory_order_relaxed);
            detectionActive_.store (false, std::memory_order_relaxed);
        }
    }

    // 3. Auto-release: only meaningful while audio actually flows (D-06).
    if (tapAlive)
    {
        const std::lock_guard<std::mutex> lock (modelMutex_);
        for (int c = 0; c < kChannels; ++c)
            for (int i = 0; i < kSlots; ++i)
            {
                auto& n = model_[slotOf (c, i)];
                // KD-7: soundcheck notches never auto-release.
                if (n.active && n.origin != Origin::Soundcheck
                    && (liveMs_ - n.lastDetectedMs) > kAutoReleaseMs)
                    pushClearLocked (c, i, ClearReason::AutoRelease);
            }
    }

    // 4. Flush whatever the steps above queued.
    flushOutbox();
    flushEventOutbox();
}

double NotchController::liveMsForTest() const
{
    const std::lock_guard<std::mutex> lock (modelMutex_);
    return liveMs_;
}

double NotchController::depthDbForTest (int channel, int index) const
{
    const std::lock_guard<std::mutex> lock (modelMutex_);
    return model_[slotOf (std::clamp (channel, 0, kChannels - 1),
                          std::clamp (index, 0, kSlots - 1))].depthDB;
}

double NotchController::deepestDbForTest (int channel, int index) const
{
    const std::lock_guard<std::mutex> lock (modelMutex_);
    return model_[slotOf (std::clamp (channel, 0, kChannels - 1),
                          std::clamp (index, 0, kSlots - 1))].deepestDb;
}

double NotchController::quietMsForTest (int channel, int index) const
{
    const std::lock_guard<std::mutex> lock (modelMutex_);
    return model_[slotOf (std::clamp (channel, 0, kChannels - 1),
                          std::clamp (index, 0, kSlots - 1))].quietMs;
}

bool NotchController::activeForTest (int channel, int index) const
{
    // B-3: the FLAG. pushClearLocked lowers `active` and leaves
    // frequency/Q/depthDB standing -- the Clear event reads n.depthDB -- so
    // `depthDB < 0` is true for every slot that has ever held a notch.
    const std::lock_guard<std::mutex> lock (modelMutex_);
    return model_[slotOf (std::clamp (channel, 0, kChannels - 1),
                          std::clamp (index, 0, kSlots - 1))].active;
}

bool NotchController::retuneForTest (int channel, int index, double newDepthDb,
                                     RetuneReason reason)
{
    if (channel < 0 || channel >= kChannels || index < 0 || index >= kSlots)
        return false;
    const std::lock_guard<std::mutex> lock (modelMutex_);
    return pushRetuneLocked (channel, index, newDepthDb, reason);
}

// Fix-round 1 (review finding "Important 1"): ceilingDbFor and the rest of
// ModelNotch's ladder state had no accessor and no test.
double NotchController::ceilingDbForTest (int channel, int index) const
{
    const std::lock_guard<std::mutex> lock (modelMutex_);
    return ceilingDbFor (model_[slotOf (std::clamp (channel, 0, kChannels - 1),
                                        std::clamp (index, 0, kSlots - 1))]);
}

double NotchController::rawCeilingDbForTest (int channel, int index) const
{
    const std::lock_guard<std::mutex> lock (modelMutex_);
    return model_[slotOf (std::clamp (channel, 0, kChannels - 1),
                          std::clamp (index, 0, kSlots - 1))].ceilingDb;
}

int NotchController::releasedStepsForTest (int channel, int index) const
{
    const std::lock_guard<std::mutex> lock (modelMutex_);
    return model_[slotOf (std::clamp (channel, 0, kChannels - 1),
                          std::clamp (index, 0, kSlots - 1))].releasedSteps;
}

double NotchController::stageChangedAtMsForTest (int channel, int index) const
{
    const std::lock_guard<std::mutex> lock (modelMutex_);
    return model_[slotOf (std::clamp (channel, 0, kChannels - 1),
                          std::clamp (index, 0, kSlots - 1))].stageChangedAtMs;
}

void NotchController::setRingRiskOverrideForTest (std::optional<std::pair<bool, float>> override)
{
    // Detector-thread state, written from the test thread with the detector
    // STOPPED -- same precondition as setWidth() and setEventSink().
    ringRiskOverrideForTest_ = override;
}

void NotchController::setDetectionActive (bool active)
{
    detectionActive_.store (active, std::memory_order_relaxed);
}

// --- Detection tuning (brief 2026-08-24). Message thread; atomics relaxed. --

void NotchController::setRiseReferenceMs (double ms)
{
    for (auto& l : lanes_)
        l.scorer.setRiseReferenceMs (ms);
}

double NotchController::getRiseReferenceMs() const
{
    return lanes_[0].scorer.getRiseReferenceMs();
}

double NotchController::getRiseReferenceMs (int lane) const
{
    return lanes_[(std::size_t) std::clamp (lane, 0, kChannels - 1)].scorer.getRiseReferenceMs();
}

void NotchController::setPersistenceBlocks (int blocks)
{
    persistenceBlocks_.store (std::clamp (blocks, kMinPersistenceBlocks, kMaxPersistenceBlocks),
                              std::memory_order_relaxed);
}

int NotchController::getPersistenceBlocks() const
{
    return persistenceBlocks_.load (std::memory_order_relaxed);
}

void NotchController::setNotchDefaults (double q, double depthDb)
{
    // Same floors as the UI combos, widened slightly: a Q of 8 is the lowest
    // musically usable notch width here and -24 dB is deep enough to kill any
    // howl; beyond either end a preset or future panel could only do harm.
    notchQ_.store      (std::clamp (q, 8.0, 50.0),        std::memory_order_relaxed);
    notchDepthDb_.store(std::clamp (depthDb, -24.0, -6.0), std::memory_order_relaxed);
}

double NotchController::getNotchQ() const
{
    return notchQ_.load (std::memory_order_relaxed);
}

double NotchController::getNotchDepthDb() const
{
    return notchDepthDb_.load (std::memory_order_relaxed);
}

void NotchController::setPeakinessThreshold (float t)
{
    for (auto& l : lanes_)
        l.analyzer.setThreshold (t);   // clamped in PeakinessAnalyzer
}

float NotchController::getPeakinessThreshold() const
{
    return lanes_[0].analyzer.getThreshold();
}

float NotchController::getPeakinessThreshold (int lane) const
{
    return lanes_[(std::size_t) std::clamp (lane, 0, kChannels - 1)].analyzer.getThreshold();
}

void NotchController::startSoundcheck()
{
    const std::lock_guard<std::mutex> lock (modelMutex_);
    soundcheckEndsAtLiveMs_.store (liveMs_ + kSoundcheckDurationMs,
                                   std::memory_order_relaxed);
    detectionActive_.store (true, std::memory_order_relaxed);
}

// soundcheckEndsAtLiveMs_ is atomic<double> but is ALWAYS read under
// modelMutex_ together with liveMs_ -- the atomic is belt-and-braces only;
// the mutex is what actually keeps the pair consistent.
double NotchController::remainingSoundcheckMs() const
{
    const std::lock_guard<std::mutex> lock (modelMutex_);
    const double end = soundcheckEndsAtLiveMs_.load (std::memory_order_relaxed);
    if (! (end > 0.0))
        return 0.0;
    return std::max (0.0, end - liveMs_);
}

bool NotchController::soundcheckActive() const
{
    return remainingSoundcheckMs() > 0.0;
}

double NotchController::getSoundcheckRemainingMs() const
{
    return remainingSoundcheckMs();
}

void NotchController::setLaneAsymmetryBonus (float b)
{
    laneAsymmetryBonus_.store (std::clamp (b, kMinLaneAsymmetryBonus, kMaxLaneAsymmetryBonus),
                               std::memory_order_relaxed);
}

float NotchController::getLaneAsymmetryBonus() const
{
    return laneAsymmetryBonus_.load (std::memory_order_relaxed);
}

// Design §4.4. Neutral (1.0) whenever the comparison is not meaningful: no
// opposite-lane block this iteration, a bonus of exactly 1.0 (the default --
// no arithmetic, no rounding), or a bin too close to either end for
// peakinessAt to build a full annulus. The 0.5x margin says the other lane
// must be MARKEDLY flatter, not merely lower -- stereo programme material
// differs between lanes by a few dB all the time.
float NotchController::asymmetryMultiplier (const float* mine, const float* other, int bin, float bonus)
{
    if (other == nullptr || ! (bonus > 1.0f))
        return 1.0f;
    if (bin < PeakinessAnalyzer::kNeighbourOuterRadius
        || bin >= Detector::kNumBins - PeakinessAnalyzer::kNeighbourOuterRadius)
        return 1.0f;
    const float minePk  = PeakinessAnalyzer::peakinessAt (mine,  Detector::kNumBins, bin);
    const float otherPk = PeakinessAnalyzer::peakinessAt (other, Detector::kNumBins, bin);
    return (otherPk < 0.5f * minePk) ? bonus : 1.0f;
}

int NotchController::firstFreeIndexLocked (int lane) const
{
    for (int i = 0; i < kSlots; ++i)
        if (! model_[slotOf (lane, i)].active)
            return i;
    return -1;
}

int NotchController::firstFreeIndexAllLanesLocked() const
{
    for (int i = 0; i < kSlots; ++i)
    {
        bool free = true;
        for (int c = 0; c < width_; ++c)
            free = free && ! model_[slotOf (c, i)].active;
        if (free)
            return i;
    }
    return -1;
}

// The one place a confirmed candidate becomes notches. INDEP touches `lane`
// alone; LINKED takes one index free on every driven lane and writes them all.
void NotchController::placeConfirmed (int lane, const PeakinessAnalyzer::Candidate& cand, bool linkedNow,
                                      const PlacementContext& pc)
{
    // soundcheckActive() takes modelMutex_ itself, so it is asked BEFORE the
    // lock below -- never underneath it.
    const Origin origin  = soundcheckActive() ? Origin::Soundcheck : Origin::Detector;
    const double q       = notchQ_.load (std::memory_order_relaxed);
    // Lane G (spec 4.3, Q1): the depth default is now the CEILING, not the
    // depth. A notch is placed as SHALLOW as the policy allows and earns the
    // rest from the reinforce loop, so a room that only needs 6 dB keeps the
    // 12 dB of tone 1.1.3 threw away. Q13: the ceiling need not be a multiple
    // of 6 and is itself the deepest rung -- presets/Music.json ships -10.
    const double ceiling = notchDepthDb_.load (std::memory_order_relaxed);

    int index = -1, firstLane = lane, lastLane = lane;
    {
        const std::lock_guard<std::mutex> lock (modelMutex_);
        if (linkedNow) { index = firstFreeIndexAllLanesLocked(); firstLane = 0; lastLane = width_ - 1; }
        else           { index = firstFreeIndexLocked (lane); }
        // Task 8 hooks the room-memory lookup in HERE -- it needs `index >= 0`
        // (the entry is consumed) and the bin width, and it must run under
        // this same lock. Nothing to do in this task.
    }
    if (index < 0)
        return;   // chain full on the lanes concerned: same outcome as today

    // --- the depth choice (spec 4.3). It lives BELOW the index lookup so that
    // Task 8's step 3 can read the room memory the lookup above provides.
    double depthDb = kDepthLadderDb[0];                       // step 1: -6
    if (pc.breakdown.riseRatio >= kSteepRiseRatio)            // step 2: +6 dB or more
        depthDb = kDepthLadderDb[1];                          //         over the rise
                                                              //         window -> -12
    // step 3 (room memory) is inserted HERE by Task 8, and nowhere else.

    // step 4: never deeper than the ceiling, which under Q13 IS the deepest
    // rung. max() picks the SHALLOWER of the two because deeper is more
    // negative: ceiling -10 turns a steep-rise -12 into -10.
    depthDb = std::max (depthDb, ceiling);

    // KD-7, and this line stays LAST through Task 8: soundcheck has no ladder.
    // Those notches never deepen, never release and never reclamp, so starting
    // them shallow -- or letting a remembered depth decide for them -- would
    // leave a howl the operator explicitly asked to lock permanently under-cut.
    // Moving or dropping it reds SoundcheckPlacesAtTheFullSliderDepth and
    // SoundcheckNotchesNeverDeepen.
    if (origin == Origin::Soundcheck)
        depthDb = ceiling;

    // Lane D (data loop): the one allocation per placed notch, on the
    // detector thread (plan A-4). Shared by the lane-0 and lane-1 Set events
    // of a LINKED placement -- never allocated twice for one confirm.
    // pc.breakdown.refFrame points into the scorer's history_ and is only
    // guaranteed valid until la.scorer.commitBlock() runs; that call happens
    // AFTER the whole candidate loop in processSpectrumForDetection(), i.e.
    // after this placeConfirmed() returns, so copying it here is safe.
    auto ctx = std::make_shared<SpectralContext>();
    ctx->binHz = pc.sampleRate / (double) Detector::kFftSize;
    std::copy_n (pc.now, Detector::kNumBins, ctx->now.begin());
    if (pc.breakdown.refFrame != nullptr)
    {
        ctx->hasRef = true;
        ctx->refAgeMs = pc.breakdown.refAgeMs;
        std::copy_n (pc.breakdown.refFrame, Detector::kNumBins, ctx->ref.begin());
    }
    if (pc.other != nullptr)
    {
        ctx->hasOther = true;
        std::copy_n (pc.other, Detector::kNumBins, ctx->other.begin());
    }

    NotchEvent scored;
    scored.hasScore = true;
    scored.confirmedLane = lane;
    scored.score = pc.finalScore; scored.peakiness = pc.breakdown.rawPeakiness;
    scored.pNorm = pc.breakdown.pNorm; scored.rise = pc.breakdown.rNorm;
    scored.novelty = pc.breakdown.mNorm; scored.penalty = pc.breakdown.penalty;
    scored.riseRatio = pc.breakdown.riseRatio;   // B-5: the RAW ratio, not rNorm
    scored.asymmetry = pc.asymmetry; scored.persistNeeded = pc.persistNeeded; scored.thr = pc.thr;
    scored.ctx = ctx;

    int applied = 0;
    for (int l = firstLane; l <= lastLane; ++l)
        if (setNotchImpl (l, index, cand.frequencyHz, q, depthDb, origin, &scored))
            ++applied;

    // Review finding (round 1, corrected in round 2): under LINKED, lane 0 and
    // lane 1 keep independent persistence counters per bin (§4.3 --
    // persistence is still counted per (lane, bin)). runOnce() drains lane 0
    // then lane 1 within the SAME iteration, so if a howl sits on BOTH lanes
    // their counters can both cross the confirm threshold before either
    // placement lands: lane 0's call sets index i on both lanes, then lane
    // 1's call -- unaware the pair is already placed -- sets index i+1 on
    // both lanes too. That is two notches for one frequency, double the cut
    // 1.0.4 never produced (it only ever analysed one lane), and a chain that
    // fills twice as fast.
    //
    // TWO mechanisms below, and they are complementary rather than redundant:
    //
    //   * the STAMP stops a lane processed AFTER this one in this same
    //     iteration. Zeroing that lane's counter cannot do that job: with
    //     persistenceBlocks == 1 -- operator-selectable from the DETECTION
    //     panel and what the AGGRESSIVE preset ships -- its very next ++ takes
    //     the counter 0 -> 1 >= 1 and it confirms anyway, in this same drain.
    //     processSpectrumForDetection() reads the stamp and skips the bin.
    //   * the RESET stops a lane processed BEFORE this one, whose streak for
    //     this bin is now stale. The stamp expires with the iteration, so it
    //     is never consulted on that lane's behalf; only zeroing its counter
    //     costs it the requiredBlocks it must now re-earn.
    //
    // Both cover bin-1..bin+1 (bounds-checked). A howl straddling two bins can
    // confirm on bin b on one lane and bin b+1 on the other, and one placed
    // pair should answer that whole neighbourhood: at 48 kHz / 2048 a bin is
    // 23.4 Hz, narrower than the notch just placed (Q 30 at 1 kHz is ~33 Hz
    // wide), so no second howl the analyser could actually tell apart is being
    // suppressed.
    //
    // lanes_[].persistence, linkedPlacedAt_ and drainIteration_ are all
    // detector-thread-only state (runOnce is never called concurrently from
    // two threads), so none of this needs a lock.
    if (linkedNow && applied > 0)
    {
        const int firstBin = std::max (0, cand.bin - 1);
        const int lastBin  = std::min (Detector::kNumBins - 1, cand.bin + 1);
        for (int b = firstBin; b <= lastBin; ++b)
        {
            linkedPlacedAt_[(std::size_t) b] = drainIteration_;
            for (int l = 0; l < width_; ++l)
                if (l != lane)
                    lanes_[(std::size_t) l].persistence[(std::size_t) b] = 0;
        }
    }

    // Partial-failure analysis, unchanged from 1.0.4: setNotch validates only
    // index bounds + params + sample rate, identical across lanes, so a
    // partial application has no realistic trigger. If it ever happens,
    // unwind rather than leave one lane unprotected while the GUI claims
    // protection.
    const int wanted = lastLane - firstLane + 1;
    if (applied != wanted && applied > 0)
        for (int l = firstLane; l <= lastLane; ++l)
            clearNotch (l, index, ClearReason::PartialApplyUnwind);

    // One light line per detection event -- per PLACED notch, not per frame.
    // Logged outside every lock: juce::Logger is not a place to hold one.
    juce::Logger::writeToLog (
        "[detect] slot=" + juce::String (slotId_)
        + " lane=" + (linkedNow ? juce::String ("LR") : juce::String (lane == 0 ? "L" : "R"))
        + " idx=" + juce::String (index)
        + " freq=" + juce::String ((int) std::lround (cand.frequencyHz))
        + " Q=" + juce::String (q, 1) + " depth=" + juce::String (depthDb, 1)
        + " rise=" + juce::String ((int) std::lround (lanes_[(std::size_t) lane].scorer.getRiseReferenceMs())));
}

void NotchController::processSpectrumForDetection (int lane, const Detector::Spectrum& block,
                                                   const float* otherLaneMagnitudes,
                                                   double blockNowMs)
{
    if (! detectionActive_.load (std::memory_order_relaxed))
        return;

    auto& la = lanes_[(std::size_t) lane];
    // Read ONCE: a setLinked() landing mid-block must not have this frame
    // place with one policy and feed auto-release with the other.
    const bool linkedNow = effectiveLinked();

    // Real gap since the previous DRAINED block; the first block has no
    // predecessor and passes 0 (the EMA deliberately skips zero-dt updates).
    const double rawDt     = blockNowMs - la.previousBlockNowMs;
    const double elapsedMs = (la.previousBlockNowMs > 0.0 && rawDt > 0.0) ? rawDt : 0.0;
    la.previousBlockNowMs  = blockNowMs;

    la.scorer.beginBlock (block.sampleRate);

    // RING RISK validity (A-R4), taken BEFORE this frame is committed: a
    // scorer with no committed frame SINCE THE LAST RESET takes the "no
    // history at all -> rNorm 1" branch, or compares against magnitudes from
    // a rate/width this lane no longer runs at, so whatever it returns is not
    // a measurement of the room now. OR across the lanes analysed this
    // iteration -- one readout per slot.
    frameScoreValid_ = frameScoreValid_ || la.blocksSinceReset > 0;

    // Feed auto-release FIRST so a still-ringing locked notch stays fed by the
    // same frame the scorer looks at (spec 5.2 step 7).
    //
    // Which lanes a frame may reinforce is the placement policy read back:
    // INDEP feeds only the lane this spectrum came from (design §4.3 -- a
    // lane's notch is fed by "the spectrum of that same lane"); LINKED feeds
    // the whole pair from either lane, so a linked pair releases together.
    {
        const std::lock_guard<std::mutex> lock (modelMutex_);
        const double binWidthHz = block.sampleRate / (double) Detector::kFftSize;
        for (int c = 0; c < kChannels; ++c)
        {
            if (! linkedNow && c != lane)
                continue;
            for (int i = 0; i < kSlots; ++i)
            {
                auto& n = model_[slotOf (c, i)];
                if (! n.active || n.origin == Origin::Soundcheck)
                    continue;
                const int bin = (int) std::lround (n.frequency / binWidthHz);
                if (bin < 0 || bin >= Detector::kNumBins)
                    continue;
                // Same live threshold analyse() used to accept candidates --
                // a notch re-tested against a stale default would disagree
                // with the panel about whether it is still reinforced.
                if (PeakinessAnalyzer::peakinessAt (block.magnitudes,
                                                    Detector::kNumBins, bin)
                        > la.analyzer.getThreshold())
                {
                    n.lastDetectedMs = liveMs_;
                }
            }
        }
    }

    // Locked fundamentals for the harmonic penalty (KD-3). Same lane rule as
    // the reinforcement loop above: under INDEP the lanes hold different
    // notches, and lane 1's fundamental has no business halving a lane-0
    // candidate's score (design §4.3). Under LINKED the two lists are
    // identical anyway, so 1.0.4's behaviour is preserved exactly.
    std::vector<double> locked;
    {
        const std::lock_guard<std::mutex> lock (modelMutex_);
        for (int c = 0; c < kChannels; ++c)
        {
            if (! linkedNow && c != lane)
                continue;
            for (int i = 0; i < kSlots; ++i)
                if (model_[slotOf (c, i)].active)
                    locked.push_back (model_[slotOf (c, i)].frequency);
        }
    }

    const auto result = la.analyzer.analyse (block);
    for (std::size_t i = 0; i < result.count && i < (std::size_t) Detector::kNumBins; ++i)
    {
        const auto& cand = result.candidates[i];
        const int  bin  = cand.bin;

        // A LINKED pair placed THIS drain iteration -- by the other lane, or
        // by an earlier candidate of this same block -- already covers this
        // bin and its two neighbours on every driven lane. Confirming again
        // would stack a second pair at the next free index for one howl (see
        // placeConfirmed's note). The streak dies with it: the bin has been
        // answered, so what it accumulated is spent. Consulted only under
        // LINK -- INDEP never places on a lane it did not confirm on, so it
        // has no pair to double up on and its behaviour is untouched.
        if (linkedNow && linkedPlacedAt_[(std::size_t) bin] == drainIteration_)
        {
            la.persistence[(std::size_t) bin] = 0;
            continue;
        }

        const auto breakdown = la.scorer.scoreCandidateDetailed (
            cand, block.magnitudes,
            { locked.data(), locked.size() });
        // Design §4.4: neutral at the default bonus of 1.0.
        const float asym = asymmetryMultiplier (block.magnitudes, otherLaneMagnitudes, cand.bin,
                                                laneAsymmetryBonus_.load (std::memory_order_relaxed));
        const float score = breakdown.score * asym;

        // RING RISK readout (A-R1/A-R8): the highest of exactly these numbers
        // over every candidate of every lane of the slot, recorded before the
        // confirm test so the readout cannot disagree with the decision. Read
        // only -- nothing below this line looks at it.
        frameMaxScore_ = std::max (frameMaxScore_, score);

        if (score > CandidateScorer::kConfirmScore)
        {
            const std::uint32_t requiredBlocks =
                (std::uint32_t) persistenceBlocks_.load (std::memory_order_relaxed);
            if (++la.persistence[(std::size_t) bin] >= requiredBlocks)
            {
                la.persistence[(std::size_t) bin] = 0;

                // KD-5: automatic notch params are runtime defaults set from
                // the DETECTION panel. Which LANES and which INDEX the notch
                // lands on is now the placement policy's business (§4.3);
                // persistence is still counted per (lane, bin), so under LINK
                // a confirm on either lane is enough for the pair.
                PlacementContext pc;
                pc.now = block.magnitudes; pc.other = otherLaneMagnitudes;
                pc.breakdown = breakdown; pc.finalScore = score; pc.asymmetry = asym;
                pc.persistNeeded = (int) requiredBlocks;
                pc.thr = la.analyzer.getThreshold();
                pc.sampleRate = block.sampleRate;
                placeConfirmed (lane, cand, linkedNow, pc);
            }
        }
        else
        {
            // Any non-confirming block resets that bin's streak.
            la.persistence[(std::size_t) bin] = 0;
        }
    }

    la.scorer.commitBlock (block.magnitudes, elapsedMs);
    // Counted next to the commit it counts (A-R4). Saturates instead of
    // wrapping -- see the member's comment.
    if (la.blocksSinceReset < std::numeric_limits<std::uint32_t>::max())
        ++la.blocksSinceReset;
}

void NotchController::copySnapshot (SnapshotBuffer& destOwnedByCaller) const
{
    const std::lock_guard<std::mutex> lock (snapshotMutex_);
    destOwnedByCaller = latest_;
}

std::uint64_t NotchController::retryCount() const
{
    return retryCount_.load (std::memory_order_relaxed);
}

void NotchController::setSampleRate (double sampleRate)
{
    for (auto& l : lanes_)
    {
        l.detector.setSampleRate (sampleRate);
        // A-R4 reset boundary. The scorer itself is deliberately NOT reset
        // (that would move notches; lane D removed it), so its rise history
        // and baseline EMAs still hold old-rate magnitudes at bin indices
        // that now mean different frequencies. Zeroing the readout's counter
        // is what stops the chip claiming a measurement it does not have --
        // it reads N/A until this lane has committed a block at the new rate.
        l.blocksSinceReset = 0;
    }
}

void NotchController::flushOutbox()
{
    std::vector<NotchCommand> pending;
    {
        const std::lock_guard<std::mutex> lock (modelMutex_);
        pending.swap (outbox_);
    }
    if (pending.empty())
        return;

    const std::size_t written = commands_.write (pending.data(), pending.size());
    if (written < pending.size())
    {
        // Short write: the ring was full. Keep the unsent tail and retry on
        // the next pump -- a full queue DELAYS a notch, never loses one, so
        // the model cannot diverge from the chain (bridge design §2).
        const std::lock_guard<std::mutex> lock (modelMutex_);
        outbox_.insert (outbox_.end(),
                        pending.begin() + (std::ptrdiff_t) written,
                        pending.end());
        retryCount_.fetch_add (pending.size() - written, std::memory_order_relaxed);
    }
}

bool NotchController::flushEventOutbox()
{
    {
        const std::lock_guard<std::mutex> lock (modelMutex_);
        if (eventOutbox_.empty())
            return false;
        eventScratch_.clear();
        eventScratch_.swap (eventOutbox_);   // both keep their reserve()
    }
    // Lock released: the sink may take its own locks, log, or call straight
    // back into clearNotch() (spec test 9). eventSink_ is only written with
    // the thread stopped, so reading it here is race-free.
    if (eventSink_ != nullptr)
        for (const auto& e : eventScratch_)
            eventSink_ (e);
    eventScratch_.clear();
    return true;
}
