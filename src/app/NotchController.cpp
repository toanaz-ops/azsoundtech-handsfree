#include "app/NotchController.h"

#include <algorithm>
#include <cmath>

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
    if (! isThreadRunning())
        return;
    stopThread (timeoutMs);
}

void NotchController::setWidth (int lanes)
{
    // Precondition: the detector thread is STOPPED (see header comment).
    // No lock: width_ has no concurrent reader while run() is not running.
    width_ = std::clamp (lanes, 1, 2);
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
    // width gates the policy surface; internal fan-out loops never exceed it.
    if (channel < 0 || channel >= width_ || index < 0 || index >= kSlots)
        return false;

    // Same predicates Biquad::setNotchFilter applies (see header comment).
    const double sampleRate = lanes_[0].detector.getSampleRate();
    if (! (sampleRate > 0.0))                       return false;
    if (! (Q > 0.0))                                return false;
    if (! (frequency > 0.0 && frequency < sampleRate * 0.5))
                                                    return false;
    if (! (depthDB <= 0.0))                         return false;  // positive depth would BOOST

    const NotchCommand cmd { NotchCommandType::Set,
                             (std::uint8_t) channel, (std::uint8_t) index,
                             (float) frequency, (float) Q, (float) depthDB,
                             slotId_ };

    {
        const std::lock_guard<std::mutex> lock (modelMutex_);
        auto& n = model_[slotOf (channel, index)];
        n.frequency      = frequency;
        n.Q              = Q;
        n.depthDB        = depthDB;
        n.origin         = origin;
        n.active         = true;
        n.lockedAtMs     = liveMs_;
        n.lastDetectedMs = liveMs_;
        outbox_.push_back (cmd);
    }
    return true;
}

void NotchController::pushClearLocked (int channel, int index)
{
    auto& n = model_[slotOf (channel, index)];
    if (! n.active)
        return;
    n.active = false;
    outbox_.push_back ({ NotchCommandType::Clear,
                         (std::uint8_t) channel, (std::uint8_t) index,
                         0.0f, 0.0f, 0.0f, slotId_ });
}

void NotchController::clearNotch (int channel, int index)
{
    // width gates the policy surface; internal fan-out loops never exceed it.
    if (channel < 0 || channel >= width_ || index < 0 || index >= kSlots)
        return;
    const std::lock_guard<std::mutex> lock (modelMutex_);
    pushClearLocked (channel, index);
}

void NotchController::clearAll()
{
    const std::lock_guard<std::mutex> lock (modelMutex_);
    for (int c = 0; c < kChannels; ++c)
        for (int i = 0; i < kSlots; ++i)
            pushClearLocked (c, i);
}

int NotchController::adoptPreset (const std::vector<PresetNotch>& notches)
{
    int adopted = 0;
    for (const auto& p : notches)
    {
        // All width_ lanes or neither: a half-applied preset notch would
        // leave one side unprotected while the GUI claims protection.
        // setNotch validates before touching anything, so a failure here
        // means the parameters were rejected -- skip the whole notch.
        int appliedLanes = 0;
        for (int lane = 0; lane < width_; ++lane)
            if (setNotch (lane, p.index, p.freq, p.Q, p.depthDB, Origin::Preset))
                ++appliedLanes;

        if (appliedLanes == width_)
        {
            ++adopted;
        }
        else if (appliedLanes > 0)
        {
            // Cannot happen today (validation depends only on index + params,
            // which are identical across lanes), but if it ever does,
            // unwind the half-applied lanes rather than keep them.
            for (int lane = 0; lane < width_; ++lane)
                clearNotch (lane, p.index);
        }
    }
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

        // --- snapshot publish: identical to today's block, but per lane ---
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
                    notchList[notchCount++] = { (float) n.frequency, (float) n.Q, (float) n.depthDB,
                                                (std::uint8_t) c, (std::uint8_t) i };
                }
        }
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
            ++latest_.sequence;
        }

        // The cross-lane comparison (§4.4) only ever compares the SAME hop:
        // `other` is handed over only when the opposite lane produced a block
        // in THIS drain iteration, and is nullptr otherwise.
        for (int l = 0; l < lanesToRead; ++l)
            if (spec[(std::size_t) l].magnitudes != nullptr)
            {
                const float* other = (lanesToRead == 2 && spec[(std::size_t) (1 - l)].magnitudes != nullptr)
                                         ? spec[(std::size_t) (1 - l)].magnitudes : nullptr;
                processSpectrumForDetection (l, spec[(std::size_t) l], other, now);
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
                    pushClearLocked (c, i);
            }
    }

    // 4. Flush whatever the steps above queued.
    flushOutbox();
}

double NotchController::liveMsForTest() const
{
    const std::lock_guard<std::mutex> lock (modelMutex_);
    return liveMs_;
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
void NotchController::placeConfirmed (int lane, const PeakinessAnalyzer::Candidate& cand, bool linkedNow)
{
    // soundcheckActive() takes modelMutex_ itself, so it is asked BEFORE the
    // lock below -- never underneath it.
    const Origin origin  = soundcheckActive() ? Origin::Soundcheck : Origin::Detector;
    const double q       = notchQ_.load (std::memory_order_relaxed);
    const double depthDb = notchDepthDb_.load (std::memory_order_relaxed);

    int index = -1, firstLane = lane, lastLane = lane;
    {
        const std::lock_guard<std::mutex> lock (modelMutex_);
        if (linkedNow) { index = firstFreeIndexAllLanesLocked(); firstLane = 0; lastLane = width_ - 1; }
        else           { index = firstFreeIndexLocked (lane); }
    }
    if (index < 0)
        return;   // chain full on the lanes concerned: same outcome as today

    int applied = 0;
    for (int l = firstLane; l <= lastLane; ++l)
        if (setNotch (l, index, cand.frequencyHz, q, depthDb, origin))
            ++applied;
    // Partial-failure analysis, unchanged from 1.0.4: setNotch validates only
    // index bounds + params + sample rate, identical across lanes, so a
    // partial application has no realistic trigger. If it ever happens,
    // unwind rather than leave one lane unprotected while the GUI claims
    // protection.
    const int wanted = lastLane - firstLane + 1;
    if (applied != wanted && applied > 0)
        for (int l = firstLane; l <= lastLane; ++l)
            clearNotch (l, index);

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
        float score = la.scorer.scoreCandidate (
            cand, block.magnitudes,
            { locked.data(), locked.size() });
        // Design §4.4: neutral at the default bonus of 1.0.
        score *= asymmetryMultiplier (block.magnitudes, otherLaneMagnitudes, cand.bin,
                                      laneAsymmetryBonus_.load (std::memory_order_relaxed));

        const int bin = cand.bin;
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
                placeConfirmed (lane, cand, linkedNow);
            }
        }
        else
        {
            // Any non-confirming block resets that bin's streak.
            la.persistence[(std::size_t) bin] = 0;
        }
    }

    la.scorer.commitBlock (block.magnitudes, elapsedMs);
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
        l.detector.setSampleRate (sampleRate);
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
