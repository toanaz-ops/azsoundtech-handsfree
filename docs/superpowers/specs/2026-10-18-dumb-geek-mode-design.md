# Lane U — Hai chế độ giao diện: Dumb mode (≤ 3 nút) ⇄ Geek mode (console đầy đủ)

**Ngày:** 2026-10-18. **Roadmap:** [`2026-09-04-anti-feedback-v2-roadmap.md`](2026-09-04-anti-feedback-v2-roadmap.md) (lane U, cần G — G đã hạ cánh 1.2.0; main `f36ce29`, 1.3.0 alpha, suite 714/714).
**Sổ quyết định:** [`../decisions/2026-10-18-lane-u-dumb-geek-mode.md`](../decisions/2026-10-18-lane-u-dumb-geek-mode.md) (Q1–Q4 owner chọn; Q5–Q12 điều phối tự chọn, owner CHƯA duyệt).
**Trạng thái:** **rev 3** — rev 2 + ba chỗ chỉnh khi triển khai (§9). Rev 2: sau phản biện read-only độc lập vòng 1 (6 BLOCKER / 7 IMPORTANT / 6 MINOR, bảng đối chiếu ở §8). Rev 1 không dùng làm cơ sở cho code. **Đã triển khai** trên `feat/lane-u-dumb-geek-mode` (PR #7), suite 769/769 (Linux/clang); chưa merge.
**Đụng audio path:** không về DSP — không đổi hệ số, gain, buffer, detector. Nhưng **có đổi cái gì được mở lúc khởi động** (§3.2): app nay nhớ thiết bị + routing. Mức thay đổi dự kiến: **0 dB** trên mọi đường tín hiệu có cấu hình giống lần chạy trước; khác biệt duy nhất người nghe gặp được là sau khi mở lại app, các đường đã cấu hình **vẫn còn** thay vì về mặc định slot 0 `{0,1}→{0,1}`. Peak meter của wizard chỉ đọc mẫu vào (§3.6).
**Release:** 1.4.0 (`-Part minor`), không gộp lane khác.

## 1. Vấn đề

- Console hôm nay có ~40 điều khiển. Người dùng mục tiêu ở mic vox của một sự kiện
  nhỏ không cần và không nên chạm hầu hết chúng.
- Owner 06/09/2026: hai chế độ, chuyển qua lại; Dumb ≤ 3 nút; giữ console hiện tại
  làm Geek.
- **Cấu hình thiết bị và routing không được lưu** giữa hai lần mở app:
  `startAudio()` (`src/app/MainComponent.cpp:1010`) luôn chọn
  `chooseDefaultDeviceType` + thiết bị mặc định; `SlotConfig` chỉ sống trong RAM
  hoặc preset. Dumb không có chỗ để chỉnh lại mỗi lần mở — nên phải nhớ.
- **Engine chỉ mở 2 kênh vào / 2 kênh ra** (`kNumChannels = 2`,
  `src/app/AudioEngine.cpp:12`). Lane U **không** đổi điều đó (đổi = thay đổi
  audio path có mức riêng, lane khác). Wizard vì vậy chỉ cho chọn trong các kênh
  callback thật sự nhận được.

## 2. Quyết định đã chốt

| Mục | Quyết định | Sổ |
|---|---|---|
| Ba nút | `ĐO PHÒNG` (lane M) · `BẬT CHỐNG HÚ` (= `Mode::Auto`) · `TẮT` (= `Mode::Bypass`) | Q1 |
| Chuyển chế độ | nút nhỏ cố định ở góc (`GEEK` trong Dumb, `ĐƠN GIẢN` trong Geek); nhớ qua lần mở | Q2 |
| Hiển thị | một đèn trạng thái lớn bằng câu người + một dòng thông báo; không phổ, không dB/Hz | Q3 |
| Thiết bị/routing | wizard riêng trong Dumb | Q4 |
| Lưu ở đâu | `settings.json` cạnh `presets/`, dùng cho cả hai chế độ | Q5 (tạm) |
| Wizard | 3 bước, tạo slot 0, tắt slot 1–7, xác nhận nếu sẽ ghi đè > 1 slot | Q6 (tạm) |
| Khi nào wizard hiện | khi vào Dumb mà chưa cấu hình được; đèn "Cần cài đặt" bấm được | Q7 (tạm) |
| Kết quả `ĐO PHÒNG` | tái dùng nguyên luồng lane M | Q8 (tạm) |
| Thư viện | `az_host_settings`, target INTERFACE (§3.1) | Q9 (tạm) |
| `TẮT` lúc đang đo | khoá, giống `BYPASS` của Geek; `DỪNG` ở ngay cạnh | Q10 (tạm) |
| Lần đầu của tester 1.3.x | không có `settings.json` mà có `presets/` → mở **Geek** | Q11 (tạm) |
| Thiết bị đã lưu không còn | **không** áp routing đã lưu lên thiết bị khác; "Cần cài đặt" | Q12 (tạm) |

## 3. Thiết kế

### 3.1 Thư viện `az_host_settings` (Q9)

Target thư viện đầu tiên của repo. Lý do tách (CLAUDE.md "Phần dùng chung phải là
thư viện"): nhớ thiết bị âm thanh là phần "JUCE host shell", và voice isolator
(`docs/research/2026-09-06-cedar-realtime-plugins.md` §7) là sản phẩm thứ hai đã
được nêu tên, cùng điểm insert, cần đúng phần này.

- `libs/host_settings/`, `add_library(az_host_settings INTERFACE)` với
  `target_sources(INTERFACE …)`: **người dùng biên dịch nguồn**, giống một module
  JUCE. Một thư viện STATIC link `juce::juce_core` sẽ biên dịch `juce_core.cpp` vào
  thư viện VÀ vào app (module JUCE là INTERFACE thêm nguồn cho ai link nó) — hai bản
  của mọi symbol JUCE. Chỉ dùng `juce_core`; không include gì từ `src/`.
- API (namespace `az::host`):

  ```cpp
  struct DeviceSetup { juce::String type, name; double sampleRate = 0; int bufferSize = 0; };
  struct HostSettings { DeviceSetup device; juce::var product; };

  struct LoadResult
  {
      HostSettings settings;     // mặc định khi không đọc được
      juce::String error;        // rỗng = ổn (kể cả "chưa có file")
      bool mayOverwrite = true;  // false = file của build MỚI hơn: không được ghi đè
  };

  LoadResult loadHostSettings (const juce::File&);
  bool       saveHostSettings (const juce::File&, const HostSettings&);  // atomic
  ```

- JSON hỏng → `error`, file đổi tên `.bad` (để chẩn đoán), `mayOverwrite = true`.
- `schema` lớn hơn bản này hiểu → `error`, file **giữ nguyên**, `mayOverwrite = false`:
  hạ cấp một bản alpha không được phá cấu hình của bản mới.
- Rate ngoài [8 k, 768 k] / buffer ngoài [16, 8192] → đọc như "chưa lưu" (0).

### 3.2 Nạp/lưu trong app

`src/app/AppSettings.{h,cpp}` chuyển `HostSettings.product` ⇄
`{ UiMode, hasRouting, std::array<SlotConfig, 8> }`. Một entry slot không parse được
→ slot đó **tắt** (không kẹp — khác `PresetManager`, vốn kẹp width và kênh,
`PresetManager.cpp:885-890`: kẹp ở đây là đưa mic vào một kênh không ai chọn).

**Không I/O trong ctor `MainComponent`** (bài học data-loop). `main.cpp` gọi
`content->enableSettings (defaultAppSettingsFile())` **trước** `startAudio()`. Không có
lời gọi đó (26 test, snapshot tool) thì `MainComponent` không bao giờ đọc/ghi file.

**Thứ tự khởi động (sửa B-1, B-2, B-3, B-4, B-6):**

1. `enableSettings` đọc file. Không có file: `uiMode` = Geek nếu thư mục `presets/`
   đã tồn tại (tester 1.3.x, Q11), ngược lại Dumb.
2. Nếu `hasRouting`: ghi 8 `SlotConfig` vào engine bằng `engine_.setSlotConfig`
   **khi engine còn dừng** (chưa thread nào chạy, nên không cần chu trình restart;
   `startAudio()` đã `setWidth` từ `getSlotConfig`, `MainComponent.cpp:1026-1031`;
   callback tự kiểm biên từng làn).
3. `startAudio()` đổi một chỗ: loại driver = `savedDevice.type` nếu loại đó có trong
   `getAvailableDeviceTypeNames()`, ngược lại `chooseDefaultDeviceType` như hôm nay;
   tên thiết bị = `savedDevice.name`.
4. Sau `engine_.start()`: nếu có thiết bị đã lưu mà `getCurrentDeviceType/Name()`
   **khác** (JUCE `initialise(…, selectDefaultDeviceOnFailure = true, …)` đã mở thiết
   bị mặc định thay) → **tắt mọi slot đã khôi phục** qua `changeSlotConfig`, đặt cờ
   `deviceMismatch_`, `showMessage("Không thấy <tên>. Cần cài đặt lại.")` (Q12).
5. Rate/buffer đã lưu: áp **trong** `onBeforeRestart/onAfterRestart`, đúng như
   `DevicePanel::applySampleRate` (`DevicePanel.cpp:136-145`). Từ chối → giữ của
   thiết bị, một dòng thông báo.
6. **Kiểm kênh ở tick timer đầu tiên mà `getNumInput/OutputChannels() > 0`**
   (số kênh = 0 cho tới khi audio chạy, `AudioEngine.h:145`): slot bật mà
   `!slotConfigIsValid` → tắt qua `changeSlotConfig` + một dòng thông báo. Chạy
   một lần mỗi lần mở thiết bị.

**Khi nào ghi** (chỉ khi đã `enableSettings` và `mayOverwrite`):
đổi chế độ UI; wizard `XONG`; `changeSlotConfig` do người dùng (SlotPanel); đổi
thiết bị/rate/buffer từ DevicePanel **thành công**; `MainComponent::~MainComponent`
**không** ghi (dtor không I/O) — `main.cpp::shutdown()` gọi `saveSettings()` trước
`mainWindow.reset()`.

**Không xoá trí nhớ (I-9):** phần `device` chỉ được cập nhật từ một thiết bị **đã mở
thành công** do người dùng chọn. Khi `deviceMismatch_` hoặc mở thất bại, lần ghi giữ
nguyên `device` + routing đã đọc, cho tới khi người dùng chủ động cài lại.

### 3.3 `DumbView` (Q1, Q3)

`src/gui/DumbView.{h,cpp}`, theme Sodium Rack. Không giữ tham chiếu engine/controller:
MainComponent đẩy `DumbInputs` mỗi tick của timer có sẵn, view gọi `dumbStatusFor`
(hàm thuần, `src/gui/DumbStatus.{h,cpp}`) và vẽ. Hành động qua `std::function`.

```
┌──────────────────────────────────────────────── [GEEK] ┐
│                ●  ĐANG CHẶN 3 ĐIỂM                       │  đèn + câu
│                   Hệ thống đang chống hú.                │  dòng phụ
│                   <dòng thông báo, nếu có>               │  showMessage
│   [ ĐO PHÒNG ]     [ BẬT CHỐNG HÚ ]     [ TẮT ]          │
└─────────────────────────────────────────────────────────┘
```

**Đầu vào** — mọi thứ đọc từ đúng nguồn Geek đang đọc:

| Trường | Nguồn |
|---|---|
| `configured` | thiết bị đang chạy, không `deviceMismatch_`, không `getLastDeviceError()`, slot 0 bật và hợp lệ |
| `phase` | `soundcheckPanel_.getMode()`: Hidden → Idle, Running → Measuring, Results, Applied |
| `measureAllowed` | **đúng** vị từ của `updateMeasureEnabled` (`MainComponent.cpp:1523`): lock None và không có hộp xác nhận đang mở |
| `risk` | `worstRingRiskSnapshot` → `riskForScore` → **một `RingRiskHysteresis` riêng của Dumb** (750 ms, `SpectrumView.h:438`) |
| `notchCount` | tổng notch hoạt động mọi slot bật |
| `message` | đúng dòng thanh trạng thái Geek đang giữ (`refreshStatus`: lỗi thiết bị, rồi `showMessage`) — giữ, không mờ, như Geek |

**Bảng trạng thái** (ưu tiên từ trên xuống):

| # | Điều kiện | Đèn | Câu chính | Nút trái · giữa · TẮT |
|---|---|---|---|---|
| 1 | `!configured` | xám | Cần cài đặt (đèn bấm được → wizard) | ĐO ✗ · BẬT ✗ · TẮT ✓ |
| 2 | Measuring | cyan, thở | Đang đo phòng… | **DỪNG** ✓ · BẬT ✗ · TẮT ✗ (Q10, giống Geek) |
| 3 | Results | cyan | Tìm thấy N điểm dễ hú / Không thấy điểm dễ hú | **ÁP DỤNG** (✗ nếu N=0) · **BỎ** · TẮT ✓ |
| 4 | Applied | cyan | Đã chặn trước N điểm | ĐO ✗ · **BỎ** · TẮT ✓ |
| 5 | Bypass | xám | Đang TẮT | ĐO (theo `measureAllowed`) · BẬT ✓ · TẮT (chọn) |
| 6 | Critical | đỏ | Đang hú — đang xử lý | |
| 7 | Rising | amber | Sắp hú | |
| 8 | N > 0 | xanh | Đang chặn N điểm | |
| 9 | Low | xanh | An toàn | |
| 10 | Unavailable | xanh mờ | Đang chống hú (không khẳng định "an toàn" khi detector chưa có số) | |

Hàng 5–10: ĐO theo `measureAllowed`; BẬT, TẮT ✓.

- `DỪNG` / `ÁP DỤNG` / `BỎ` gọi **chính** các lambda của `soundcheckPanel_`
  (`onStop`, `onApply`, `onDismiss`); `ĐO PHÒNG` gọi `beginSoundcheck()` (cùng hộp
  thoại xác nhận, cùng lời từ chối — lời từ chối giờ hiện ở dòng thông báo);
  `BẬT`/`TẮT` gọi `requestMode`. **Không có đường điều khiển thứ hai.**
- `Mode::Soundcheck` (thụ động) không có nút trong Dumb; nếu đang ở mode đó thì đèn
  đọc như Auto, `BẬT` không ở trạng thái chọn.

### 3.4 Chuyển chế độ (Q2)

- `MainComponent::setUiMode (UiMode)`: Dumb → ẩn mọi con trừ `dumbView_` và nút góc;
  Geek → ngược lại. `resized()` rẽ nhánh ở đầu. Engine, controller, detector không
  biết đang ở chế độ nào.
- Đổi chế độ lúc đang đo: được. Cả hai view đọc cùng `soundcheckPanel_`/controller.
- Hộp thoại async đang mở khi đổi chế độ: vẫn sống, callback đi qua `SafePointer`
  + bộ đếm thế hệ có sẵn.

### 3.5 Wizard (Q4, Q6, Q7)

`src/gui/SetupWizard.{h,cpp}` + máy trạng thái thuần `src/app/SetupWizardModel.{h,cpp}`.

1. **Thiết bị**: combo driver + combo thiết bị. Đổi = `devicePanel_`'s
   `restartWith` path (cùng hook `onBeforeRestart/onAfterRestart`).
2. **Mic**: các lựa chọn dựng từ `engine_.getNumInputChannels()` — số kênh callback
   **thật sự nhận**, không phải `getInputChannelNames()` của thiết bị (I-7). Với
   engine 2 kênh: `Mic 1`, `Mic 2`, `Stereo 1+2`. Mỗi kênh có thanh mức vào sống (§3.6).
3. **Ngõ ra**: tương tự từ `getNumOutputChannels()`, cùng width với bước 2.
   `XONG` → nếu đang bật > 1 slot: hộp xác nhận "Thay N đường hiện có bằng 1
   đường?" → slot 0 = cấu hình mới, slot 1–7 tắt, **qua `changeSlotConfig`** (B-6:
   restart cycle + `setWidth` + `applyModeGating`), rồi `saveSettings()`.

`HUỶ`: không ghi routing. Nếu bước 1 đã đổi thiết bị, chạy luôn kiểm kênh của §3.2
bước 6 (slot không còn hợp lệ trên thiết bị mới → tắt) và **không** lưu thiết bị mới
(I-8). Wizard không mở được khi đang đo (đèn không bấm được ở hàng 2–4).

### 3.6 Peak meter đầu vào cho wizard

`std::array<std::atomic<float>, 2>` (= `kNumChannels`), ghi `max(old, |x|)` bằng
load + store `relaxed` cho kênh `ch < min(numInputChannels, 2)` có con trỏ khác null;
GUI đọc bằng `exchange(0)` mỗi frame. Cờ `inputMeterEnabled_` đọc **một lần** đầu
callback như các atomic khác; chỉ bật khi wizard ở bước 2. Một đỉnh có thể mất khi
load/store của callback xen giữa `exchange` của GUI — chấp nhận được cho một thanh
mức hiển thị, nói thẳng ở đây. Không cấp phát, không lock, không ghi mẫu ra.
**Mức thay đổi: 0.**

## 4. Bất biến

1. DumbView không có đường điều khiển nào mà Geek không có; mọi nút gọi đúng hàm Geek gọi.
2. `TẮT` sống trong mọi trạng thái trừ lúc đang đo — y như `BYPASS` của Geek
   (`setModeControlsEnabled(!measuring)`, `MainComponent.cpp:1477`), và lúc đó `DỪNG`
   ở cùng màn hình.
3. Không có số dB/Hz trong chữ của Dumb.
4. `settings.json` hỏng không chặn app mở; file của build mới hơn không bị ghi đè.
5. Không I/O settings khi chưa `enableSettings` (mọi test, snapshot tool).
6. Routing đã lưu không bao giờ chạy trên một thiết bị khác thiết bị đã lưu.
7. Slot không hợp lệ với số kênh **thật** → tắt, không kẹp. Kiểm chỉ khi số kênh > 0.
8. Mọi thay đổi slot sau khi engine chạy đi qua `changeSlotConfig`; mọi restart thiết
   bị/rate/buffer đi qua hook `onBeforeRestart/onAfterRestart`.
9. Trí nhớ thiết bị chỉ bị ghi đè bởi một thiết bị đã mở thành công do người dùng chọn.
10. Peak meter không ghi gì khi cờ tắt; không đụng buffer ra.

## 5. Test

Headless (`ctest`):
- `az_host_settings`: round-trip; thiếu file; JSON hỏng → `.bad`; schema mới hơn →
  file giữ nguyên + `mayOverwrite=false`; rate/buffer ngoài khoảng; ghi atomic.
- `AppSettings`: round-trip; slot lạ → tắt; vắng routing ≠ 8 slot tắt; uiMode lạ.
- `dumbStatusFor`: một test mỗi hàng §3.3 + thứ tự ưu tiên (Results+Critical → Results;
  Bypass+Critical → Bypass; Measuring → TẮT ✗, DỪNG ✓; Unavailable ≠ "An toàn").
- `SetupWizardModel`: bước, width bước 3 theo bước 2, lựa chọn chỉ trong số kênh
  callback nhận, xác nhận khi > 1 slot bật, HUỶ không ghi routing.
- `MainComponent` (headless, 0 kênh): `setUiMode` ẩn/hiện đúng; nút Dumb gọi đúng
  hàm (`onClick()`); khôi phục khi 0 kênh **không** tắt slot nào (B-1); kiểm kênh
  tắt slot không hợp lệ khi số kênh được giả lập > 0 (cần một seam test cho số kênh);
  không `enableSettings` → không file nào được tạo.
- Peak meter: cờ tắt → 0; cờ bật → peak đúng của block giả; con trỏ null bỏ qua.

Ảnh (`HandsFreeSnapshot`): `dumb-setup.png`, `dumb-safe.png`, `dumb-blocking.png`,
`dumb-measuring.png`, `dumb-results.png`, `wizard-step{1,2,3}.png`. Đọc lại từng ảnh.

Rig: wizard với ASIO thật; đổi/rút sound card giữa hai lần mở (Q12); peak meter đúng
kênh; chuyển chế độ lúc đang hú.

## 6. Docs và tester notes

`docs/GIOI-THIEU.md` (hai chế độ, wizard, nhớ cấu hình), `docs/KY-THUAT-CHONG-HU.md`
(§ settings; DSP không đổi), roadmap (dòng U), tester notes 1.4.0: **app nay nhớ
thiết bị + routing giữa hai lần mở**; cài mới mở Dumb, máy đã có preset mở Geek; rút
sound card → "Cần cài đặt", không chạy routing cũ lên card khác.

## 7. Ngoài phạm vi

Khoá Geek bằng mật khẩu; wizard nhiều slot; mở hơn 2 kênh (lane riêng, có mức riêng);
preset trong Dumb; phổ trong Dumb.

## 8. Phản biện vòng 1 (2026-10-18) → rev 2

| # | Mức | Finding | Xử lý |
|---|---|---|---|
| 1 | BLOCKER | Số kênh = 0 tới khi audio chạy → kiểm sau `startAudio` tắt hết slot rồi ghi xuống đĩa | §3.2 bước 2 + 6: ghi slot trước start, kiểm ở tick đầu có kênh |
| 2 | BLOCKER | `startAudio` tự đè loại driver | §3.2 bước 3 |
| 3 | BLOCKER | JUCE mở thiết bị mặc định khi thiết bị đã lưu mất → routing cũ chạy trên card khác | §3.2 bước 4, Q12, inv 6 |
| 4 | BLOCKER | rate/buffer không qua hook §6.5 | §3.2 bước 5, inv 8 |
| 5 | BLOCKER | "TẮT luôn sống" mâu thuẫn inv 1 và lane M | Q10, inv 2 |
| 6 | BLOCKER | `setSlotConfig` trần ≠ đường của SlotPanel | wizard + kiểm kênh dùng `changeSlotConfig` |
| 7 | IMPORTANT | Engine chỉ mở 2 kênh | §1, §3.5 bước 2, §7 |
| 8 | IMPORTANT | "0 dB" chưa đủ; HUỶ để slot cũ trên card mới | header, §3.5 HUỶ, §6 |
| 9 | IMPORTANT | Ghi lúc thất bại xoá trí nhớ; `shutdown` không có handle | §3.2 "Không xoá trí nhớ", `saveSettings` trước `mainWindow.reset()` |
| 10 | IMPORTANT | Thiếu trạng thái (confirm đang mở, Applied, Unavailable, lỗi thiết bị) | §3.3 bảng hàng 1, 4, 10 + `measureAllowed` |
| 11 | IMPORTANT | Không hysteresis → đèn nháy | `RingRiskHysteresis` riêng của Dumb |
| 12 | IMPORTANT | `showMessage` vô hình trong Dumb | dòng thông báo |
| 13 | IMPORTANT | Tester 1.3.x bị coi như cài mới | Q11 |
| m | MINOR | STATIC vs INTERFACE; "giống PresetManager" sai; schema mới bị phá; peak meter biên; displayedSlot cũ; test 0-kênh pass sai lý do | §3.1, §3.2, §3.6, §5 |

## 9. Sai lệch khi triển khai (rev 3)

| Chỗ | Rev 2 nói | Code làm | Lý do |
|---|---|---|---|
| §3.5 HUỶ | không lưu thiết bị mới | đúng như spec, **sau verifier code I-1**: lúc wizard mở không lưu gì (`onAfterRestart` bỏ qua `saveSettings`); HUỶ đưa bộ nhớ về snapshot lúc mở; nếu bước 1 đã đổi card thì đó là một mismatch — mọi slot tắt, bộ nhớ giữ | bản đầu lưu luôn trong `onAfterRestart`, và sau một mismatch thì ghi đè routing đã lưu bằng routing toàn-tắt |
| §3.6 | mảng 2 phần tử (= `kNumChannels`) | `kInputMeterChannels = 8`, vòng lặp chặn bởi `min(numInputChannels, 8)` | để khi engine mở thêm kênh thì meter không lặng lẽ cắt ở 2; không đổi gì khi engine còn 2 kênh |
| §3.3 tiêu đề | (không nói) | câu chính viết **sentence case**, không `toUpperCase()` | ảnh render: `towupper` để nguyên phần lớn chữ Việt ngoài locale Việt — "ĐANG CHặN 3 điểM"; nhãn nút được viết HOA sẵn trong mã nguồn vì L&F tự uppercase |
| §3.3 đèn cyan | (không nói màu) | màu `settled` (ICE) | `trace` là amber — ảnh đầu tiên ra đèn cam cho "đang đo" |
| §3.4 | nút `ĐƠN GIẢN` ở góc | ở **mép phải ngoài cùng** của masthead, badge dịch vào trong | ảnh render: đặt cạnh badge thì đè chip "NO DEVICE" |
| `AudioEngine::hasDeliveredSinceStart` | (không nói) | cờ false từ `audioDeviceAboutToStart` tới callback đầu; wizard và kiểm slot chỉ tin số kênh khi cờ bật | verifier I-2: đổi card thì số kênh của card CŨ còn treo. Vòng 1 sửa bằng cách zero số kênh khi dừng — vòng 2 chỉ ra cách đó làm nạp preset lúc rút card kẹp mọi slot về 2×2, nên đổi sang cờ |
| wizard xác nhận ghi đè | `SafePointer` | + bộ đếm thế hệ, một hộp mỗi lần | verifier I-3: OK trên hộp cũ sau HUỶ → mở lại áp kết quả cũ |
| `DumbStatus` thứ tự hàng | "Cần cài đặt" là hàng 1 | ba hàng soundcheck đứng TRƯỚC | verifier B-1: đo bắt đầu ở Geek trên slot ≠ 0 rồi sang Dumb thì mất `DỪNG` và `TẮT` sống giữa lúc quét |
| `loadHostSettings` | file không đọc được = hỏng | file có byte mà đọc ra rỗng → giữ nguyên, `mayOverwrite=false` | verifier MINOR: file đang bị sync/AV giữ không phải là file hỏng |
| HUỶ sau khi đổi card | cờ "đã chọn ở bước 1" = mismatch | so card đang mở với card trong snapshot | verifier vòng 2 I-1: chọn Y rồi về X rồi HUỶ đã tắt bảo vệ trên X giữa show; về đúng card đã nhớ thì routing đã nhớ quay lại |
| khôi phục routing khi về card đã nhớ | (không nói) | sao chép slot ra biến cục bộ, giữ cờ mismatch suốt vòng lặp, lưu MỘT lần sau | verifier vòng 3: mỗi `changeSlotConfig` gọi `onAfterRestart` → lưu; xoá cờ trước vòng lặp thì lần lưu đầu ghi slot 1–7 = tắt vào đĩa VÀ vào `settings_`, vòng lặp đọc lại chính nó |
