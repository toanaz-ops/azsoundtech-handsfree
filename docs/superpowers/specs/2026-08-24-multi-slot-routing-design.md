# Multi-Slot Routing — thiết kế (2026-08-24)

Trạng thái: **đã duyệt qua hội thoại** với owner. Phase 1 implement trong
worktree riêng `feat/multi-slot-routing`.

## 1. Mục tiêu

App hiện cứng 2 kênh stereo (L/R), một luồng chống hú. Nâng cấp thành tối đa
**8 slot xử lý độc lập** trên một thiết bị ASIO/CoreAudio đa kênh:

- Mỗi slot: chọn kênh in/out **bất kỳ** (route chéo tự do — quyết định G-2),
  mono hoặc stereo.
- Mỗi slot có **detector FFT độc lập** (quyết định A) — tự phát hiện hú và đặt
  notch cho kênh của nó.
- Chạy cả **Windows và macOS** (ASIO / CoreAudio).

### Non-goals (Phase 1)

- Nhiều thiết bị mở đồng thời (Phase 2 — xem §9).
- Route 1-in-ra-n-out tùy ý ngoài width của slot (mono = 1→1, stereo = 2→2).
- Đóng gói .dmg cho macOS.

## 2. Quyết định owner (chốt trong hội thoại 2026-08-24)

| # | Quyết định |
|---|---|
| A | Mỗi slot một bộ detector độc lập, cap **kMaxSlots = 8** |
| G-1 | Preset gắn `slotId` vào từng notch; **cho phép đón notch vào slot trống/chưa bật** (tự kích hoạt); slotId ≥ 8 bỏ notch đó + cảnh báo, không fail preset |
| G-2 | Route chéo tự do: in/out là chỉ số kênh bất kỳ do user chọn |
| G-3 | GUI bảng cuộn được cho 8 slot |
| MC-1 | Multicore: đa core khai thác qua detector thread / slot. DSP notch giữ 1 thread audio realtime — chủ ý không song song hóa callback. Pool thread dùng chung ghi là **nâng cấp tương lai**, không làm v1 |
| XP-1 | Cross-platform: tên device type mặc định chọn tại MỘT chỗ bằng `#if` (`"ASIO"` Windows / `"CoreAudio"` macOS); CI thêm matrix macOS |

## 3. Mô hình dữ liệu

```cpp
constexpr int kMaxSlots = 8;

struct SlotConfig {
    bool enabled = false;
    int  width   = 2;            // 1 = mono, 2 = stereo
    int  inputChannels[2]  = {0, 1};   // chỉ số kênh in bất kỳ
    int  outputChannels[2] = {0, 1};   // chỉ số kênh out bất kỳ
    int  deviceId  = 0;          // RESERVED cho Phase 2 (multi-device);
                                 // Phase 1 bỏ qua giá trị này
};
```

Mặc định khi mở thiết bị: slot 0 = stereo `{0,1} → {0,1}`; slot 1..7 trống.

## 4. Đường audio

Một callback duy nhất (giữ nguyên vị trí trong `AudioEngine`), bên trong lặp
slot:

```
clear toàn bộ output buffer
với mỗi slot enabled:
  với mỗi sample n, mỗi làn w ∈ [0, width):
    x = deviceInput[slot.inputChannels[w]]
    y = slot.chains[w].processSample(x)
    deviceOutput[slot.outputChannels[w]] += y     // cộng dồn: nhiều slot
                                                  // cùng nhắm 1 out là hợp lệ
  tap post-notch của slot → SPSC ring CỦA SLOT ĐÓ
Bypass: copy thẳng theo mapping, bỏ DSP.
```

Ràng buộc bất biến (giữ nguyên từ bridge design hiện hành):

- Không lock mới trên audio thread. Đổi cấu hình slot qua snapshot atomics
  giống cơ chế hiện có.
- SPSC discipline mỗi slot: audio thread là producer duy nhất của ring đó,
  detector thread của slot là consumer duy nhất.
- Restart thiết bị đi qua hook `onBeforeRestart/onAfterRestart` đã có ở
  DevicePanel (§6.5 bridge design).

## 5. Detector & multicore (MC-1)

Mỗi slot sở hữu trọn bộ: tap ring + `Detector` + `NotchController` +
detector thread. 8 slot ⇒ 8 thread FFT chạy dàn các core. DSP notch ở lại
một thread audio realtime — chi phí ~16 biquad/sample là không đáng kể so với
chi phí đồng bộ đa nhân trong callback.

`NotchController` bỏ hardcode kênh 0/1: mọi chỗ lấy `slot.width`;
`adoptPreset` áp lên đúng `width` làn của slot mình.

**Nâng cấp tương lai (không làm v1):** nếu đo đạc sau này thấy 8 detector
thread vượt ngân sách CPU trên máy yếu, thay bằng một `ThreadPool` dùng chung
cho toàn bộ detector — giao diện `Detector`/ring giữ nguyên nên thay đổi cục
bộ ở lớp khởi tạo thread.

## 6. Preset v2 (G-1)

- `PresetNotch` thêm trường **tùy chọn** `slot` (thiếu → 0). Thêm mục tùy
  chọn `slots` chứa mapping `SlotConfig` theo slotId.
- Load: notch slot nào về slot đó; slot chưa enabled/tồn tại **tự kích hoạt**
  để đón — giữ `SlotConfig` hiện có của slot nếu đã từng cấu hình, nếu chưa
  thì dùng mặc định stereo `{0,1}→{0,1}` và **kẹp chỉ số kênh vào khoảng hợp
  lệ** của thiết bị đang mở; `slot ≥ 8` → bỏ notch đó, cảnh báo count,
  KHÔNG fail cả preset.
- Mapping kênh nằm trong preset (owner chốt "có gắn").
- **Tương thích ngược**: preset v1 (không có trường mới) đọc ra slot 0 stereo
  `{0,1}→{0,1}` — hành vi ngày nay.

## 7. GUI (G-3)

`SlotPanel` mới đặt dưới/phần `DevicePanel`: bảng cuộn 8 dòng, mỗi dòng:

```
[On/Off] [Mono/Stereo ▼] [In ▼ (▼ khi stereo)] [Out ▼ (▼)] [LED live + số notch]
```

Combo kênh liệt kê tên kênh thật từ driver (`getInputChannelNames` /
`getOutputChannelNames` của `AudioIODevice`). Mọi đổi mapping đi qua chu kỳ
restart chuẩn đã có.

## 8. Cross-platform (XP-1)

- Engine/slot/detector/preset nằm trên lớp `AudioIODevice` trừu tượng —
  không code platform-specific.
- Tên device type mặc định: một hằng số duy nhất, `#if JUCE_WINDOWS → "ASIO"`,
  `#if JUCE_MAC → "CoreAudio"`.
- Build: CMake thêm generator Xcode cho macOS; `.github/workflows/build.yml`
  thêm matrix `[windows-latest, macos-latest]`.
- Installer NSIS vẫn Windows-only; đóng gói .dmg là việc riêng sau Phase 1.

## 9. Phase 2 (reserved, không implement Phase 1)

Multi-device: `deviceId` trong `SlotConfig` là chỗ neo. Hai driver ASIO khác
nhau có thể cùng mở trong một process nhưng cần tự quản vòng đời device (JUCE
`AudioDeviceManager` chỉ hỗ trợ một) và xử clock drift. Cùng-một-driver-hai-
lần gần như luôn fail. Chỉ cân nhắc sau khi Phase 1 chạy thật trên show.

## 10. Test & kiểm chứng

1. Unit test mới (ctest):
   - Ma trận route chéo: tách mapping thành hàm thuần, test không cần card
     âm thanh (mono/stereo, chồng đích output, clear-buffer).
   - `NotchController` width 1 và 2.
   - Parse preset v1/v2, fallback slot trống, slot ≥ 8 bị bỏ + cảnh báo.
2. Full build Release + `ctest` trên Windows; macOS qua CI matrix.
3. **Human listen bắt buộc** trước khi dùng trên show thật (CLAUDE.md — đường
   audio thay đổi).
