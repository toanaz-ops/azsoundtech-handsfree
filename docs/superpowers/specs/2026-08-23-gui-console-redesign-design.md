# Design — GUI "Console" redesign (giao diện A + layout L1/L2)

**Ngày:** 2026-08-23 · **Chủ:** AZ · **Tình trạng:** ĐÃ DUYỆT qua brainstorm có
mockup (`.superpowers/brainstorm/4109-1787504806/`). Thay thế phần GUI của spec
§6.1 về *hình thức*; chức năng §6.1 vẫn là nguồn thật.

## 0. Quyết định của chủ dự án

| # | Quyết định |
|---|---|
| G-1 | Ngôn ngữ thị giác: **A — Console công nghiệp** (đen tuyệt đối, viền mảnh, mono). Không glassmorphism, không glow trang trí |
| G-2 | Bố cục: **L1 (dọc spec) VÀ L2 (performance rail) đều có**, chuyển trong Settings, **mặc định L2** |
| G-3 | Nhập liệu: **chuột + cảm ứng** — target chạm ≥ 44 px ở mọi nút bấm được |
| G-4 | App hướng **freeware** (không thu phí; `LicenseManager` tạm giữ nguyên, quyết định gỡ/bỏ là việc riêng) |
| G-5 | Hấp thụ Surge XT / Vital **chỉ ở mức pattern + ngôn ngữ thị giác — không copy code/GPL asset nào** vào repo |

## 1. Theme system — một nguồn sự thật

`src/gui/theme/AzTheme.h` (+ `.cpp` nếu cần):

```
// palette
background    #0B0F14   panel   #161E27   border  #232C36
text          #C8D6E5   dim     #546E7A
accent        #4FC3F7   warn    #FF9800   ok      #81C784
danger        #FF8A65   marker  #FFB74D
// metrics
spacing grid  4px       touch target ≥ 44px     rail width 96px
button cell   96×64px   corner radius 4px       base font 13px
numbers font  mono (Consolas fallback)
```

- Cài qua `AzLookAndFeel : juce::LookAndFeel_V4`. **Cấm hardcode màu** trong
  component — mọi giá trị đọc từ theme.
- Số (Hz, dB, countdown) luôn vẽ bằng font mono, cỡ lớn nhất vừa trong ô.

## 2. Components

### SpectrumView (`src/gui/SpectrumView.h/.cpp`)
- Canvas chính. Trục X log-frequency 20 Hz–20 kHz, trục Y dB.
- Vẽ từ `NotchController::copySnapshot(dest)` vào buffer **caller-owned,
  pre-allocated** — paint path không cấp phát (bắt buộc Task 21).
- Marker ▼ cam tại freq mỗi notch active; **cao độ marker ∝ depthDB**;
  notch mới detect (< ~5 s, so tuổi qua snapshot sequence/thời điểm) vẽ màu
  khác để mắt bắt được thay đổi.
- `juce::Timer` 30 fps; khi không có block mới (sequence không đổi) thì bỏ qua
  repaint.

### ModeRail (`src/gui/ModeRail.h/.cpp`)
- Ba mode SOUNDCHECK / AUTO ● / BYPASS + nút CLEAR ALL màu danger.
- Soundcheck đang chạy hiển thị **countdown còn lại** (`getSoundcheckRemainingMs()`).
- CLEAR ALL **luôn mở hộp xác nhận** trước khi gọi `clearAll()`.
- Hai orientation: dọc (L2, phải) / ngang (L1, dưới device bar). Component
  giống nhau, chỉ đổi hướng stack.

### StatusBadge
- `● PROTECTING` (Auto/Soundcheck + detectionActive), `IDLE`, `BYPASSED`.
- Nguyên tắc: nhìn 1 giây trả lời được "app đang bảo vệ không?".

### NotchListPanel
- Bảng # / FREQ / DEPTH / Q / STATUS (kèm "2m ago" từ model age).
- L2: panel trượt ra khi bấm; L1: nằm đáy cố định. Click hàng → chi tiết
  (popover) — để sau, không nằm scope v1.

### DeviceDrawer
- Bọc `DevicePanel` + `DeviceViewModel` HIỆN CÓ (tái sử dụng, không viết lại).
- L1: thanh trên như cũ. L2: rút vào ngăn kéo ⚙ trên cùng bên phải.
- Thêm hàng điều khiển mới: **Layout L1/L2 toggle** + (chỗ để preset sau này).

## 3. Layout & kỷ luật chống lệch/tràn (bài học review)

- `MainComponent::resized()` dùng `juce::FlexBox`/`Grid`, **cấm toạ độ tuyệt đối**.
- Rail: rộng cố định 96 px, nút xếp lưới 96×64, gap 8. Spectrum nhận phần còn lại.
- `setResizeLimits` đặt minimum window đủ chứa rail; dưới ngưỡng không chồng lấn.
- Chuyển L1↔L2 = re-parent các component đã có vào bố cục khác — component
  không tự bố trí theo kiểu riêng. Lựa chọn lưu `ApplicationProperties`,
  khôi phục lần mở sau.

## 4. Luồng dữ liệu & an toàn thread

- GUI đọc **duy nhất** `NotchController::copySnapshot()` (mutex bên trong) và
  API công khai: `startSoundcheck`, `setDetectionActive`, `clearAll`,
  `getSoundcheckRemainingMs`, engine mode/device APIs.
- Không thêm bất kỳ đường nào từ GUI vào audio thread. Không đụng `AudioEngine.cpp`
  ngoài setter mode đã có.

## 5. Lỗi & trạng thái rỗng

- Không device: spectrum phẳng, badge `IDLE`, DeviceDrawer nhấn mạnh lỗi
  `getLastDeviceError()`.
- Snapshot sequence 0 (chưa có audio): vẽ lưới trục + chữ "no signal" mờ,
  KHÔNG vẽ marker.

## 6. Kiểm thử

| Hành vi | Test |
|---|---|
| ModeRail phát đúng lệnh | unit: click AUTO → `setMode(Auto)` được gọi qua ViewModel fake |
| CLEAR ALL xác nhận | unit: không clear nếu chưa confirm |
| Layout switch | unit: chuyển L1↔L2 lưu/khôi phục; component set đúng bounds cha |
| SpectrumView không cấp phát trong paint | test smoke: 100 repaint không tăng allocation counter (nếu đo được) hoặc review code chốt |
| Theme phủ hết | grep: không hex literal ngoài theme file trong `src/gui/` |

Mọi thứ thẩm mỹ cuối cùng → **người nhìn mắt** (quy tắc dự án).

## 7. Ngoài scope v1

Popover chi tiết notch, Link L/R toggle UI, preset manager UI (Tasks 25–26),
đổi icon app, i18n.
