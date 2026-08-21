# Task 4: Lock-Free Ring Buffer (SPSC)

## Requirements

Implement a single-producer single-consumer (SPSC) lock-free ring buffer for real-time audio data passing between audio and detector threads.

## Files to Create

- `src/dsp/LockFreeRingBuffer.h` - Template class implementation
- `tests/test_ringbuffer.cpp` - GoogleTest unit tests
- `tests/CMakeLists.txt` - Test build configuration

## Exact Specifications (from spec)

**Critical constraints:**
- **Zero allocation in steady state** - pre-allocate buffer in constructor
- **Lock-free** - must not use mutex or any blocking synchronization
- **SPSC pattern** - one thread writes, one thread reads (no concurrent writers/readers)
- **Memory ordering**: acquire/release semantics for cross-thread visibility
- **Used for**: Audio tap (audio→detector) AND notch commands (detector→audio)

**Type:** Template `<typename T>` to support both `float` (audio samples) and `NotchCommand` (Task 5)

## Class Interface

**Constructor:**
- `explicit LockFreeRingBuffer(size_t capacity)` - pre-allocate buffer

**Methods:**
- `size_t write(const T* data, size_t count)` - Write samples, returns actual count written
- `size_t read(T* data, size_t count)` - Read samples, returns actual count read
- `size_t getAvailableRead() const` - Number of samples available to read
- `size_t getAvailableWrite() const` - Free space for writing

**Capacity:** Power of 2 recommended but must handle arbitrary capacity correctly (no modulo via bitmask unless power of 2)

## Implementation Requirements

**Atomic indices:**
- `std::atomic<size_t> writePos_` and `readPos_`
- Writer updates writePos with release semantics
- Reader updates readPos with release semantics
- Cross-thread reads use acquire semantics

**Wraparound handling:**
- Split copy into two parts: end-of-buffer + start-of-buffer
- First chunk: `min(toWrite, capacity - writeIndex)`
- Second chunk: remaining (if wrapping)

**Performance:**
- No memory allocation in write/read
- Use `std::copy` (compiler optimizes to memcpy for POD types)
- Cache line padding to avoid false sharing between indices (optional but recommended)

## Test Cases (GoogleTest)

Create `tests/test_ringbuffer.cpp`:

```cpp
TEST(LockFreeRingBuffer, WriteAndReadSingleSample)
TEST(LockFreeRingBuffer, AvailableSpace)
TEST(LockFreeRingBuffer, Wraparound)
TEST(LockFreeRingBuffer, MultipleWritesAndReads)
TEST(LockFreeRingBuffer, ReadEmptyReturnsZero)
TEST(LockFreeRingBuffer, WriteFullStopsAtCapacity)
```

## Test CMakeLists.txt

Create `tests/CMakeLists.txt`:

```cmake
include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.14.0
)
FetchContent_MakeAvailable(googletest)

add_executable(HandsFreeTests test_ringbuffer.cpp)

target_link_libraries(HandsFreeTests PRIVATE
    GTest::gtest_main
)

target_include_directories(HandsFreeTests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

include(GoogleTest)
gtest_discover_tests(HandsFreeTests)
```

## Test Strategy

1. Configure GoogleTest via FetchContent
2. Build test executable
3. Run all tests via ctest
4. Expected: All 6 tests PASS
5. Command: `cmake --build build --config Release; cd build; ctest -C Release --output-on-failure`

## Critical Considerations

**Thread safety verification:**
- Single-threaded tests verify correctness
- Document: "Production usage requires one writer thread, one reader thread"
- Do NOT test multi-threaded behavior in this task (Task 5+ will handle SPSC integration tests)

**Capacity sizing:**
- For audio: 48000 samples/sec × 0.1 sec = 4800 samples (100ms buffer)
- For commands: 16 notches × 10 Hz = small queue (32 entries typical)

## Success Criteria

1. `src/dsp/LockFreeRingBuffer.h` created with template class
2. `tests/test_ringbuffer.cpp` created with 6 test cases
3. `tests/CMakeLists.txt` created with GoogleTest setup
5. CMake configuration succeeds (with MSVC toolchain)
6. All 6 GoogleTest tests pass
7. Commit with message: "feat: add lock-free SPSC ring buffer with tests"

## Interfaces Produced

- `template<typename T> class LockFreeRingBuffer` ready for:
  - Task 5: `LockFreeRingBuffer<NotchCommand>` for control commands
  - Task 8: `LockFreeRingBuffer<float>` for audio tap (via wrapper)
