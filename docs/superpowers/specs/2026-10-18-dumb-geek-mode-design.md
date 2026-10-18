# Lane U — Hai chế độ giao diện: Dumb mode (≤ 3 nút) ⇄ Geek mode (console đầy đủ)

**Ngày:** 2026-10-18. **Roadmap:** [`2026-09-04-anti-feedback-v2-roadmap.md`](2026-09-04-anti-feedback-v2-roadmap.md) (lane U, cần G — G đã hạ cánh 1.2.0; main `f36ce29`, 1.3.0 alpha, suite 714/714).
**Sổ quyết định:** [`../decisions/2026-10-18-lane-u-dumb-geek-mode.md`](../decisions/2026-10-18-lane-u-dumb-geek-mode.md) (Q1–Q4 owner chọn; Q5–Q9 điều phối tự chọn, owner CHƯA duyệt).
**Trạng thái:** rev 1, chưa phản biện, chưa plan.
**Đụng audio path:** **không** về DSP — không đổi hệ số, gain, buffer, detector. Wizard ghi `SlotConfig` qua đúng API mà `SlotPanel` dùng hôm nay (`AudioEngine::setSlotConfig`), và nhớ thiết bị qua đúng đường `DevicePanel::restartWith`. **Mức thay đổi dự kiến: 0 dB** trên mọi đường tín hiệu đã cấu hình giống nhau. Thay đổi hành vi duy nhất ngoài GUI: app **nhớ thiết bị + routing** giữa hai lần mở (Q5).
**Release:** 1.4.0 (`-Part minor`), không gộp lane khác.

## 1. Vấn đề

- Console hôm nay có ~40 điều khiển (ModeRail 5 nút, TuningPanel 3 nút + 5 combo,
  SlotPanel 8 hàng routing, DevicePanel 4 combo, SpectrumView 4 nhóm nút,
  NotchListPanel nút verdict mỗi hàng). Người dùng mục tiêu ở mic vox của một
  sự kiện nhỏ không cần và không nên chạm hầu hết chúng.
- Owner 06/09/2026: hai chế độ, chuyển qua lại; Dumb ≤ 3 nút; giữ console hiện
  tại làm Geek.
- **Cấu hình thiết bị và routing không được lưu** giữa hai lần mở app
  (`AudioEngine::start()`, `src/app/AudioEngine.cpp:50`): mỗi lần mở là ASIO +
  thiết bị mặc định + `SlotConfig` mặc định. Với Geek thì người vận hành tự chỉnh
  lại; với Dumb thì không có chỗ để chỉnh — nên phải nhớ.

## 2. Quyết định đã chốt

| Mục | Quyết định | Sổ |
|---|---|---|
| Ba nút | `ĐO PHÒNG` (lane M) · `BẬT CHỐNG HÚ` (= `Mode::Auto`) · `TẮT` (= `Mode::Bypass`) | Q1 |
| Chuyển chế độ | nút nhỏ cố định ở góc (`GEEK` trong Dumb, `ĐƠN GIẢN` trong Geek); nhớ qua lần mở; cài mới mặc định Dumb | Q2 |
| Hiển thị | một đèn trạng thái lớn bằng câu người, không phổ, không số dB/Hz | Q3 |
| Thiết bị/routing | wizard riêng trong Dumb | Q4 |
| Lưu ở đâu | `settings.json` cạnh `presets/`, dùng cho cả hai chế độ | Q5 (tạm) |
| Wizard | 3 bước, tạo slot 0, tắt slot 1–7, xác nhận nếu sẽ ghi đè > 1 slot | Q6 (tạm) |
| Khi nào wizard hiện | tự hiện khi vào Dumb mà cấu hình không hợp lệ; đèn "Cần cài đặt" bấm được | Q7 (tạm) |
| Kết quả `ĐO PHÒNG` | tái dùng nguyên luồng lane M; `ÁP DỤNG`/`BỎ` thay tạm hai nút | Q8 (tạm) |
| Thư viện | `az_host_settings` (target riêng, chỉ `juce_core`) | Q9 (tạm) |

## 3. Thiết kế

### 3.1 Thư viện `az_host_settings` (Q9)

Đây là **target CMake thư viện đầu tiên** của repo (hôm nay mọi `.cpp` được liệt
kê thẳng vào `HandsFree`, `HandsFreeTests`, `HandsFreeSnapshot`). Lý do tách,
theo CLAUDE.md "Phần dùng chung phải là thư viện": nhớ thiết bị âm thanh là phần
"JUCE host shell", và voice isolator (`docs/research/2026-09-06-cedar-realtime-plugins.md`
§7) là sản phẩm thứ hai đã được nêu tên, cùng điểm insert, cần đúng phần này.

- `libs/host_settings/` → `add_library(az_host_settings STATIC ...)`, link
  `juce::juce_core` duy nhất. Không include gì từ `src/`.
- API (namespace `az::host`):

  ```cpp
  struct DeviceSetup { juce::String type, name; double sampleRate = 0; int bufferSize = 0; };

  struct HostSettings
  {
      int         schema = 1;
      DeviceSetup device;          // rỗng = chưa từng lưu
      juce::var   product;         // phần riêng của sản phẩm, thư viện không đọc
  };

  // Không bao giờ throw. File thiếu / hỏng / schema lạ -> trả về HostSettings{}
  // và đặt `error` (để log), KHÔNG xóa file hỏng (đổi tên thành .bad để chẩn đoán).
  HostSettings loadHostSettings (const juce::File& file, juce::String& error);

  // Ghi atomic: file tạm cùng thư mục rồi replaceFileIn. false = không ghi được.
  bool saveHostSettings (const juce::File& file, const HostSettings& s);
  ```

- `product` là `juce::var` để thư viện không phụ thuộc ngược `SlotConfig`. App
  serialize vào đó: `{ "uiMode": "dumb"|"geek", "slots": [ ... 8 SlotConfig ... ] }`.
- Đường dẫn: `%APPDATA%/AZSoundtech/HandsFree/settings.json` — app quyết định,
  thư viện chỉ nhận `juce::File`.

### 3.2 Nạp/lưu trong app

- `src/app/AppSettings.{h,cpp}`: chuyển đổi `HostSettings.product` ⇄
  `{ UiMode, std::array<SlotConfig, kMaxSlots> }`. Giá trị lạ (width ∉ {1,2},
  kênh âm) bị bỏ, slot đó về mặc định — giống quy tắc của `PresetManager`.
- **Không đọc/ghi trong ctor `MainComponent`** (bài học data-loop: 26 test + snapshot
  tool dựng `MainComponent`). `main.cpp` gọi `mainComponent.applySettings(file)`
  TRƯỚC `startAudio()` và `saveSettings(file)` ở `shutdown()` + sau mỗi lần wizard
  hoàn tất + sau mỗi lần đổi chế độ.
- Áp dụng lúc khởi động: `engine_.setAudioDeviceType(type)` + `engine_.setAudioDevice(name)` (hai setter có
  sẵn mà `DevicePanel::restartWith` dùng, `src/gui/DevicePanel.cpp:105`), rồi `startAudio()`, rồi sample rate/buffer qua
  `setSampleRate/setBufferSize` (trả false nếu thiết bị từ chối → giữ mặc định,
  `showMessage`), rồi `setSlotConfig` cho từng slot **sau khi** đã có số kênh thật,
  kẹp bằng `slotConfigIsValid` — slot không hợp lệ bị **tắt**, không kẹp kênh
  (kẹp sẽ âm thầm đưa mic vào kênh khác: trên PA đó là một đường tín hiệu lạ).
- Geek cũng được lợi: mở app lại về đúng thiết bị/routing lần trước. Đây là
  thay đổi hành vi có chủ đích, phải ghi vào tester notes.

### 3.3 `DumbView` (Q1, Q3)

`src/gui/DumbView.{h,cpp}`, một `juce::Component` dùng theme Sodium Rack. Không
giữ tham chiếu tới engine/controller: MainComponent đẩy trạng thái vào bằng
`setStatus (DumbStatus)` mỗi tick của timer có sẵn, và nhận hành động qua
`std::function` (`onMeasure`, `onStop`, `onEnable`, `onDisable`, `onApply`,
`onDismiss`, `onOpenWizard`, `onSwitchToGeek`).

Bố cục (1440×920 tham chiếu, co theo kích thước):

```
┌──────────────────────────────────────────────── [GEEK] ┐
│                                                         │
│                ●  ĐANG CHẶN 3 ĐIỂM                       │   đèn: màu + câu
│                   Hệ thống đang chống hú.                │   dòng phụ
│                                                         │
│   [ ĐO PHÒNG ]     [ BẬT CHỐNG HÚ ]     [ TẮT ]          │   3 nút to
└─────────────────────────────────────────────────────────┘
```

**Trạng thái → câu (hàm thuần `dumbStatusFor`, test được):**

| Ưu tiên | Điều kiện | Đèn | Câu chính | Nút |
|---|---|---|---|---|
| 1 | cấu hình không hợp lệ / thiết bị không mở | xám | Cần cài đặt | ĐO PHÒNG, BẬT tắt (disabled); đèn bấm được → wizard |
| 2 | soundcheck đang chạy (`Preflight`…`Gap`) | cyan nhấp nháy | Đang đo phòng… | `DỪNG` thay `ĐO PHÒNG`; BẬT disabled; **TẮT luôn sống** |
| 3 | soundcheck `Results` | cyan | Tìm thấy N điểm dễ hú | `ÁP DỤNG` · `BỎ` thay ĐO PHÒNG, BẬT; TẮT sống |
| 4 | `Mode::Bypass` | xám | Đang TẮT — không chống hú | TẮT ở trạng thái chọn |
| 5 | Auto, risk `Critical` | đỏ | Đang hú — đang xử lý | |
| 6 | Auto, risk `Rising` | amber | Sắp hú | |
| 7 | Auto, N > 0 notch | xanh | Đang chặn N điểm | |
| 8 | Auto, còn lại | xanh | An toàn | |

- Risk lấy **slot xấu nhất** qua `worstRingRiskSnapshot` (có sẵn,
  `MainComponent.h:401`), không phải slot đang hiển thị: Dumb không có khái niệm
  slot đang chọn. N = tổng notch đang hoạt động trên mọi slot bật.
- `Mode::Soundcheck` (thụ động 15 s) không có nút trong Dumb. Nếu đang ở mode đó
  khi chuyển sang Dumb thì hiện như Auto (đèn ưu tiên 5–8).
- `TẮT` không bao giờ bị khoá — giống quy tắc lane M "BYPASS is how a show is
  saved" (`SoundcheckLock`).
- Mọi hành động gọi **đúng** hàm Geek đang gọi: `requestMode(Auto/Bypass)`,
  `beginSoundcheck()`, và các lambda của `soundcheckPanel_` cho DỪNG/ÁP DỤNG/BỎ.
  Không có đường điều khiển thứ hai.

### 3.4 Chuyển chế độ (Q2)

- `MainComponent::setUiMode (UiMode)`: Dumb → ẩn mọi con trừ `dumbView_` và nút
  góc; Geek → ngược lại. `resized()` rẽ nhánh ở đầu. Engine, controller,
  detector **không biết** đang ở chế độ nào.
- Đổi chế độ trong lúc soundcheck đang chạy: được phép, trạng thái giữ nguyên
  (cả hai view đọc cùng `SoundcheckController`).
- Hộp thoại đang mở (xác nhận đo, xác nhận CLEAR ALL) khi đổi chế độ: hộp thoại
  async vẫn sống; callback đi qua `SafePointer` + bộ đếm thế hệ như lane M.

### 3.5 Wizard (Q4, Q6, Q7)

`src/gui/SetupWizard.{h,cpp}` + máy trạng thái thuần `src/app/SetupWizardModel.{h,cpp}`
(không JUCE GUI, test được).

1. **Thiết bị**: combo driver (ASIO đứng đầu nếu có — `chooseDefaultDeviceType`
   có sẵn) + combo thiết bị. Chọn = restart qua `onBeforeRestart/onAfterRestart`
   (§6.5 bridge lifecycle — bắt buộc, xem `memory/bridge-lifecycle-devicepanel-2026-08-23.md`).
2. **Mic**: `MỘT MIC` / `CẶP STEREO`, rồi kênh vào; mỗi kênh có một thanh mức
   vào sống (đọc từ ring thu thô của lane M? — **không**: ring đó chỉ ghi khi đang
   đo. Dùng một peak meter atomic mới mỗi kênh vào trong callback, xem §4).
3. **Ngõ ra**: kênh ra (một hoặc cặp, cùng width với bước 2).
   `XONG` → nếu Geek đang bật > 1 slot: hộp xác nhận "Thay cấu hình hiện tại
   (N đường) bằng 1 đường?" → ghi slot 0, tắt 1–7, `saveSettings`.

`HUỶ` ở bất kỳ bước nào: không ghi gì; thiết bị đã đổi ở bước 1 **được giữ** (đã
restart rồi, lùi lại là một restart nữa trên PA).

### 3.6 Peak meter đầu vào cho wizard

Thứ duy nhất chạm audio callback: `std::array<std::atomic<float>, kMaxInputs>`
peak tuyệt đối mỗi kênh vào, ghi `max(old, |x|)` bằng `memory_order_relaxed`,
GUI đọc và `exchange(0)` mỗi frame. Chỉ ghi khi cờ `inputMeterEnabled_` bật
(wizard đang mở bước 2). Không cấp phát, không lock, không đụng mẫu ra.
**Mức thay đổi trên đường tín hiệu: 0** (chỉ đọc mẫu vào).

## 4. Bất biến

1. DumbView không có đường điều khiển nào mà Geek không có.
2. `TẮT` sống trong mọi trạng thái, kể cả lúc đo và lúc có kết quả.
3. Không có số dB/Hz trong chữ chính của Dumb (lane M Q10).
4. `settings.json` hỏng không bao giờ chặn app mở; app mở với mặc định và báo một dòng.
5. Không đọc/ghi `settings.json` trong ctor `MainComponent`.
6. Slot nạp từ settings mà không hợp lệ với số kênh thật → **tắt**, không kẹp.
7. Mọi restart thiết bị của wizard đi qua hook `onBeforeRestart/onAfterRestart`.
8. Peak meter không ghi gì khi cờ tắt; không đụng buffer ra.

## 5. Test

Headless (`ctest`):
- `az_host_settings`: round-trip; file thiếu; JSON hỏng → mặc định + `.bad`;
  schema lạ; ghi atomic không để lại file tạm.
- `AppSettings`: product ⇄ struct; slot lạ bị bỏ; uiMode lạ → Dumb.
- `dumbStatusFor`: một test mỗi hàng của bảng §3.3, cộng thứ tự ưu tiên (Results
  + Critical → Results; Bypass + Critical → Bypass).
- `SetupWizardModel`: chuyển bước, width bước 3 theo bước 2, xác nhận ghi đè khi
  > 1 slot bật, HUỶ không ghi.
- `MainComponent`: `setUiMode` ẩn/hiện đúng con; nút Dumb gọi đúng `requestMode`
  (`onClick()`, không `triggerClick()` — bài học data-loop); `applySettings` tắt
  slot không hợp lệ.
- Peak meter: cờ tắt → 0; cờ bật → peak đúng của block giả.

Ảnh (`HandsFreeSnapshot`): thêm `dumb-safe.png`, `dumb-blocking.png`,
`dumb-measuring.png`, `dumb-results.png`, `dumb-setup.png`, `wizard-step{1,2,3}.png`.
Đọc lại từng ảnh trước khi gửi.

Rig (không headless được): wizard với ASIO thật; peak meter đúng kênh; nhớ thiết bị
sau khi khởi động lại máy; chuyển chế độ giữa lúc đang hú.

## 6. Docs phải cập nhật cùng thay đổi

`docs/GIOI-THIEU.md` (hai chế độ, wizard), `docs/KY-THUAT-CHONG-HU.md` (§ settings,
không đổi DSP), roadmap (dòng U), tester notes 1.4.0 (nêu: app nay nhớ thiết bị
và routing; cài mới mở ở Dumb).

## 7. Ngoài phạm vi

- Khoá Geek bằng mật khẩu (Q2-2 không chọn).
- Wizard nhiều slot.
- Đổi preset trong Dumb.
- Phổ trong Dumb (Q3-2 không chọn).
