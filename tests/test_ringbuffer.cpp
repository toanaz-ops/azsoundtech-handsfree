// Lock-Free Ring Buffer (SPSC) tests
// Verifies correctness of single-producer / single-consumer ring buffer
// used for real-time audio sample passing between threads.
//
// The tests are in two halves: single-threaded functional tests first, then
// the concurrent SPSC tests required by spec 9.1 at the bottom of the file.
// (This note used to say concurrency "must be validated separately -- see
// Task 5 / Task 8 integration tests". Both of those tasks closed without it.)

#include <gtest/gtest.h>
#include "dsp/LockFreeRingBuffer.h"
#include "dsp/NotchCommand.h"

#include <vector>
#include <numeric>

namespace
{
    constexpr size_t kTestCapacity = 16;
}

TEST(LockFreeRingBuffer, WriteAndReadSingleSample)
{
    LockFreeRingBuffer<int> rb(kTestCapacity);

    int in = 42;
    EXPECT_EQ(rb.write(&in, 1), 1u);
    EXPECT_EQ(rb.getAvailableRead(), 1u);

    int out = 0;
    EXPECT_EQ(rb.read(&out, 1), 1u);
    EXPECT_EQ(out, 42);
    EXPECT_EQ(rb.getAvailableRead(), 0u);
}

TEST(LockFreeRingBuffer, AvailableSpace)
{
    LockFreeRingBuffer<int> rb(kTestCapacity);

    // Empty buffer: full free space available for writing.
    EXPECT_EQ(rb.getAvailableWrite(), kTestCapacity);
    EXPECT_EQ(rb.getAvailableRead(), 0u);

    std::vector<int> data(kTestCapacity / 2, 7);
    EXPECT_EQ(rb.write(data.data(), data.size()), data.size());
    EXPECT_EQ(rb.getAvailableWrite(), kTestCapacity - data.size());
    EXPECT_EQ(rb.getAvailableRead(), data.size());
}

TEST(LockFreeRingBuffer, Wraparound)
{
    // Use a small non-power-of-two capacity to exercise the generic
    // wrap path (no bitmask shortcut).
    constexpr size_t cap = 7;
    LockFreeRingBuffer<int> rb(cap);

    // Fill the buffer then drain -- forces the read index well past the
    // initial write index, so subsequent writes wrap.
    std::vector<int> first(cap, 1);
    EXPECT_EQ(rb.write(first.data(), first.size()), first.size());

    std::vector<int> drain(cap);
    EXPECT_EQ(rb.read(drain.data(), drain.size()), drain.size());
    for (int v : drain) EXPECT_EQ(v, 1);

    // Now write a chunk that must wrap: positions near end + start.
    std::vector<int> second(cap);
    std::iota(second.begin(), second.end(), 100);
    EXPECT_EQ(rb.write(second.data(), second.size()), second.size());

    // Read it back -- order must be preserved.
    std::vector<int> readback(cap);
    EXPECT_EQ(rb.read(readback.data(), readback.size()), readback.size());
    for (size_t i = 0; i < second.size(); ++i)
    {
        EXPECT_EQ(readback[i], second[i]);
    }
}

TEST(LockFreeRingBuffer, MultipleWritesAndReads)
{
    LockFreeRingBuffer<int> rb(kTestCapacity);

    constexpr size_t kBatch = 3;
    std::vector<int> in1(kBatch);
    std::iota(in1.begin(), in1.end(), 0);     // {0, 1, 2}
    std::vector<int> in2(kBatch);
    std::iota(in2.begin(), in2.end(), 10);    // {10, 11, 12}

    EXPECT_EQ(rb.write(in1.data(), in1.size()), in1.size());
    EXPECT_EQ(rb.write(in2.data(), in2.size()), in2.size());

    std::vector<int> out1(kBatch);
    EXPECT_EQ(rb.read(out1.data(), out1.size()), out1.size());
    EXPECT_EQ(out1, in1);

    std::vector<int> out2(kBatch);
    EXPECT_EQ(rb.read(out2.data(), out2.size()), out2.size());
    EXPECT_EQ(out2, in2);

    EXPECT_EQ(rb.getAvailableRead(), 0u);
}

TEST(LockFreeRingBuffer, ReadEmptyReturnsZero)
{
    LockFreeRingBuffer<int> rb(kTestCapacity);

    int out = -1;
    EXPECT_EQ(rb.read(&out, 1), 0u);
    EXPECT_EQ(rb.getAvailableRead(), 0u);

    // Read with a larger count on an empty buffer must also return 0
    // and must not touch the caller's buffer beyond what we own.
    std::vector<int> sink(8, -1);
    EXPECT_EQ(rb.read(sink.data(), sink.size()), 0u);
    for (int v : sink) EXPECT_EQ(v, -1);
}

TEST(LockFreeRingBuffer, WriteFullStopsAtCapacity)
{
    LockFreeRingBuffer<int> rb(kTestCapacity);

    std::vector<int> data(kTestCapacity, 9);
    // Fill it completely (capacity items fit).
    size_t written = rb.write(data.data(), data.size());
    EXPECT_EQ(written, kTestCapacity);
    EXPECT_EQ(rb.getAvailableWrite(), 0u);
    EXPECT_EQ(rb.getAvailableRead(), kTestCapacity);

    // Trying to write one more sample must not block -- returns 0.
    int extra = 1;
    EXPECT_EQ(rb.write(&extra, 1), 0u);
    // And nothing was committed.
    EXPECT_EQ(rb.getAvailableRead(), kTestCapacity);
}
// B2 -- draining the tap across a device restart.
//
// stop() at 48 kHz can leave thousands of samples in the ring. If the device
// restarts at 96 kHz those stale 48 kHz samples are spliced onto the front of
// the first analysis windows, and every bin-to-Hz conversion downstream is
// wrong by up to an octave. clear() discards them.
//
// Precondition (documented at the declaration): neither thread may be running.
// audioDeviceAboutToStart() qualifies -- JUCE inserts the callback into its
// dispatch list only after that call returns.
TEST(LockFreeRingBuffer, ClearDiscardsPendingData)
{
    LockFreeRingBuffer<float> buffer(1024);

    const std::vector<float> stale(400, 1.0f);
    ASSERT_EQ(buffer.write(stale.data(), stale.size()), 400u);
    ASSERT_EQ(buffer.getAvailableRead(), 400u);

    buffer.clear();

    EXPECT_EQ(buffer.getAvailableRead(), 0u);
    EXPECT_EQ(buffer.getAvailableWrite(), buffer.getCapacity());

    // A read after clear() must return nothing, not the discarded samples.
    float scratch[16] = {};
    EXPECT_EQ(buffer.read(scratch, 16), 0u);

    // And the buffer must still be usable: fresh data written after the clear
    // reads back exactly, with no stale prefix in front of it.
    const std::vector<float> fresh { 7.0f, 8.0f, 9.0f };
    ASSERT_EQ(buffer.write(fresh.data(), fresh.size()), 3u);
    float out[3] = {};
    ASSERT_EQ(buffer.read(out, 3), 3u);
    EXPECT_FLOAT_EQ(out[0], 7.0f);
    EXPECT_FLOAT_EQ(out[1], 8.0f);
    EXPECT_FLOAT_EQ(out[2], 9.0f);

    // Clearing an already-empty buffer is a no-op, not a corruption.
    buffer.clear();
    EXPECT_EQ(buffer.getAvailableRead(), 0u);
    EXPECT_EQ(buffer.getAvailableWrite(), buffer.getCapacity());
}

TEST(LockFreeRingBuffer, NotchCommandRoundTripKeepsSlotId)
{
    LockFreeRingBuffer<NotchCommand> ring(kTestCapacity);

    NotchCommand in {};
    in.type = NotchCommandType::Set;
    in.channel = 1;
    in.index = 3;
    in.frequency = 1000.0f;
    in.Q = 8.0f;
    in.depthDB = -12.0f;
    in.slot = 5;

    EXPECT_EQ(ring.write(&in, 1), 1u);

    NotchCommand out {};
    ASSERT_EQ(ring.read(&out, 1), 1u);
    EXPECT_EQ(out.type, in.type);
    EXPECT_EQ(out.slot, 5);
}

//==============================================================================
// Concurrent SPSC tests -- spec 9.1, "lock-free ring buffer khong drop sample
// khi load cao".
//
// Every test above this line is single-threaded, and the note at the top of
// this file deferred concurrency to "Task 5 / Task 8 integration tests". Both
// of those closed without it, so until now the one property this class exists
// for was the one property never tested.
//
// WHAT THESE TESTS PROVE, AND WHAT THEY DO NOT
// ============================================
// They prove that under genuine two-thread contention the index arithmetic,
// the wraparound split and the capacity accounting are correct: nothing is
// lost, nothing is duplicated, and nothing arrives out of order.
//
// They do NOT prove the acquire/release annotations are necessary. On x86-64
// loads are acquire and stores are release in hardware, so weakening the
// orderings would still pass here -- it would fail on ARM. Claiming otherwise
// would be the kind of green-means-correct assertion this project has already
// shipped twice. What guards the orderings is the derivation in the header,
// not this file.
//
// Capacity is deliberately TINY. A small ring wraps constantly and keeps the
// producer pressed against a full buffer, which is the state that exercises
// the modulo split and the free-space calculation. A large ring would run
// nearly empty and test almost nothing.

#include <atomic>
#include <cstdint>
#include <thread>

namespace
{
    // Push `count` monotonically increasing values through `ring` on one
    // thread while another drains it, and verify the consumer observes
    // EXACTLY 0, 1, 2, ... count-1 in order.
    //
    // A single sequence carries four failure modes at once: a lost item makes
    // the next value too large, a duplicate makes it too small, a reordering
    // makes it wrong, and a torn read makes it nonsense. One equality check
    // catches all four.
    //
    // The producer retries on a short write instead of dropping, which is the
    // discipline the detector thread will use on the notch command queue --
    // its producer can retry, unlike the audio thread on the tap.
    void runSpscSequence(std::size_t capacity, std::uint64_t count)
    {
        LockFreeRingBuffer<std::uint64_t> ring(capacity);
        std::atomic<bool>          consumerFailed { false };
        std::atomic<std::uint64_t> consumed       { 0 };
        std::uint64_t              firstBadValue  = 0;
        std::uint64_t              firstBadExpect = 0;

        std::thread consumer([&]
        {
            std::uint64_t expected = 0;
            std::uint64_t chunk[64];

            while (expected < count)
            {
                const std::size_t got = ring.read(chunk, 64);

                for (std::size_t i = 0; i < got; ++i)
                {
                    if (chunk[i] != expected)
                    {
                        if (! consumerFailed.load(std::memory_order_relaxed))
                        {
                            firstBadValue  = chunk[i];
                            firstBadExpect = expected;
                            consumerFailed.store(true, std::memory_order_relaxed);
                        }
                        return;
                    }
                    ++expected;
                }

                consumed.store(expected, std::memory_order_relaxed);

                if (got == 0)
                {
                    std::this_thread::yield();
                }
            }
        });

        std::uint64_t next = 0;
        while (next < count)
        {
            std::uint64_t chunk[64];
            const std::size_t want = static_cast<std::size_t>(
                std::min<std::uint64_t>(64, count - next));

            for (std::size_t i = 0; i < want; ++i)
            {
                chunk[i] = next + i;
            }

            // Retry the remainder rather than dropping it.
            std::size_t offset = 0;
            while (offset < want)
            {
                const std::size_t wrote = ring.write(chunk + offset, want - offset);

                if (wrote == 0)
                {
                    if (consumerFailed.load(std::memory_order_relaxed))
                    {
                        break;   // consumer gave up; do not spin forever
                    }
                    std::this_thread::yield();
                }

                offset += wrote;
            }

            if (consumerFailed.load(std::memory_order_relaxed))
            {
                break;
            }

            next += want;
        }

        consumer.join();

        EXPECT_FALSE(consumerFailed.load())
            << "sequence broke at expected " << firstBadExpect
            << ", got " << firstBadValue
            << " (capacity " << capacity << ")";

        // WITHOUT THIS THE TEST IS VACUOUS. consumerFailed stays false if the
        // consumer never ran, exited early, or read nothing at all, so the
        // check above alone would pass a ring that transported no data
        // whatsoever. Asserting the full count is what makes "nothing was
        // lost" mean anything.
        EXPECT_EQ(consumed.load(), count)
            << "consumer saw " << consumed.load() << " of " << count
            << " items (capacity " << capacity << ")";
    }
}

TEST(LockFreeRingBuffer, ConcurrentSpscLosesNothingWithPowerOfTwoCapacity)
{
    // 64 is the shape the tap uses (kTapCapacity is 8192, also a power of two),
    // where `index % capacity` reduces to a mask and the wraparound split lands
    // on a clean boundary.
    runSpscSequence(64, 1'000'000);
}

TEST(LockFreeRingBuffer, ConcurrentSpscLosesNothingWithNonPowerOfTwoCapacity)
{
    // The riskier shape, and the reason this is a separate test. The class
    // takes an arbitrary std::size_t capacity, and with a non-power-of-two the
    // monotonic indices do NOT wrap at the same point the modulo does. Any
    // conflation of "index" with "index % capacity" survives the power-of-two
    // case and fails here.
    //
    // Measured, not assumed. A deliberate one-item-drop mutation was injected
    // into the producer above: THIS test caught it ("consumer saw 100 of
    // 1000000"), while the capacity-64 and capacity-1 cases stayed green --
    // with a 64-item chunk into a 64-slot ring the write is all-or-nothing, so
    // the mutation had no partial write to corrupt. Capacity 100 is the only
    // one of the three that drives partial writes, which is exactly the path
    // the retry loop exists for.
    runSpscSequence(100, 1'000'000);
}

TEST(LockFreeRingBuffer, ConcurrentSpscSurvivesACapacityOfOne)
{
    // Degenerate ring: every single write fills it and every read empties it,
    // so producer and consumer hand off item by item with no slack at all.
    // Fewer items because the handoff dominates the runtime.
    runSpscSequence(1, 50'000);
}
