// Lock-Free Ring Buffer (SPSC)
//
// Single-Producer / Single-Consumer lock-free ring buffer for passing data
// between two threads without blocking -- safe for real-time audio use
// (audio thread must NEVER block).
//
// Usage: one thread writes via write(), another thread reads via read().
// The producer must be the ONLY caller of write(); the consumer must be
// the ONLY caller of read(). Violating this single-producer /
// single-consumer invariant breaks the lock-free guarantee.
//
// Design notes
// ============
// - The buffer holds up to `capacity` items. Indices monotonically
//   increase forever and are taken modulo capacity for array access.
// - "How many items are present" = writePos - readPos (mod 2^64).
//   The producer can therefore write up to `capacity` items in one
//   call without an extra counter.
// - Cross-thread visibility: the writer stores slot data first, then
//   publishes writePos with release semantics. The reader acquires
//   writePos before reading the slots. The reader's update of readPos
//   (release) lets the writer know how much space is free.
// - The two atomic indices are padded to a typical cache line (64 bytes)
//   to avoid false sharing on hot paths.
// - Pre-allocated in the constructor; write/read never allocate.

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <vector>

template <typename T>
class LockFreeRingBuffer
{
public:
    explicit LockFreeRingBuffer(std::size_t capacity)
        : capacity_(capacity > 0 ? capacity : 1)
        , buffer_(capacity_)
    {
    }

    LockFreeRingBuffer(const LockFreeRingBuffer&)            = delete;
    LockFreeRingBuffer& operator=(const LockFreeRingBuffer&) = delete;
    LockFreeRingBuffer(LockFreeRingBuffer&&)                 = delete;
    LockFreeRingBuffer& operator=(LockFreeRingBuffer&&)      = delete;

    // Producer side. Returns the number of items actually written.
    // Writes are non-blocking: if the buffer has less than `count` free
    // slots, the write is truncated to whatever fits and the remaining
    // input is dropped. This is the audio-thread-safe behaviour -- the
    // caller must check the return value.
    std::size_t write(const T* data, std::size_t count)
    {
        const std::size_t writeIndex = writePos_.load(std::memory_order_relaxed);
        const std::size_t readIndex  = readPos_.load(std::memory_order_acquire);

        const std::size_t available = capacity_ - (writeIndex - readIndex);
        const std::size_t toWrite   = std::min(count, available);
        if (toWrite == 0)
        {
            return 0;
        }

        // First chunk: from write index up to the end of the buffer.
        const std::size_t firstChunk = std::min(toWrite, capacity_ - (writeIndex % capacity_));
        std::copy(data, data + firstChunk, buffer_.begin() + (writeIndex % capacity_));

        // Second chunk (wrap-around): from the start of the buffer.
        if (firstChunk < toWrite)
        {
            std::copy(data + firstChunk, data + toWrite, buffer_.begin());
        }

        // Publish the new write index so the consumer can observe the
        // data we just wrote.
        writePos_.store(writeIndex + toWrite, std::memory_order_release);
        return toWrite;
    }

    // Consumer side. Returns the number of items actually read.
    // Reads are non-blocking: if fewer than `count` items are available,
    // only those are read and the rest of the output buffer is untouched.
    std::size_t read(T* data, std::size_t count)
    {
        const std::size_t writeIndex = writePos_.load(std::memory_order_acquire);
        const std::size_t readIndex  = readPos_.load(std::memory_order_relaxed);

        const std::size_t available = writeIndex - readIndex;
        const std::size_t toRead    = std::min(count, available);
        if (toRead == 0)
        {
            return 0;
        }

        // First chunk: from read index up to the end of the buffer.
        const std::size_t firstChunk = std::min(toRead, capacity_ - (readIndex % capacity_));
        std::copy(buffer_.begin() + (readIndex % capacity_),
                  buffer_.begin() + (readIndex % capacity_) + firstChunk,
                  data);

        // Second chunk (wrap-around): from the start of the buffer.
        if (firstChunk < toRead)
        {
            std::copy(buffer_.begin(), buffer_.begin() + (toRead - firstChunk),
                      data + firstChunk);
        }

        // Publish the new read index so the producer can reclaim slots.
        readPos_.store(readIndex + toRead, std::memory_order_release);
        return toRead;
    }

    std::size_t getAvailableRead() const
    {
        const std::size_t writeIndex = writePos_.load(std::memory_order_acquire);
        const std::size_t readIndex  = readPos_.load(std::memory_order_relaxed);
        return writeIndex - readIndex;
    }

    std::size_t getAvailableWrite() const
    {
        const std::size_t writeIndex = writePos_.load(std::memory_order_relaxed);
        const std::size_t readIndex  = readPos_.load(std::memory_order_acquire);
        return capacity_ - (writeIndex - readIndex);
    }

    std::size_t getCapacity() const noexcept { return capacity_; }

private:
    // Cache-line sized padding to keep the two indices on separate
    // cache lines and avoid false sharing between producer and consumer.
    struct alignas(64) CachePadded
    {
        std::atomic<std::size_t> value;

        CachePadded() noexcept : value(0) {}

        std::size_t load(std::memory_order order) const noexcept
        {
            return value.load(order);
        }

        void store(std::size_t desired, std::memory_order order) noexcept
        {
            value.store(desired, order);
        }
    };

    std::size_t          capacity_;
    std::vector<T>       buffer_;
    CachePadded          writePos_;
    CachePadded          readPos_;
};