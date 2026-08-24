# Multi-Slot Routing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Nâng app chống hú từ 1 luồng stereo cứng thành tối đa 8 slot xử lý độc lập với route chéo tự do in/out trên thiết bị ASIO/CoreAudio đa kênh, chạy Windows + macOS.

**Architecture:** Mỗi `AudioSlot` = cấu hình kênh + 1–2 `NotchChain` + tap ring riêng + `NotchController`/`Detector` thread riêng. Một callback audio duy nhất lặp qua slot. Mapping đổi luôn đi qua chu kỳ restart thiết bị (`onBeforeRestart/onAfterRestart`) nên không cần snapshot lock-free trong callback.

**Tech Stack:** C++17 / JUCE 9 / CMake / MSVC + Xcode / GoogleTest

**Spec:** `docs/superpowers/specs/2026-08-24-multi-slot-routing-design.md` (đọc cùng plan này)

## Global Constraints

- `kMaxSlots = 8`. Mono = 1 in → 1 out; stereo = 2 in → 2 out. Chỉ số kênh bất kỳ (G-2).
- Audio thread: KHÓA KHÔNG, CẤP PHÁT KHÔNG (giữ nguyên kỷ luật hiện có của `AudioEngine`).
- SPSC mỗi slot: audio thread là producer duy nhất của ring tap của slot; detector thread của slot là consumer duy nhất.
- Mọi đổi mapping/restart qua hook `onBeforeRestart/onAfterRestart` (§6.5).
- Preset v1 phải đọc được 100% (slot mặc định = 0, stereo {0,1}→{0,1}).
- Bypass vẫn cho detector thấy tín hiệu (tap viết trong mọi mode).
- Build: `cmake -B build -G "Visual Studio 18 2026" -A x64` rồi `cmake --build build --config Release`, test `cd build && ctest -C Release`. Worktree mới phải configure từ đầu.
- Không commit `external/asiosdk/`, `external/JUCE` là junction — KHÔNG để git đụng tới.
- Sau Task 7: **human listen bắt buộc** trước khi dùng thật.

---

### Task 0: Worktree + build nền

**Files:** none (môi trường)

- [ ] Kiểm tra junction trước khi tạo worktree (bài học 2026-08-23): `(Get-Item external\JUCE).LinkType` — nếu là Junction thì worktree sẽ chứa junction trỏ ra repo chính, đúng như các lane cũ; xác nhận `git status` sạch trước khi thao tác.
- [ ] `git worktree add .worktrees/multi-slot-routing -b feat/multi-slot-routing`
- [ ] Configure + build lần đầu trong worktree (SDK đã có ở repo chính qua junction `external/asiosdk`):
```bash
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
cd build && ctest -C Release
```
Expected: 257/257 pass (baseline).

### Task 1: SlotConfig + hàm thuần validate/clamp

**Files:**
- Create: `src/app/SlotConfig.h`
- Test: `tests/test_slotconfig.cpp`
- Modify: `tests/CMakeLists.txt` (thêm `test_slotconfig.cpp` vào danh sách nguồn)

**Interfaces (produces):**
- `constexpr int kMaxSlots = 8;`
- `struct SlotConfig { bool enabled=false; int width=2; int inputChannels[2]={0,1}; int outputChannels[2]={0,1}; };`
- `bool slotConfigIsValid(const SlotConfig&, int numInputChannels, int numOutputChannels);`
- `SlotConfig slotClampedTo(const SlotConfig&, int numInputChannels, int numOutputChannels);`

- [ ] **Step 1: Test thất bại** — `tests/test_slotconfig.cpp`:

```cpp
#include <gtest/gtest.h>
#include "app/SlotConfig.h"

TEST(SlotConfig, DefaultIsEnabledFalseStereoZeroOne)
{
    SlotConfig c;
    EXPECT_FALSE(c.enabled);
    EXPECT_EQ(c.width, 2);
    EXPECT_EQ(c.inputChannels[0], 0);
    EXPECT_EQ(c.outputChannels[1], 1);
}

TEST(SlotConfig, IsValidRejectsOutOfRangeChannels)
{
    SlotConfig c;                       // in/out {0,1}
    EXPECT_TRUE(slotConfigIsValid(c, 2, 2));
    EXPECT_FALSE(slotConfigIsValid(c, 1, 2));   // in ch1 > maxIn-1
    EXPECT_FALSE(slotConfigIsValid(c, 2, 1));   // out ch1 > maxOut-1
    SlotConfig mono; mono.width = 1;    // mono chỉ dùng làn 0
    mono.inputChannels[1] = 99;
    EXPECT_TRUE(slotConfigIsValid(mono, 2, 2)); // làn 1 bị bỏ qua khi width==1
}

TEST(SlotConfig, ClampPullsIndicesIntoRange)
{
    SlotConfig c; c.inputChannels[0] = 7; c.outputChannels[1] = 42;
    SlotConfig fixed = slotClampedTo(c, 4, 2);
    EXPECT_EQ(fixed.inputChannels[0], 3);
    EXPECT_EQ(fixed.outputChannels[1], 1);
    EXPECT_TRUE(slotConfigIsValid(fixed, 4, 2));
}
```

- [ ] **Step 2:** Build test → FAIL (header chưa tồn tại).
- [ ] **Step 3: Implement** — `src/app/SlotConfig.h`:

```cpp
// SlotConfig: một luồng xử lý độc lập trong mô hình multi-slot (spec
// 2026-08-24 §3). Thuần dữ liệu, không phụ thuộc JUCE để test được standalone.
#pragma once

constexpr int kMaxSlots    = 8;
constexpr int kMaxSlotLanes = 2;   // stereo = 2 làn, mono = 1

struct SlotConfig
{
    bool enabled = false;
    int  width   = 2;                    // 1 = mono, 2 = stereo
    int  inputChannels[kMaxSlotLanes]  = { 0, 1 };
    int  outputChannels[kMaxSlotLanes] = { 0, 1 };
};

inline bool slotConfigIsValid (const SlotConfig& c,
                               int numInputChannels,
                               int numOutputChannels)
{
    if (numInputChannels <= 0 || numOutputChannels <= 0) return false;
    if (c.width != 1 && c.width != 2) return false;

    for (int lane = 0; lane < c.width; ++lane)
    {
        if (c.inputChannels[lane]  < 0 || c.inputChannels[lane]  >= numInputChannels)
            return false;
        if (c.outputChannels[lane] < 0 || c.outputChannels[lane] >= numOutputChannels)
            return false;
    }
    return true;   // các làn >= width bị bỏ qua, không cần hợp lệ
}

inline SlotConfig slotClampedTo (const SlotConfig& c,
                                 int numInputChannels,
                                 int numOutputChannels)
{
    SlotConfig r = c;
    if (numInputChannels  <= 0) numInputChannels  = 1;
    if (numOutputChannels <= 0) numOutputChannels = 1;
    for (int lane = 0; lane < kMaxSlotLanes; ++lane)
    {
        r.inputChannels[lane]  = std::clamp (r.inputChannels[lane],
                                             0, numInputChannels  - 1);
        r.outputChannels[lane] = std::clamp (r.outputChannels[lane],
                                             0, numOutputChannels - 1);
    }
    return r;
}
```
(Thêm `#include <algorithm>` và `#include <array>` khi cần.)

- [ ] **Step 4:** Build + chạy test → PASS. Commit: `feat: SlotConfig model with pure validation helpers`.

### Task 2: NotchCommand thêm trường slot

**Files:**
- Modify: `src/dsp/NotchCommand.h` (thêm `int slot = 0;` vào struct, kèm comment bridge §2)
- Test: `tests/test_ringbuffer.cpp` (thêm 1 case assert roundtrip giữ nguyên slot)

**Interfaces:** `struct NotchCommand { ... int slot = 0; ... }` — default 0 để mọi caller cũ biên dịch lại không đổi hành vi.

- [ ] Test roundtrip: ghi 1 command `slot=5` vào ring, đọc ra `EXPECT_EQ(cmd.slot, 5)` — FAIL trước, PASS sau khi thêm trường.
- [ ] Commit: `feat: NotchCommand carries slot id`.

### Task 3: AudioEngine — lưu trữ slot + rewrite callback

**Files:**
- Modify: `src/app/AudioEngine.h`, `src/app/AudioEngine.cpp`
- Test: `tests/test_audioengine.cpp`

**Consumes:** Task 1, 2. **Produces:**
- `std::array<LockFreeRingBuffer<float>&, kMaxSlots>` không — API: `LockFreeRingBuffer<float>& getTapBuffer (int slot);` (overload mới; bản không đối số = slot 0, giữ cho caller cũ)
- `LockFreeRingBuffer<NotchCommand>& getCommandQueue (int slot);` (như trên)
- `void setSlotConfig (int slotIndex, const SlotConfig&);`
- `SlotConfig getSlotConfig (int slotIndex) const;`
- `juce::StringArray getInputChannelNames();` / `juce::StringArray getOutputChannelNames();` (từ device đang mở, guard nullptr như các accessor hiện có)
- `const NotchChain& getNotchChainForTest (int slot, int lane) const;` (giữ overload cũ (channel) = slot0/lane channel)

Thiết kế lưu trữ (thay thế mảng cũ):

```cpp
// AudioEngine.h — private
std::array<std::array<NotchChain, kMaxSlotLanes>, kMaxSlots> notchChains_;
std::array<LockFreeRingBuffer<float>,     kMaxSlots> tapBuffers_;      // mỗi cái { kTapCapacity }
std::array<std::atomic<std::uint64_t>,    kMaxSlots> tapDropCounts_ {{}};
std::array<LockFreeRingBuffer<NotchCommand>, kMaxSlots> commandQueues_; // { kCommandCapacity }

// Kích thước bump theo spec §3: burst xấu nhất 16 notch × 2 làn × 8 slot
static constexpr size_t kCommandCapacity = 1024;
static constexpr int    kMaxCommandsPerCallback = 256;  // ~52 us vs budget 670 us

// Cấu hình slot: atomics thường. AN TOÀN vì mọi thay đổi mapping đều đi qua
// chu kỳ restart thiết bị (§6.5) — callback không chạy lúc đó. Callback đọc
// relaxed vào biến local ĐẦU callback rồi dùng bản copy đó xuyên suốt.
std::array<std::atomic<int>, kMaxSlots> slotWidth_;        // mặc định {2,0,...} — width 0 = disabled
std::array<std::atomic<int>, kMaxSlots> slotInCh_[kMaxSlotLanes];  // xem note dưới
std::array<std::atomic<int>, kMaxSlots> slotOutCh_[kMaxSlotLanes];
std::array<std::atomic<bool>, kMaxSlots> slotEnabled_;
```
(Note cho executor: C++ không cho mảng array-of-array như dòng chú thích — dùng
`std::array<std::array<std::atomic<int>, kMaxSlotLanes>, kMaxSlots> slotInCh_;`. Width 0 nghĩa là slot trống.)

Callback mới — thân hàm thay đoạn vòng lặp output hiện tại (giữ ScopedNoDenormals, drainCommandQueue, publish channel counts):

```cpp
// Copy cấu hình một lần đầu callback (relaxed): mapping chỉ đổi giữa các
// callback nhờ §6.5, nên bản copy nhất quán suốt block.
struct LaneRef { const float* in; float* out; NotchChain* chain; bool live; };
LaneRef lanes[kMaxSlots * kMaxSlotLanes];
int numLanes = 0;

for (int s = 0; s < kMaxSlots; ++s)
{
    if (! slotEnabled_[s].load (std::memory_order_relaxed)) continue;
    const int width = slotWidth_[s].load (std::memory_order_relaxed);

    for (int w = 0; w < width; ++w)
    {
        const int inIdx  = slotInCh_[s][w].load (std::memory_order_relaxed);
        const int outIdx = slotOutCh_[s][w].load (std::memory_order_relaxed);
        if (inIdx >= numInputChannels || outIdx >= numOutputChannels) continue;

        auto& l = lanes[numLanes++];
        l.in    = inputChannelData[inIdx];
        l.out   = outputChannelData[outIdx];
        l.chain = &notchChains_[s][w];
        l.live  = ! bypass;
    }
}

// Clear TOÀN BỘ output trước (JUCE yêu cầu viết/clear từng kênh).
for (int ch = 0; ch < numOutputChannels; ++ch)
    if (outputChannelData[ch] != nullptr)
        juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);

// DSP: mỗi làn đọc in của nó, lọc, CỘNG DỒN ra out.
for (int i = 0; i < numLanes; ++i)
{
    LaneRef& l = lanes[i];
    if (l.in == nullptr) continue;

    for (int n = 0; n < numSamples; ++n)
        l.out[n] += static_cast<float> (
            l.chain->processSample (static_cast<double> (l.in[n])));
}

// Bypass: copy thẳng theo mapping (detector vẫn thấy qua tap bên dưới).
if (bypass)
    for (int i = 0; i < numLanes; ++i)
        if (lanes[i].in != nullptr)
            juce::FloatVectorOperations::copy (lanes[i].out, lanes[i].in, numSamples);
```
Tap mới: với MỖI slot enabled có input hợp lệ, bulk-write output làn 0 post-DSP
vào `tapBuffers_[s]` (giữ toàn bộ comment/kỷ luật drop-count hiện có, mỗi slot
một counter riêng).

`drainCommandQueue`: **command queue PER SLOT** (SPSC đòi hỏi single-producer —
8 controller = 8 producer riêng = 8 ring riêng). Callback drains cả 8 (mỗi ring
một batch stack, bound tổng vẫn 256 lệnh/callback):

```cpp
for (int s = 0; s < kMaxSlots; ++s)
    drainCommandsFrom (commandQueues_[s]);   // tách thân drainCommandQueue cũ thành hàm nhận ring + slot
```

`audioDeviceAboutToStart`: setSampleRate + reset cho cả `kMaxSlots×2` chain,
clear cả 8 tap + 8 command ring (precondition §6.5 giữ nguyên).

Legacy: `getTapBuffer()` → `getTapBuffer(0)`, tương tự command; constructor
khởi tạo slot 0 enabled=true stereo {0,1}→{0,1}, slot còn lại width=0/disabled
— **hành vi ngày nay được bảo toàn khi chỉ slot 0 bật**.

- [ ] Step 1: viết test THẤT BẠI trong `test_audioengine.cpp` (pattern hiện có gọi thẳng `audioDeviceIOCallbackWithContext` với buffer giả):
  1. Route chéo mono: slot 1 enabled width1 in{1}→out{3}; feed sóng hình sin vào in1, các in khác im lặng → out3 nhận tín hiệu lọc, out0/1/2 im lặng.
  2. Cộng dồn: slot 0 (in0→out0) + slot 1 (in1→out0) cùng bật → out0 = f(x0)+g(x1).
  3. Bypass theo mapping: mode=Bypass → out3 == copy(in1).
  4. Kênh ngoài phạm vi (device 4in nhưng slot chỉ định in7): bỏ làn, không crash, out được clear.
  5. Tap per-slot: sau callback, `getTapBuffer(1).read(...)` trả đúng khối post-notch của slot 1; `getTapDropCount(slot)` tách biệt.
  6. Command routing: viết `NotchCommand{slot=1,...}` vào `getCommandQueue(1)` → `getNotchChainForTest(1,0)` có notch active, chain slot khác không.
- [ ] Step 2: chạy FAIL. Step 3: implement như trên. Step 4: PASS toàn bộ suite cũ + mới (suite cũ phải xanh không sửa gì — đó là bằng chứng tương thích ngược hành vi).
- [ ] Step 5: Commit: `feat: AudioEngine 8-slot routing with per-slot taps and command queues`.

### Task 4: NotchController theo width slot

**Files:**
- Modify: `src/app/NotchController.h`, `src/app/NotchController.cpp`
- Test: `tests/test_notchcontroller.cpp`

**Consumes:** Task 2 (cmd.slot do CHÍNH controller này gán khi gửi command — constructor thêm tham số `int slotId`; mọi command gửi đi mang `slot = slotId_`, `channel` = làn trong slot).

- [ ] Thêm `setWidth (int lanes)` (1 hoặc 2; default 2). Sửa `adoptPreset` và đường detect (đoạn hardcode `left/right` ở NotchController.cpp:118-119 và :341-343) thành vòng `for (lane = 0; lane < width_; ++lane)` — mỗi làn một lệnh `setNotch(slotId_, lane, ...)`.
- [ ] Test: controller width1 + slotId=3: adoptPreset 2 notch → chain slot3/lane0 có notch, KHÔNG sinh lệnh nào cho lane1; command trong ring mang `slot==3`.
- [ ] Commit: `feat: NotchController slot-aware width and command tagging`.

### Task 5: Preset v2 — slotId + slots section

**Files:**
- Modify: `src/app/PresetManager.h`, `src/app/PresetManager.cpp`
- Test: `tests/test_presetmanager.cpp`

**Consumes:** Task 1. **Produces:**
- `struct PresetNotch { ... int slot = 0; }` (JSON key `"slot"`, thiếu → 0)
- `struct PresetSlot { int index; SlotConfig config; }` + `std::vector<PresetSlot> slots;` trong `PresetData` (JSON mảng `"slots"`, tùy chọn)
- Load rules (spec §6): notch slot chưa enabled/tồn tại → tự kích hoạt, config giữ nguyên nếu đã có, nếu chưa dùng mặc định stereo rồi `slotClampedTo` theo kênh device (số kênh truyền vào hàm load mới `loadPreset(data, int numIn, int numOut)` — overload mới, bản cũ giữ hành vi = clamp về {0,1}); `slot ≥ 8` → bỏ notch, cộng `skippedCount` trả về caller để cảnh báo.
- Writer: emit `"slots"` khi có slot nào ≠ mặc định; **vẫn đọc được file v1** (không có key nào trong số đó → hành vi cũ).

- [ ] Test: (a) parse v1 thuần → mọi notch slot=0; (b) parse v2 với slot=3 → PresetNotch.slot==3; (c) load v2 lên engine giả 4in/4out → slot3 được kích hoạt đúng; (d) slot=9 → skippedCount==1; (e) round-trip write/read giữ slot.
- [ ] Commit: `feat: preset format v2 with slot ids and slot configs`.

### Task 6: MainComponent — 8 pipeline + wiring

**Files:**
- Modify: `src/app/MainComponent.h`, `src/app/MainComponent.cpp`
- Test: `tests/test_gui_wiring.cpp`

**Consumes:** Task 3, 4, 5.

- Thay `NotchController notchController_;` bằng:
```cpp
std::array<NotchController, kMaxSlots> notchControllers_;   // khởi tạo với (engine_.getTapBuffer(i), engine_.getCommandQueue(i), systemClock_) + slotId=i
```
- Restart hooks (§6.5) lặp cả 8: `onBeforeRestart` stop từng cái, `onAfterRestart` start từng cái + re-apply width theo config slot.
- `spectrumView_` tiếp tục đọc `notchControllers_[0]` (slot 0 focus — hiển thị slot khác là việc GUI sau, ngoài scope plan này).
- ModeBar/clearAll áp cho tất cả slot đang bật.
- Apply preset: gọi load rule Task 5, set width từng controller theo SlotConfig, rồi adopt.
- Test wiring: dựng MainComponent với engine không device (pattern hiện có), assert 8 controller tồn tại/tắt được mà không crash; slot 0 enabled mặc định.
- Commit: `feat: main component drives 8 independent detector pipelines`.

### Task 7: SlotPanel GUI

**Files:**
- Create: `src/gui/SlotPanel.h`, `src/gui/SlotPanel.cpp`
- Modify: `CMakeLists.txt` (target_sources HandsFree), `src/app/MainComponent.cpp/.h` (đặt panel dưới DevicePanel), `tests/CMakeLists.txt` (test GUI nếu pattern cho phép)
- Test: `tests/test_slotpanel.cpp` (construct + populate với engine giả, assert số row combo)

**Produces:** component bảng cuộn 8 dòng `[On/Off toggle] [Mono/Stereo ▼] [In ▼] [(In ▼)] [Out ▼] [(Out ▼)] [LED + notch count]`, dùng AzTheme tokens. Combo kênh đổ từ `engine_.getInputChannelNames()/getOutputChannelNames()`. Mọi thay đổi đi qua `onBeforeRestart/onAfterRestart` rồi `engine_.setSlotConfig()` + restart.

- [ ] Commit: `feat: SlotPanel UI for 8-slot cross routing`.

### Task 8: Platform default device type + CI macOS

**Files:**
- Modify: `src/app/AudioEngine.h` (một chỗ duy nhất):
```cpp
inline const char* defaultDeviceTypeName()
{
#if JUCE_MAC
    return "CoreAudio";
#elif JUCE_WINDOWS
    return "ASIO";
#else
    return "";
#endif
}
// desiredDeviceType_ { defaultDeviceTypeName() }
```
- Modify: `.github/workflows/build.yml` — matrix:
```yaml
strategy:
  matrix:
    os: [windows-latest, macos-latest]
runs-on: ${{ matrix.os }}
```
với bước configure chọn generator theo OS (Windows giữ `-G "Visual Studio ..." -A x64`; macOS dùng mặc định Xcode, KHÔNG truyền asiosdk — JUCE_ASIO chỉ compile trên Windows).
- [ ] Commit: `feat: platform-aware default device type; CI builds macos`.

### Task 9: Close-out Phase 1

- [ ] Full build Release + `ctest -C Release` trong worktree, PASTE OUTPUT.
- [ ] Rebuild installer NSIS từ build của worktree (verify-installer optional, nói rõ nếu bỏ).
- [ ] Human listen checklist (ghi trong PR/handoff): chọn Audient id14, bật slot 0 stereo nghe passthrough; bật slot mono route chéo; soundcheck đặt notch; bypass so sánh.
- [ ] Handoff `shared/handoff/handoff-<date>-multi-slot.md` + cập nhật memory nếu có bài học.
- [ ] Merge CHỈ khi user nói merge.
