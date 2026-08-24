#include "app/NotchController.h"

#include <algorithm>
#include <cmath>

NotchController::NotchController (LockFreeRingBuffer<float>& tap,
                                  LockFreeRingBuffer<NotchCommand>& commands,
                                  ClockSource& clock,
                                  int slotId)
    : juce::Thread ("AZNotchDetector")
    , tap_ (tap)
    , commands_ (commands)
    , clock_ (clock)
    , slotId_ (slotId)
    , detector_ (48000.0)
    , lastPollMs_ (clock.nowMs())
{
}

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
    const double sampleRate = detector_.getSampleRate();
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
    for (auto block = detector_.processLatestBlock (tap_);
         block.magnitudes != nullptr;
         block = detector_.processLatestBlock (tap_))
    {
        // Capture model state under the model lock FIRST, release, THEN take
        // the snapshot lock -- the two mutexes are never held together.
        std::array<SnapshotNotch, kTotalSlots> notchList {};
        std::uint32_t notchCount = 0;
        double liveNow = 0.0;
        {
            const std::lock_guard<std::mutex> lock (modelMutex_);
            lastDataMs_ = now;
            liveNow     = liveMs_;
            for (int c = 0; c < kChannels; ++c)
                for (int i = 0; i < kSlots; ++i)
                {
                    const auto& n = model_[slotOf (c, i)];
                    if (! n.active)
                        continue;
                    notchList[notchCount] = { (float) n.frequency, (float) n.Q,
                                              (float) n.depthDB,
                                              (std::uint8_t) c, (std::uint8_t) i };
                    ++notchCount;
                }
        }
        (void) liveNow;

        {
        const std::lock_guard<std::mutex> lock (snapshotMutex_);
        std::copy (block.magnitudes, block.magnitudes + Detector::kNumBins,
                   latest_.magnitudes.begin());
        latest_.magnitudeCount = (std::uint32_t) Detector::kNumBins;
        latest_.sampleRate     = block.sampleRate;
        latest_.notches        = notchList;
        latest_.notchCount     = notchCount;
        ++latest_.sequence;
        }

        // Detection policy for THIS block, while its magnitudes are still
        // alive (they die at the next processLatestBlock call).
        processSpectrumForDetection (block, now);
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

void NotchController::processSpectrumForDetection (const Detector::Spectrum& block,
                                                   double blockNowMs)
{
    if (! detectionActive_.load (std::memory_order_relaxed))
        return;

    // Real gap since the previous DRAINED block; the first block has no
    // predecessor and passes 0 (the EMA deliberately skips zero-dt updates).
    const double rawDt     = blockNowMs - previousBlockNowMs_;
    const double elapsedMs = (previousBlockNowMs_ > 0.0 && rawDt > 0.0) ? rawDt : 0.0;
    previousBlockNowMs_    = blockNowMs;

    scorer_.beginBlock (block.sampleRate);

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
                if (PeakinessAnalyzer::peakinessAt (block.magnitudes,
                                                    Detector::kNumBins, bin)
                        > PeakinessAnalyzer::kDefaultThreshold)
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

    if (persistence_.size() != (std::size_t) Detector::kNumBins)
        persistence_.assign ((std::size_t) Detector::kNumBins, 0);

    const auto result = analyzer_.analyse (block);
    for (std::size_t i = 0; i < result.count && i < (std::size_t) Detector::kNumBins; ++i)
    {
        const auto& cand = result.candidates[i];
        const float score = scorer_.scoreCandidate (
            cand, block.magnitudes,
            { locked.data(), locked.size() });

        const int bin = cand.bin;
        if (score > CandidateScorer::kConfirmScore)
        {
            if (++persistence_[(std::size_t) bin] >= (std::uint32_t) kPersistenceBlocks)
            {
                persistence_[(std::size_t) bin] = 0;

                // KD-5: automatic notch params are fixed. KD-6: first index
                // whose lane-0 model notch is inactive; ALL width_ lanes take
                // that SAME index, or nothing.
                const int slot = firstFreeSlotLocked();
                if (slot >= 0)
                {
                    const Origin origin = soundcheckActive() ? Origin::Soundcheck
                                                             : Origin::Detector;
                    int appliedLanes = 0;
                    for (int lane = 0; lane < width_; ++lane)
                        if (setNotch (lane, slot, cand.frequencyHz,
                                      30.0, -12.0, origin))
                            ++appliedLanes;
                    // Partial-failure analysis: setNotch validates only index
                    // bounds + params + sample rate, identical across all
                    // lanes, so a partial application has no realistic
                    // trigger today (same reasoning as adoptPreset). If it
                    // ever happens, unwind the half-applied lanes rather than
                    // leave one lane unprotected while the GUI claims
                    // protection.
                    if (appliedLanes != width_ && appliedLanes > 0)
                        for (int lane = 0; lane < width_; ++lane)
                            clearNotch (lane, slot);
                }
            }
        }
        else
        {
            // Any non-confirming block resets that bin's streak.
            persistence_[(std::size_t) bin] = 0;
        }
    }

    scorer_.commitBlock (block.magnitudes, elapsedMs);
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
    detector_.setSampleRate (sampleRate);
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
