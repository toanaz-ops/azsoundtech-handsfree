#include "app/NotchController.h"

NotchController::NotchController (LockFreeRingBuffer<float>& tap,
                                  LockFreeRingBuffer<NotchCommand>& commands,
                                  ClockSource& clock)
    : tap_ (tap)
    , commands_ (commands)
    , clock_ (clock)
    , detector_ (48000.0)
{
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
        n.frequency = frequency;
        n.Q         = Q;
        n.depthDB   = depthDB;
        n.origin    = origin;
        n.active    = true;
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

void NotchController::runOnce()
{
    flushOutbox();
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
