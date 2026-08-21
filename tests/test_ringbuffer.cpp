// Lock-Free Ring Buffer (SPSC) tests
// Verifies correctness of single-producer / single-consumer ring buffer
// used for real-time audio sample passing between threads.
//
// NOTE: These are single-threaded functional tests only.
// Thread safety under concurrent SPSC usage must be validated separately
// (see Task 5 / Task 8 integration tests).

#include <gtest/gtest.h>
#include "dsp/LockFreeRingBuffer.h"

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
