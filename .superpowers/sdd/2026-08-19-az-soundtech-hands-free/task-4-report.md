# Task 4 Report: Lock-Free Ring Buffer (SPSC)

**Status:** DONE_WITH_CONCERNS

## Summary

Implemented a header-only SPSC lock-free ring buffer for real-time audio
data passing between threads. Pre-allocated, lock-free, single-producer
single-consumer. Tests use GoogleTest fetched via CMake `FetchContent`.
All 6 unit tests pass on MSVC toolchain (VS 2026 BuildTools 18.7, MSVC
19.51.36248).

## Files created

| File                              | Lines | Purpose                                        |
| --------------------------------- | ----- | ---------------------------------------------- |
| `src/dsp/LockFreeRingBuffer.h`    | 155   | Template `LockFreeRingBuffer<T>` implementation |
| `tests/test_ringbuffer.cpp`       | 121   | 6 GoogleTest unit tests                        |
| `tests/CMakeLists.txt`            | 25    | FetchContent + gtest_discover_tests            |

Root `CMakeLists.txt` already had `enable_testing()` and `add_subdirectory(tests)` — no edit required.

## Test results

Command: `cmake --build build --config Release; cd build; ctest -C Release --output-on-failure`

```
1/6 Test #1: LockFreeRingBuffer.WriteAndReadSingleSample ...   Passed    0.01 sec
2/6 Test #2: LockFreeRingBuffer.AvailableSpace .............   Passed    0.01 sec
3/6 Test #3: LockFreeRingBuffer.Wraparound .................   Passed    0.01 sec
4/6 Test #4: LockFreeRingBuffer.MultipleWritesAndReads .....   Passed    0.01 sec
5/6 Test #5: LockFreeRingBuffer.ReadEmptyReturnsZero .......   Passed    0.01 sec
6/6 Test #6: LockFreeRingBuffer.WriteFullStopsAtCapacity ...   Passed    0.01 sec

100% tests passed, 0 tests failed out of 6

Total Test time (real) =   0.06 sec
```

All 6 tests **PASS**.

## Commit

- Hash: `3910752`
- Message: `feat: add lock-free SPSC ring buffer with tests`
- Files: 3 (320 insertions)

## Design notes

**Full capacity usable.** Unlike the common "waste one slot" SPSC pattern,
this implementation uses the difference `writePos - readPos` as the count
and allows storing up to `capacity` items. The brief's
"WriteFullStopsAtCapacity" test name implies the full capacity should be
usable; that is the convention chosen here.

**Memory ordering.** Symmetric acquire/release chain between producer and
consumer:
- Producer: relaxed-load `writePos` (only it writes), acquire-load
  `readPos` (see consumer's release), store data, release-store
  `writePos` (publish).
- Consumer: acquire-load `writePos` (see producer's release), relaxed-load
  `readPos` (only it writes), read data, release-store `readPos`.

**Wraparound.** Generic modulo (not bitmask) handles non-power-of-two
capacities correctly. Each write/read is split into at most two
`std::copy` calls (end-of-buffer + start-of-buffer). `std::copy` lowers to
`memcpy` for trivially-copyable T (e.g. `float`, `NotchCommand`).

**Cache-line padding.** Indices are wrapped in `alignas(64)` structs so
the writer's cache line is separate from the reader's, avoiding false
sharing on the hot path.

**Zero allocation in steady state.** Buffer is a `std::vector<T>`
constructed once in the ctor. `write`/`read` only do pointer arithmetic +
`std::copy`.

**Self-review — thread safety.** Single-threaded tests only — they verify
*functional* correctness of the ring arithmetic, not concurrent safety.
SPSC concurrent safety is documented as a contract (only one writer
thread, only one reader thread). Real concurrent testing is the
responsibility of Task 5 / Task 8 integration tests, as the brief
explicitly states.

**Self-review — wraparound correctness.** Verified by the `Wraparound`
test which uses a deliberately non-power-of-two capacity (7) and walks the
write/read indices around the buffer boundary in both write and read
directions. The chunk split is `min(toWrite, capacity_ - index%capacity_)`
followed by a possible remainder, exactly per the brief.

**Self-review — capacity sizing.** No `is_power_of_two` check is enforced;
the buffer correctly handles any positive capacity because the wrap math
uses `% capacity_`, not a bitmask.

## Concerns

1. **Pre-existing build issue unrelated to Task 4.** The root
   `HandsFree` (JUCE GUI app) target fails to compile with
   `error C1083: Cannot open include file: 'JuceHeader.h'`. This error
   predates Task 4 — verified by reverting my changes and reproducing the
   same error. It is outside Task 4's scope. The tests target builds and
   runs cleanly.

2. **Full-capacity vs. one-slot-short.** The brief name
   "WriteFullStopsAtCapacity" was interpreted as "the full capacity is
   usable; further writes return 0". This is the more user-friendly
   behaviour but differs from the more conservative "waste one slot" SPSC
   idiom. If downstream code (Task 5 / 8) needs the latter, the
   constructor can clamp `capacity_` to `capacity - 1` or the tests can be
   rewritten.

3. **`alignas(64)` on inner struct without explicit padding bytes.** The
   standard guarantees the struct is aligned to 64 bytes; if its member
   is smaller (8 bytes here), the compiler pads the struct's *size* up to
   64. This is well-defined but not visually obvious from the source. If
   a future maintainer worries about this, add an explicit
   `char padding[N];` member — no behaviour change.

4. **`getAvailableRead`/`getAvailableWrite` load `acquire` even though
   the typical call sites are single-threaded.** The acquire is needed
   for correctness when called from the other thread (a real consumer
   may call `getAvailableWrite` to size a batch, and vice-versa). Cost
   is negligible on x86 (acquire is a no-op) and correct on ARM/weak
   memory models.

## How to build & test

```cmd
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release --target HandsFreeTests
cd build
ctest -C Release --output-on-failure
```