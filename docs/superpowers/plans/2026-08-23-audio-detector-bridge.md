# Audio–Detector Bridge Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Xây kênh giao tiếp giữa audio thread và detector thread theo design đã
duyệt: `NotchController` sở hữu notch model + đồng hồ sống, `AudioEngine` sở hữu
hàng đợi lệnh lock-free, GUI đọc snapshot qua mutex.

**Architecture:** Hai kênh lock-free duy nhất, đều chạm audio thread: SPSC
`NotchCommand` (detector→audio, 128 slot, drain ≤64/callback) và SPSC tap float
(audio→detector, đã có). Mọi đường còn lại là mutex thường. Detector là tác giả
duy nhất của mọi Set/Clear (D-05); timer auto-release đo wall-clock nhưng chỉ
chạy khi tap còn sống (D-06).

**Tech Stack:** C++17, JUCE 9 (`juce::Thread`, `juce::Time`), GoogleTest 1.14,
CMake + Visual Studio 18 2026.

**Spec:** `docs/superpowers/specs/2026-08-22-audio-detector-bridge-design.md`
(+ 3 bổ sung đã duyệt ngày 2026-08-23: predicate depthDB, ghi chú float/double,
mục Lifecycle thứ tự shutdown). Plan này lập từ phiên review 2026-08-23; HEAD
lúc lập: `e7f85f4`.

## Global Constraints

- Audio callback (`audioDeviceIOCallbackWithContext`): **không allocation, không
  lock, không logging** — bất biến hiện có của `AudioEngine`.
- SPSC bất biến: tap — producer = audio thread, consumer = detector thread;
  command queue — producer = detector thread, consumer = audio thread.
- Không xoá validation / NaN / denormal / bounds check nào để gọn code.
- Detector validate **trước khi gửi**: `sampleRate > 0 && Q > 0 &&
  0 < freq < sampleRate/2 && depthDB <= 0` (predicate cuối là bổ sung đã duyệt —
  biquad 4 tham số từ chối depth dương vì sẽ boost thay vì cắt).
- `ModelNotch` dùng `double`, `NotchCommand` dùng `float` — conversion một chiều
  float→double là có chủ đích, không "sửa nhất quán" hai chiều.
- Build: `cmake -B build -G "Visual Studio 18 2026" -A x64` rồi
  `cmake --build build --config Release`; test `cd build && ctest -C Release`.
  Đổi header/CMake → full reconfigure.
- Commit với đường dẫn tường minh. Docs-only được commit không cần hỏi; code
  commit theo từng task sau khi test xanh.
- Mọi thay đổi audio-path đánh dấu **"needs a human listen"** — build xanh không
  chứng minh ổn định.
- File viết bằng PowerShell/Python phải set UTF-8 tường minh.

---

### Task 0: Cập nhật design doc + lưu plan này (docs-only)

**Files:**
- Modify: `docs/superpowers/specs/2026-08-22-audio-detector-bridge-design.md`
- Create: `docs/superpowers/plans/2026-08-23-audio-detector-bridge.md`

- [x] **Step 1: Sửa 4 chỗ đã duyệt**
  1. §3: thêm `depthDB <= 0` vào danh sách predicate detector phải validate.
  2. §3: ghi chú float/double một chiều có chủ đích.
  3. Mục mới **§6.5 Lifecycle**: stop controller thread trước khi clear ring.
  4. §9: đánh dấu prerequisite depthDB ĐÃ LAND.
- [x] **Step 2: Lưu plan này.**
- [x] **Step 3: Commit docs**
```bash
git add docs/superpowers/specs/2026-08-22-audio-detector-bridge-design.md docs/superpowers/plans/2026-08-23-audio-detector-bridge.md
git commit -m "docs(bridge): owner-approved amendments + implementation plan"
```

---

### Task 1: ClockSource

**Files:**
- Create: `src/dsp/ClockSource.h` (header-only)
- Test: `tests/test_clocksource.cpp`
- Modify: `tests/CMakeLists.txt` (thêm `test_clocksource.cpp` vào
  `add_executable(HandsFreeTests ...)`)

**Interfaces:**
- Produces: `class ClockSource { virtual double nowMs() const = 0; }`,
  `class JuceMonotonicClock final : public ClockSource`. Task 2+ tiêu thụ.

- [ ] **Step 1: Viết test thất bại**

```cpp
// tests/test_clocksource.cpp
#include <gtest/gtest.h>
#include <juce_events/juce_events.h>
#include "dsp/ClockSource.h"

namespace {
class FakeClock : public ClockSource {
public:
    double nowMs() const override { return ms_; }
    void advance (double ms) { ms_ += ms; }
private:
    double ms_ = 1000.0;
};
}

TEST (ClockSource, FakeAdvancesOnCommand)
{
    FakeClock clock;
    EXPECT_DOUBLE_EQ (clock.nowMs(), 1000.0);
    clock.advance (30000.0);
    EXPECT_DOUBLE_EQ (clock.nowMs(), 31000.0);
}

TEST (ClockSource, JuceClockIsMonotonic)
{
    JuceMonotonicClock clock;
    const double a = clock.nowMs();
    juce::Thread::sleep (5);
    const double b = clock.nowMs();
    EXPECT_GE (b, a);
}
```

- [ ] **Step 2: Chạy test xác nhận FAIL** — lỗi compile: header chưa tồn tại.
- [ ] **Step 3: Viết implementation**

```cpp
// src/dsp/ClockSource.h
#pragma once
#include <juce_core/juce_core.h>

// Injectable wall clock (design §4). Deliberately NOT std::function<double()>:
// it may heap-allocate, and this is called in the detector's hot loop.
class ClockSource
{
public:
    virtual ~ClockSource() = default;
    virtual double nowMs() const = 0;
};

// Production impl: JUCE's high-res monotonic counter (not wall UTC -- immune
// to system clock changes mid-show).
class JuceMonotonicClock final : public ClockSource
{
public:
    double nowMs() const override { return juce::Time::getMillisecondCounterHiRes(); }
};
```

- [ ] **Step 4: Build + test PASS.**
- [ ] **Step 5: Commit**
```bash
git add src/dsp/ClockSource.h tests/test_clocksource.cpp tests/CMakeLists.txt
git commit -m "feat(bridge): injectable ClockSource with JUCE monotonic backing"
```

---

### Task 2: NotchController — model, validation, outbox (chưa có thread)

**Files:**
- Create: `src/app/NotchController.h`, `src/app/NotchController.cpp`
- Test: `tests/test_notchcontroller.cpp`
- Modify: `tests/CMakeLists.txt` (thêm `test_notchcontroller.cpp` +
  `../src/app/NotchController.cpp`), `CMakeLists.txt`
  (`target_sources(HandsFree ...)` thêm cả `.h` và `.cpp` mới)

**Interfaces:**
- Consumes: `ClockSource` (Task 1), `Detector` (đã có), `NotchCommand`
  (`src/dsp/NotchCommand.h` — đã có), `LockFreeRingBuffer` (đã có),
  `PresetManager::PresetNotch` (Task 7).
- Produces (Task 3–7 tiêu thụ):
  - `NotchController (LockFreeRingBuffer<float>& tap, LockFreeRingBuffer<NotchCommand>& commands, ClockSource& clock)`
  - `bool setNotch (int channel, int index, double frequency, double Q, double depthDB, Origin origin)`
  - `void clearNotch (int channel, int index)` · `void clearAll()`
  - `void runOnce()` — một bước pump đồng bộ, không cần thread
  - `std::uint64_t retryCount() const`
  - `void setSampleRate (double)` — forward vào `Detector`
  - `enum class Origin { Detector, Preset, Manual }`
  - Constants: `kChannels = 2`, `kSlots = 16`

**Model (design §3):**
```cpp
struct ModelNotch {
    double frequency = 0.0, Q = 0.0, depthDB = 0.0;
    double lockedAtMs = 0.0, lastDetectedMs = 0.0;
    Origin origin = Origin::Detector;
    bool   active = false;
};
std::array<ModelNotch, kChannels * kSlots> model_;   // dưới modelMutex_
std::vector<NotchCommand> outbox_;                    // cùng modelMutex_
```

- [ ] **Step 1: Viết test thất bại**

```cpp
// tests/test_notchcontroller.cpp
#include <gtest/gtest.h>
#include "app/NotchController.h"
#include "app/PresetManager.h"

namespace {
class FakeClock : public ClockSource {
public:
    double nowMs() const override { return ms_; }
    void advance (double m) { ms_ += m; }
private:
    double ms_ = 1000.0;
};

struct Harness {
    LockFreeRingBuffer<float> tap { 8192 };
    LockFreeRingBuffer<NotchCommand> commands { 128 };
    FakeClock clock;
    NotchController controller { tap, commands, clock };
};
}

TEST (NotchControllerValidation, RejectsBadParamsWithoutTouchingModelOrQueue)
{
    Harness h;
    EXPECT_FALSE (h.controller.setNotch (0, 0, 24000.0, 30.0, -12.0, NotchController::Origin::Detector)); // >= Nyquist 48k
    EXPECT_FALSE (h.controller.setNotch (0, 0, 1000.0,  0.0, -12.0, NotchController::Origin::Detector));   // Q <= 0
    EXPECT_FALSE (h.controller.setNotch (0, 0, 1000.0, 30.0,  +3.0, NotchController::Origin::Detector));   // depth > 0
    EXPECT_FALSE (h.controller.setNotch (2, 0, 1000.0, 30.0, -12.0, NotchController::Origin::Detector));   // channel
    EXPECT_FALSE (h.controller.setNotch (0, 16, 1000.0, 30.0, -12.0, NotchController::Origin::Detector));  // index
    EXPECT_EQ (h.commands.getAvailableRead(), 0u);
}

TEST (NotchControllerCommands, SetNotchRecordsModelAndEnqueues)
{
    Harness h;
    ASSERT_TRUE (h.controller.setNotch (1, 3, 1000.0, 30.0, -12.0, NotchController::Origin::Detector));
    h.controller.runOnce();  // flush outbox
    ASSERT_EQ (h.commands.getAvailableRead(), 1u);
    NotchCommand cmd {};
    ASSERT_EQ (h.commands.read (&cmd, 1), 1u);
    EXPECT_EQ (cmd.type, NotchCommandType::Set);
    EXPECT_EQ (cmd.channel, 1);
    EXPECT_EQ (cmd.index, 3);
    EXPECT_FLOAT_EQ (cmd.frequency, 1000.0f);
    EXPECT_FLOAT_EQ (cmd.depthDB, -12.0f);
}

TEST (NotchControllerCommands, FullQueueDelaysButNeverLoses)   // design §2
{
    Harness h;
    for (int i = 0; i < 140; ++i)  // 140 > capacity 128
        ASSERT_TRUE (h.controller.setNotch (0, i % 16, 500.0 + i, 30.0, -12.0, NotchController::Origin::Detector));
    h.controller.runOnce();
    EXPECT_GT (h.retryCount(), 0u);
    NotchCommand batch[128];
    const auto first  = h.commands.read (batch, 128);
    h.controller.runOnce();
    const auto second = h.commands.read (batch + first, 128);
    EXPECT_EQ (first + second, 140u);  // nothing lost
}
```

- [ ] **Step 2: FAIL** (header chưa tồn tại).
- [ ] **Step 3: Implement.** Ở Task 2, `runOnce()` chỉ flush outbox:

```cpp
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
        // the next pump -- a full queue delays a notch, never loses one.
        const std::lock_guard<std::mutex> lock (modelMutex_);
        outbox_.insert (outbox_.end(), pending.begin() + (std::ptrdiff_t) written, pending.end());
        retryCount_.fetch_add (pending.size() - written, std::memory_order_relaxed);
    }
}
```

`setNotch()` validate đủ 5 predicate (`channel/index` range, `sampleRate > 0`,
`Q > 0`, `0 < freq < sampleRate/2`, `depthDB <= 0`) — sample rate lấy từ
`detector_.getSampleRate()`; pass thì ghi model (kể cả `lockedAtMs` =
`liveMs_`) rồi đẩy lệnh vào `outbox_`.

- [ ] **Step 4: PASS.**
- [ ] **Step 5: Commit**
```bash
git add src/app/NotchController.h src/app/NotchController.cpp tests/test_notchcontroller.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat(bridge): NotchController model, validation, retrying outbox"
```

---

### Task 3: Đồng hồ sống + auto-release (D-06, gate 250 ms)

**Files:**
- Modify: `src/app/NotchController.h/.cpp`
- Test: `tests/test_notchcontroller.cpp` (thêm)

**Interfaces bổ sung:**
- `runOnce()` hoàn chỉnh: drain spectrum → advance live clock → auto-release → flush outbox.
- Constants: `kTapSilenceTimeoutMs = 250.0`, `kAutoReleaseMs = 30000.0`.
- Test-only accessor: `double liveMsForTest() const` (pattern như
  `Detector::getAnalysisWindowForTest()`).

**Pseudocode bắt buộc (design §4):**
```
dt = clock.nowMs() - lastPollMs_
if (clock.nowMs() - lastDataMs_) < kTapSilenceTimeoutMs:
    liveMs_ += dt
lastPollMs_ = clock.nowMs()
```
Sai nguyên tắc "chỉ cộng ở poll có dữ liệu" là bug nửa tốc độ — có test riêng.

- [ ] **Step 1: Test thất bại**

```cpp
TEST (NotchControllerAutoRelease, NotchSurvivesDeadTapAfter30s)   // D-06
{
    Harness h;  // tap never fed => dead
    h.controller.setNotch (0, 0, 1000.0, 30.0, -12.0, NotchController::Origin::Detector);
    h.clock.advance (31000.0);
    h.controller.runOnce();
    // Ring holds ONLY the original Set -- no Clear appeared.
    NotchCommand cmd {};
    std::size_t clears = 0, sets = 0;
    while (h.commands.read (&cmd, 1) == 1)
        cmd.type == NotchCommandType::Clear ? ++clears : ++sets;
    EXPECT_EQ (sets, 1u);
    EXPECT_EQ (clears, 0u);
}

TEST (NotchControllerLiveClock, AlternatingEmptyPollsTrackWallTime)  // bug nửa tốc độ
{
    Harness h;
    std::vector<float> hop (512, 0.1f);
    for (int i = 0; i < 400; ++i) {
        if (i % 2 == 0)                       // every other poll empty
            h.tap.write (hop.data(), hop.size());
        h.clock.advance (5.0);
        h.controller.runOnce();
    }
    EXPECT_NEAR (h.controller.liveMsForTest(), 2000.0, 60.0);  // tolerance 3%
}
```
(Test live-tap-releases sau 35 s nằm trong Task 7 cùng preset adoption để tránh
lặp; implementer có thể thêm sớm.)

- [ ] **Step 2: FAIL.**
- [ ] **Step 3: Implement** đúng pseudocode trên. Auto-release duyệt model:
notch `active` mà `liveMs_ - lastDetectedMs > kAutoReleaseMs` → đẩy Clear vào
outbox + `active = false`. `lastDataMs_` cập nhật trong vòng drain spectrum
(`while ((block = detector_.processLatestBlock(tap_)).magnitudes != nullptr)`).
`lastPollMs_` khởi tạo bằng `clock_.nowMs()` trong constructor.
- [ ] **Step 4: PASS.**
- [ ] **Step 5: Commit** `feat(bridge): live clock with 250ms liveness gate, D-06 auto-release`.

---

### Task 4: Snapshot cho GUI

**Files:**
- Modify: `src/app/NotchController.h/.cpp`
- Test: `tests/test_notchcontroller.cpp`

**Interfaces bổ sung:**
```cpp
struct SnapshotNotch { float frequency, Q, depthDB; std::uint8_t channel, index; };
struct SnapshotBuffer {
    std::array<float, Detector::kNumBins> magnitudes {};   // 513 bins
    std::uint32_t magnitudeCount = 0;
    double sampleRate = 0.0;
    std::array<SnapshotNotch, kChannels * kSlots> notches {};
    std::uint32_t notchCount = 0;
    std::uint64_t sequence = 0;   // tăng mỗi lần publish; GUI phát hiện frame cũ
};
void copySnapshot (SnapshotBuffer& destOwnedByCaller) const;
```

- [ ] **Step 1: Test** — (a) sau khi tap có dữ liệu, `copySnapshot` trả
`magnitudeCount == Detector::kNumBins`; (b) **nhất quán**: notch thêm giữa hai
lần đọc không bao giờ xuất hiện đè lên spectrum của thời điểm khác — kiểm qua
`sequence` tăng đơn điệu và notch list được chụp cùng lần với spectrum; (c) API
không trả container (caller-owned, không cấp phát trong paint path).
- [ ] **Step 2: FAIL.**
- [ ] **Step 3: Implement** — `snapshotMutex_` duy nhất guard `latest_`.
Detector thread copy magnitudes (pointer chỉ hợp lệ đến lần `processLatestBlock`
kế — copy ngay), chụp notch list từ model, `++sequence`. **Không nested lock**:
chụp notch list dưới `modelMutex_`, NHẢ, rồi mới lock `snapshotMutex_`.
`copySnapshot`: lock + memcpy vào dest.
- [ ] **Step 4: PASS.**
- [ ] **Step 5: Commit** `feat(bridge): mutex-guarded spectrum+notch snapshot, caller-owned copy`.

---

### Task 5: AudioEngine — commandQueue_ + drain cap 64

**Files:**
- Modify: `src/app/AudioEngine.h/.cpp`
- Test: `tests/test_audioengine.cpp` (thêm)

**Interfaces:**
- Produces: `LockFreeRingBuffer<NotchCommand>& getCommandQueue()` — caller
contract: **write() only, từ đúng một thread** (comment kiểu `getTapBuffer()`,
`AudioEngine.h:142-152`).
- Test accessor: `const NotchChain& getNotchChainForTest (int channel) const`.

- [ ] **Step 1: Test**

```cpp
TEST (AudioEngineCommands, CommandCrossesBoundaryInOneCallback)   // plan Task 5
{
    AudioEngine engine;
    auto& q = engine.getCommandQueue();
    const NotchCommand set { NotchCommandType::Set, 0, 2, 1000.0f, 30.0f, -12.0f };
    ASSERT_EQ (q.write (&set, 1), 1u);
    float* out[2] = { nullptr, nullptr };
    const float* in[2] = { nullptr, nullptr };
    engine.audioDeviceIOCallbackWithContext (in, 2, out, 2, 64, {});
    EXPECT_EQ (engine.getNotchChainForTest(0).getNotchInfo(2).state, NotchState::Active);
    EXPECT_DOUBLE_EQ (engine.getNotchChainForTest(0).getNotchInfo(2).frequency, 1000.0);
}

TEST (AudioEngineCommands, DrainCappedAt64PerCallback)            // design §2
{
    AudioEngine engine;
    auto& q = engine.getCommandQueue();
    for (int i = 0; i < 100; ++i) {
        const NotchCommand c { NotchCommandType::Set, 0, (std::uint8_t)(i % 16), 500.0f + i, 30.0f, -12.0f };
        ASSERT_EQ (q.write (&c, 1), 1u);
    }
    float* out[2] = { nullptr, nullptr };
    const float* in[2] = { nullptr, nullptr };
    engine.audioDeviceIOCallbackWithContext (in, 2, out, 2, 64, {});
    EXPECT_EQ (q.getAvailableRead(), 36u);   // exactly 64 consumed
}
```

- [ ] **Step 2: FAIL.**
- [ ] **Step 3: Implement**
  - Member: `static constexpr size_t kCommandCapacity = 128;`
    `LockFreeRingBuffer<NotchCommand> commandQueue_ { kCommandCapacity };`
  - Drain ngay sau `ScopedNoDenormals`, trước xử lý audio:
    `NotchCommand batch[64]; const auto n = commandQueue_.read (batch, 64);`
    apply từng lệnh — **bounds check `cmd.channel < 2 && cmd.index < 16` trước
    khi index** (không tin nội dung ring — defence in depth, design §3).
  - `audioDeviceAboutToStart`: `commandQueue_.clear()` cạnh `tapBuffer_.clear()`.
- [ ] **Step 4: PASS** (toàn bộ test_audioengine cũ vẫn xanh).
- [ ] **Step 5: Commit** `feat(bridge): AudioEngine owns command queue, drains <=64 per callback`.
  **Đánh dấu: needs a human listen** (drain point nằm trong callback thật).

---

### Task 6: Thread loop + wiring MainComponent + lifecycle

**Files:**
- Modify: `src/app/NotchController.h/.cpp`, `src/app/MainComponent.h/.cpp`
- Test: `tests/test_gui_wiring.cpp` (mở rộng)

- [ ] **Step 1: Test** — MainComponent dựng được với cả ba member theo thứ tự
khai báo đúng (engine → clock → controller) để controller nhận reference hợp lệ;
controller start/stop đi theo startAudio/restart device.
- [ ] **Step 2: FAIL.**
- [ ] **Step 3: Implement**
  - `NotchController` kế thừa `private juce::Thread`;
    `run()`: `while (! threadShouldExit()) { runOnce(); wait (5); }`;
    `void start()` bọc `startThread`; `void stop (int timeoutMs)` bọc `stopThread`.
  - `MainComponent`: members khai báo theo đúng thứ tự
    `AudioEngine engine_; JuceMonotonicClock systemClock_; NotchController notchController_;`,
    init `notchController_ (engine_.getTapBuffer(), engine_.getCommandQueue(), systemClock_)`.
  - Lifecycle (§6.5): nơi restart/stop device → `notchController_.stop (100)`
    **trước** `engine_.stop()/restart`; start lại sau. Không gate theo Mode ở
    giai đoạn này (policy Tasks 12–15 là scope sau).
- [ ] **Step 4: PASS + app target build xanh.**
- [ ] **Step 5: Commit** `feat(bridge): detector thread, MainComponent wiring, lifecycle ordering`.

---

### Task 7: Preset adoption [D-05]

**Files:**
- Modify: `src/app/NotchController.h/.cpp`
- Test: `tests/test_notchcontroller.cpp`

**Interfaces bổ sung:**
- `int adoptPreset (const std::vector<PresetManager::PresetNotch>& notches)` —
  trả số notch nhận nuôi thành công.

- [ ] **Step 1: Test** — 6 notch hợp lệ → trả 6, model ghi `origin == Preset`,
  lệnh vào queue; notch vượt Nyquist → bị bỏ (trả ít hơn); sau simulated 30 s
  với tap sống, notch preset bị Clear (auto-release áp dụng bình thường).
- [ ] **Step 2: FAIL.**
- [ ] **Step 3: Implement** — loop gọi nội bộ cùng đường
  `setNotch(..., Origin::Preset)`.
- [ ] **Step 4: PASS.**
- [ ] **Step 5: Commit** `feat(bridge): preset adoption per D-05, origin recorded`.

---

### Task 8: Full verify + closeout

- [ ] **Step 1: Full reconfigure + build + test** (header/CMake đổi → bắt buộc):
```
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
cd build && ctest -C Release
```
Paste output thật. Không paste → không "done".
- [ ] **Step 2: Grep statements lỗi thời** trong `docs/`, ledger, handoff nói
  "Task 5 blocked", "bridge not started" → sửa.
- [ ] **Step 3: Handoff + memory note** nếu có bài học non-obvious.
- [ ] **Step 4: Commit** docs với đường dẫn tường minh.
- [ ] **Step 5: Đánh dấu "needs a human listen"** — drain point chạy trong
  callback thật; nghe thử âm lượng nhỏ trước khi tin.

**Ngoài scope (ghi rõ trong handoff):** thuật toán detection (Tasks 12–15),
GUI vẽ spectrum/notch (Tasks 16–24), mode gating của controller.
