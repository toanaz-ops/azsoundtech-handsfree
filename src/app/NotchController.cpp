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
            latest_.linked         = false;   // Task 3 adds linked_
            latest_.sampleRate     = spec[0].magnitudes != nullptr ? spec[0].sampleRate : spec[1].sampleRate;
            latest_.notches        = notchList;
            latest_.notchCount     = notchCount;
            ++latest_.sequence;
        }

        for (int l = 0; l < lanesToRead; ++l)
            if (spec[(std::size_t) l].magnitudes != nullptr)
                processSpectrumForDetection (l, spec[(std::size_t) l], now);
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

int NotchController::firstFreeSlotLocked() const
{
    for (int i = 0; i < kSlots; ++i)
        if (! model_[slotOf (0, i)].active)
            return i;
    return -1;
}

void NotchController::processSpectrumForDetection (int lane, const Detector::Spectrum& block,
                                                   double blockNowMs)
{
    if (! detectionActive_.load (std::memory_order_relaxed))
        return;

    auto& la = lanes_[(std::size_t) lane];

    // Real gap since the previous DRAINED block; the first block has no
    // predecessor and passes 0 (the EMA deliberately skips zero-dt updates).
    const double rawDt     = blockNowMs - la.previousBlockNowMs;
    const double elapsedMs = (la.previousBlockNowMs > 0.0 && rawDt > 0.0) ? rawDt : 0.0;
    la.previousBlockNowMs  = blockNowMs;

    la.scorer.beginBlock (block.sampleRate);

    // Feed auto-release FIRST so a still-ringing locked notch stays fed by the
    // same frame the scorer looks at (spec 5.2 step 7).
    {
        const std::lock_guard<std::mutex> lock (modelMutex_);
        const double binWidthHz = block.sampleRate / (double) Detector::kFftSize;
        for (int c = 0; c < kChannels; ++c)
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

    // Locked fundamentals for the harmonic penalty (KD-3).
    std::vector<double> locked;
    {
        const std::lock_guard<std::mutex> lock (modelMutex_);
        for (const auto& n : model_)
            if (n.active)
                locked.push_back (n.frequency);
    }

    const auto result = la.analyzer.analyse (block);
    for (std::size_t i = 0; i < result.count && i < (std::size_t) Detector::kNumBins; ++i)
    {
        const auto& cand = result.candidates[i];
        const float score = la.scorer.scoreCandidate (
            cand, block.magnitudes,
            { locked.data(), locked.size() });

        const int bin = cand.bin;
        if (score > CandidateScorer::kConfirmScore)
        {
            const std::uint32_t requiredBlocks =
                (std::uint32_t) persistenceBlocks_.load (std::memory_order_relaxed);
            if (++la.persistence[(std::size_t) bin] >= requiredBlocks)
            {
                la.persistence[(std::size_t) bin] = 0;

                // KD-5: automatic notch params were fixed; since the
                // 2026-08-24 brief they are runtime defaults set from the
                // DETECTION panel. KD-6: first index whose lane-0 model notch
                // is inactive; ALL width_ lanes take that SAME index, or
                // nothing.
                const int slot = firstFreeSlotLocked();
                if (slot >= 0)
                {
                    const Origin origin = soundcheckActive() ? Origin::Soundcheck
                                                             : Origin::Detector;
                    const double q      = notchQ_.load (std::memory_order_relaxed);
                    const double depthDb= notchDepthDb_.load (std::memory_order_relaxed);
                    int appliedLanes = 0;
                    for (int l = 0; l < width_; ++l)
                        if (setNotch (l, slot, cand.frequencyHz,
                                      q, depthDb, origin))
                            ++appliedLanes;
                    // Partial-failure analysis: setNotch validates only index
                    // bounds + params + sample rate, identical across all
                    // lanes, so a partial application has no realistic
                    // trigger today (same reasoning as adoptPreset). If it
                    // ever happens, unwind the half-applied lanes rather than
                    // leave one lane unprotected while the GUI claims
                    // protection.
                    if (appliedLanes != width_ && appliedLanes > 0)
                        for (int l = 0; l < width_; ++l)
                            clearNotch (l, slot);

                    // Brief change 3: one light line per detection event. The
                    // detector thread already logs elsewhere, this allocates
                    // a handful of short strings ONCE per placed notch (not
                    // per frame), and "rise" is the configured rise reference
                    // the confirmation ran under.
                    juce::Logger::writeToLog (
                        "[detect] slot=" + juce::String (slotId_)
                        + " lane=" + juce::String (appliedLanes)
                        + " freq=" + juce::String ((int) std::lround (cand.frequencyHz))
                        + " Q=" + juce::String (q, 1)
                        + " depth=" + juce::String (depthDb, 1)
                        + " rise=" + juce::String ((int) std::lround (
                              la.scorer.getRiseReferenceMs())));
                }
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
