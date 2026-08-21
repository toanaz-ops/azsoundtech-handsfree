# Combined Tasks 5-7: DSP Core (NotchCommand + Biquad + NotchChain)

## Context

This is a combined implementation of 3 tightly-coupled DSP components. They must be implemented together because:
- Task 5 (NotchCommand) is a simple POD struct used by Task 7
- Task 6 (Biquad) is the math foundation for Task 7
- Task 7 (NotchChain) uses both Biquad and LockFreeRingBuffer<NotchCommand>

## Architecture

```
LockFreeRingBuffer<NotchCommand>  ←  Task 4 (already done)
       ↓ used by
NotchChain (Task 7)  ← uses →  Biquad (Task 6)
                                   ↑
                            Uses RBJ Audio EQ Cookbook
                            notch filter formulas
```

---

## Task 5: NotchCommand Struct

### File
- Create: `src/dsp/NotchCommand.h`

### Requirements

**Purpose:** Command struct passed via LockFreeRingBuffer from detector thread to audio thread.

**Exact specification:**
```cpp
#pragma once

enum class NotchCommandType : uint8_t
{
    Set,
    Clear
};

struct NotchCommand
{
    NotchCommandType type;
    uint8_t channel;    // 0 = Left, 1 = Right
    uint8_t index;      // 0-15 (notch slot in chain)
    float frequency;    // Hz, only valid for Set
    float Q;            // Quality factor, only valid for Set
    float depthDB;      // Depth in dB, only valid for Set
};
```

**Constraints:**
- POD type (no virtual methods, no constructors needed)
- Small (should fit in cache line, ~24 bytes)
- LockFreeRingBuffer<NotchCommand> must compile cleanly

### Tests
No dedicated test - covered by integration tests in Task 7/8.

---

## Task 6: Biquad Filter Implementation

### Files
- Create: `src/dsp/Biquad.h`
- Create: `src/dsp/Biquad.cpp`
- Create: `tests/test_biquad.cpp`

### Requirements

**Purpose:** Single biquad IIR filter implementing notch filter from RBJ Audio EQ Cookbook.

**Class interface:**
```cpp
class Biquad
{
public:
    Biquad();
    
    void setNotchFilter(double freq, double Q, double sampleRate);
    double processSample(double input);
    void reset();
    
private:
    // Coefficients (normalized, a0=1)
    double b0_, b1_, b2_;
    double a1_, a2_;
    
    // State variables (Direct Form I)
    double z1_, z2_;
};
```

**RBJ Notch filter formulas (verbatim from cookbook):**
```
omega = 2 * pi * freq / sampleRate
alpha = sin(omega) / (2 * Q)

b0 = 1
b1 = -2 * cos(omega)
b2 = 1
a0 = 1 + alpha
a1 = -2 * cos(omega)
a2 = 1 - alpha

Normalize: divide all by a0
```

**Processing (Direct Form I, transposed):**
```
output = b0 * input + z1
z1 = b1 * input - a1 * output + z2
z2 = b2 * input - a2 * output
```

**Reset:**
- z1_ = z2_ = 0.0

### Tests Required

Create `tests/test_biquad.cpp` with these tests:

```cpp
TEST(Biquad, NotchAttenuatesTargetFrequency)
TEST(Biquad, NotchPassesOffTargetFrequency)
TEST(Biquad, ResetClearsState)
TEST(Biquad, HandlesDifferentSampleRates)
TEST(Biquad, CoefficientValidity)
```

**Test thresholds:**
- At target freq, output < 0.25 (about -12dB or more attenuation)
- Off target (1 octave away), gain ratio > 0.9
- After reset, DC passthrough (output ≈ input)

---

## Task 7: NotchChain (16 filters per channel)

### Files
- Create: `src/dsp/NotchChain.h`
- Create: `src/dsp/NotchChain.cpp`
- Create: `tests/test_notchchain.cpp`

### Requirements

**Purpose:** Container for 16 Biquad filters in series, supporting per-notch enable/disable without reallocation.

**Class interface:**
```cpp
class NotchChain
{
public:
    static constexpr int MAX_NOTCHES = 16;
    
    enum class NotchState
    {
        Idle,
        Active
    };
    
    struct NotchInfo
    {
        double frequency = 0.0;
        double Q = 0.0;
        double depthDB = 0.0;
        NotchState state = NotchState::Idle;
    };
    
    explicit NotchChain(double sampleRate);
    
    double processSample(double input);
    void setNotch(int index, double freq, double Q, double depthDB);
    void clearNotch(int index);
    void reset();
    
    const NotchInfo& getNotchInfo(int index) const;
    int getActiveNotchCount() const;

private:
    double sampleRate_;
    std::array<Biquad, MAX_NOTCHES> filters_;
    std::array<NotchInfo, MAX_NOTCHES> notchInfo_;
};
```

**Behavior:**
- `setNotch`: Configure filter i with new coefficients, mark Active
- `clearNotch`: Mark Idle (filter stays configured but bypassed)
- `processSample`: Process through all Active filters in series
- Bypass Idle filters (input passes through unchanged)
- `getActiveNotchCount`: Count Active notches

**Critical:**
- No allocation in processSample (pre-allocate in ctor)
- Bypass check must be cheap (single enum compare)
- Process loop should be cache-friendly

### Tests Required

Create `tests/test_notchchain.cpp`:

```cpp
TEST(NotchChain, InitiallyPassthrough)
TEST(NotchChain, SetNotchAttenuates)
TEST(NotchChain, ClearNotchRestoresPassthrough)
TEST(NotchChain, MultipleActiveNotches)
TEST(NotchChain, MaxNotches16)
TEST(NotchChain, ActiveNotchCount)
TEST(NotchChain, OutOfRangeIndexIgnored)
```

---

## CMakeLists.txt Updates

### Root CMakeLists.txt

Add to `target_sources(HandsFree PRIVATE`:
```cmake
    src/dsp/Biquad.cpp
    src/dsp/Biquad.h
    src/dsp/NotchChain.cpp
    src/dsp/NotchChain.h
```

### tests/CMakeLists.txt

Add to `add_executable(HandsFreeTests`:
```cmake
    test_biquad.cpp
    test_notchchain.cpp
```

---

## Test Strategy (TDD)

**For each task:**
1. Write failing test FIRST
2. Run to verify RED
3. Implement minimum code
4. Run to verify GREEN
5. Commit

**Final verification:**
```bash
cmake --build build --config Release
cd build
ctest -C Release --output-on-failure
```

**Expected:** All tests PASS (6 ring buffer + 5 biquad + 7 notch chain = 18 total tests)

---

## Commit Strategy

Use ONE commit per task (3 commits total):
- Task 5: `feat: add NotchCommand struct for detector→audio communication`
- Task 6: `feat: add biquad notch filter with RBJ coefficients and tests`
- Task 7: `feat: add notch chain with 16 filters per channel and tests`

---

## Critical Quality Requirements

1. **No allocation in processSample** - audio thread is real-time, no malloc allowed
2. **RBJ formulas exact** - copy from cookbook verbatim, do not "improve"
3. **Direct Form I** - standard biquad form, stable and well-understood
4. **Constexpr MAX_NOTCHES** - 16, matches spec exactly
5. **Thread safety** - NotchChain methods called only from audio thread (single-threaded usage assumed for now)

---

## Success Criteria

1. NotchCommand.h exists with exact struct definition
2. Biquad class implements notch filter using RBJ formulas
3. NotchChain manages 16 Biquads with set/clear/bypass logic
4. All 18 tests PASS
5. CMakeLists.txt files updated correctly
7. 3 commits made with exact messages above
8. Report file at: `D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\.superpowers\sdd\2026-08-19-az-soundtech-hands-free\tasks-5-7-report.md`

## Report Contract

Status: DONE | DONE_WITH_CONCERNS | NEEDS_CONTEXT | BLOCKED
- Per-task status
- Files created (line counts)
- Test results per task (must show all PASS)
- Commits made (3 hashes + messages)
- Self-review findings (RBJ math correctness, no-allocation verification)
- Concerns
