#include "app/NotchController.h"

#include <algorithm>

NotchController::NotchController (LockFreeRingBuffer<float>& tap,
                                  LockFreeRingBuffer<NotchCommand>& commands,
                                  ClockSource& clock)
    : juce::Thread ("AZNotchDetector")
    , tap_ (tap)
    , commands_ (commands)
    , clock_ (clock)
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
    if (channel < 0 || channel >= kChannels || index < 0 || index >= kSlots)
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
                             (float) frequency, (float) Q, (float) depthDB };

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
                         0.0f, 0.0f, 0.0f });
}

void NotchController::clearNotch (int channel, int index)
{
    if (channel < 0 || channel >= kChannels || index < 0 || index >= kSlots)
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
        // Both channels or neither: a half-applied preset notch would leave
        // one side unprotected while the GUI claims protection. setNotch
        // validates before touching anything, so a failure here means the
        // parameters were rejected -- skip the whole notch.
        const bool left  = setNotch (0, p.index, p.freq, p.Q, p.depthDB, Origin::Preset);
        const bool right = setNotch (1, p.index, p.freq, p.Q, p.depthDB, Origin::Preset);

        if (left && right)
        {
            ++adopted;
        }
        else if (left != right)
        {
            // Cannot happen today (validation depends only on slot + params,
            // which are identical for both calls), but if it ever does,
            // unwind the half-applied side rather than keep it.
            clearNotch (left ? 0 : 1, p.index);
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

        const std::lock_guard<std::mutex> lock (snapshotMutex_);
        std::copy (block.magnitudes, block.magnitudes + Detector::kNumBins,
                   latest_.magnitudes.begin());
        latest_.magnitudeCount = (std::uint32_t) Detector::kNumBins;
        latest_.sampleRate     = block.sampleRate;
        latest_.notches        = notchList;
        latest_.notchCount     = notchCount;
        ++latest_.sequence;
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
                if (n.active && (liveMs_ - n.lastDetectedMs) > kAutoReleaseMs)
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
