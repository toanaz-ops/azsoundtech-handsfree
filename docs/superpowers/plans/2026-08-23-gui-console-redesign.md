# GUI "Console" Redesign Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development
> (khuyến nghị) hoặc superpowers:executing-plans. Steps dùng checkbox (`- [ ]`).

**Goal:** Xây giao diện "Console công nghiệp" theo spec đã duyệt: theme system
một nguồn thật, `SpectrumView` vẽ từ snapshot, `ModeRail`, `StatusBadge`,
`NotchListPanel`, `DeviceDrawer`, và hai bố cục L1/L2 chuyển đổi được (mặc định L2).

**Architecture:** GUI đọc duy nhất qua `NotchController::copySnapshot()` + API
công khai có sẵn; mọi màu/font/kích thước nằm trong `AzTheme` áp qua
`AzLookAndFeel`; layout đặt bằng FlexBox/Grid trong `resized()`, đổi L1↔L2 bằng
re-parent component. Không đụng đường audio thread.

**Tech Stack:** C++17, JUCE 9, GoogleTest 1.14, CMake + VS 18 2026.

**Spec:** `docs/superpowers/specs/2026-08-23-gui-console-redesign-design.md`
(quyết định G-1..G-5 — đọc cả spec lẫn plan này trước khi code).

## Global Constraints

- **Cấm hex literal ngoài `src/gui/theme/`** — mọi màu qua theme.
- Target chạm ≥ 44 px; rail rộng 96 px; nút 96×64; gap 8 px; bo góc 4 px;
  base font 13 px; số luôn mono.
- Paint path của `SpectrumView` **không cấp phát** (buffer caller-owned).
- CLEAR ALL luôn mở hộp xác nhận.
- Không thêm đường nào từ GUI vào audio thread; không sửa `AudioEngine.cpp`.
- Full suite phải xanh sau mỗi task (`ctest -C Release`); commit đường dẫn tường minh.
- Kết quả thẩm mỹ cuối cùng → người nhìn mắt ("needs human look").

---

### Task 0: AzTheme + AzLookAndFeel

**Files:** Create `src/gui/theme/AzTheme.h/.cpp`; Modify `CMakeLists.txt`
(target_sources HandsFree), `tests/CMakeLists.txt` (thêm .cpp nếu test cần);
Test `tests/test_aztheme.cpp`.

**Interfaces produced:** `namespace az::theme` với các constexpr token đúng bảng
spec §1 (màu `#0B0F14/#161E27/#232C36/#C8D6E5/#546E7A/#4FC3F7/#FF9800/#81C784/
#FF8A65/#FFB74D`, spacing 4, rail 96, button 96×64, gap 8, corner 4, font 13);
`class AzLookAndFeel : juce::LookAndFeel_V4` áp palette + mono cho số.

- [ ] Test: `AzTheme::applyTo(component)` → component tìm được LookAndFeel là
  `AzLookAndFeel*`; grep-test dạng unit: danh sách file `src/gui/**.cpp` không
  chứa hex literal `#[0-9A-Fa-f]{6}` ngoài thư mục theme (đọc file như text,
  exception list rỗng).
- [ ] Implement theme tokens + `AzLookAndFeel` (drawButtonBackground, Label,
  ComboBox theo palette).
- [ ] Áp `AzLookAndFeel` trong `MainComponent` constructor (JUCE default LAF
  bị thay thế một lần duy nhất).
- [ ] Build + full suite xanh → commit `feat(gui): AzTheme tokens + AzLookAndFeel`.

### Task 1: SpectrumView

**Files:** Create `src/gui/SpectrumView.h/.cpp`; Modify CMake (2 file), Test
`tests/test_spectrumview.cpp`.

**Interfaces:**
- Consumes: `NotchController::SnapshotBuffer` / `copySnapshot()` (đã có),
  `az::theme` tokens.
- Produces: `class SpectrumView : juce::Component, juce::Timer` — ctor nhận
  `const NotchController&`; `void refreshFromSnapshot()` copy snapshot vào member
  buffer rồi `repaint()`; Timer 30 fps gọi refresh khi `isVisible()`.
- Vẽ: trục log-freq 20 Hz–20 kHz (tick 100/1k/10k), Y dB; spectrum line từ
  `magnitudes[]` (skip khi `sequence == 0` → lưới + chữ "no signal"); marker ▼
  cam tại freq notch active, cao độ ∝ depthDB (map −24..0 dB → 40..100% marker
  height), notch "mới" (dựa thứ tự publish gần nhất — v1 đơn giản: cùng màu cam,
  ghi chú mở rộng sau) ; tần số tick vẽ mono.

- [ ] Test (headless component): (a) feed snapshot giả 513 bins + 1 notch →
  sau `refreshFromSnapshot()` paint không crash và `getSequenceSeen()` trả đúng
  sequence; (b) gọi paint 100 lần trên buffer không đổi → không tăng số phần tử
  vector thành viên (assert cap cố định — chứng minh không cấp phát lặp).
- [ ] Implement → build + suite xanh → commit `feat(gui): SpectrumView log-freq canvas with notch markers`.

### Task 2: ModeRail + StatusBadge

**Files:** Create `src/gui/ModeRail.h/.cpp`, `src/gui/StatusBadge.h/.cpp`;
Modify CMake; Test `tests/test_moderail.cpp`.

**Interfaces:**
- Consumes: callbacks kiểu `std::function<void()>` do MainComponent wire:
  `onSoundcheck, onAuto, onBypass, onClearAllConfirmed`;
  `std::function<double()> getSoundcheckRemainingMs`.
- Produces: `ModeRail(Orientation)` — `enum class Orientation { Vertical,
  Horizontal }`; nút ≥44 px (cell 96×64 vertical); CLEAR ALL mở
  `juce::AlertWindow` xác nhận CHƠI xong mới gọi callback; nút mode active tô
  nền ok-green; StatusBadge: `void setState(ProtectionState)` với
  `enum class ProtectionState { Protecting, Idle, Bypassed }`.

- [ ] Test: bấm AUTO (gọi `triggerClick` headless) → callback phát đúng 1 lần;
  CLEAR ALL KHÔNG callback nếu chưa confirm; Soundcheck label hiển thị giây còn
  lại khi > 0.
- [ ] Implement theo theme → commit `feat(gui): ModeRail with confirm-guarded clear-all, StatusBadge`.

### Task 3: Layouts L1/L2 + DeviceDrawer + persistence

**Files:** Modify `src/app/MainComponent.h/.cpp` (nặng nhất của phase); Create
`src/gui/DeviceDrawer.h/.cpp`; Modify CMake; Test `tests/test_gui_wiring.cpp`
(mở rộng).

- [ ] Enum `enum class ScreenLayout { Classic /*L1*/, Performance /*L2*/ };`
  lưu `ApplicationProperties` key `"layout"`, mặc định Performance.
- [ ] `resized()` viết bằng FlexBox/Grid thuần cho cả hai layout; re-parent
  `SpectrumView/ModeRail/StatusBadge/NotchListPanel/DeviceDrawer` theo layout;
  `setResizeLimits(...)` min window đủ rail+spectrum.
- [ ] DeviceDrawer: bọc `DevicePanel` hiện có; L2 = toggle ⚙ mở/đóng; L1 =
  thanh trên như cũ. Thêm hàng Settings nhỏ: toggle L1/L2.
- [ ] Test wiring: chuyển layout → property ghi đúng giá trị; khởi động lại
  component → khôi phục layout đã lưu; min-size không cho phép bounds chồng lấn
  (assert bounds giao nhau = 0 giữa rail và spectrum ở size tối thiểu).
- [ ] Commit `feat(gui): switchable L1/L2 layouts with persisted preference`.

### Task 4: NotchListPanel

**Files:** Create `src/gui/NotchListPanel.h/.cpp`; Modify CMake; Test
`tests/test_notchlistpanel.cpp`.

- [ ] Bảng # / FREQ / DEPTH / Q / STATUS(kèm tuổi "12s") từ SnapshotBuffer;
  header mono dim; hàng cách nhau bằng viền border; L2 = panel trượt (toggle),
  L1 = đáy cố định (chỉ set visible/bounds, panel tự thân vô tri về layout).
- [ ] Test: feed snapshot 2 notches → model rows đúng 2 dòng, format freq
  "987 Hz" / "2.4 kHz", tuổi tính từ sequence time.
- [ ] Commit `feat(gui): NotchListPanel from snapshot`.

### Task 5: Verify + closeout

- [ ] Full reconfigure + Release + `ctest -C Release` — paste output.
- [ ] Grep kiểm chứng: `rg "#[0-9A-Fa-f]{6}" src/gui --glob "!theme/*"` → rỗng.
- [ ] Chạy app thật, chụp màn hình cả L1 + L2 gửi chủ dự án → **human look gate**.
- [ ] progress.md + memory note (nếu có bài học) + handoff session kế.

**Ngoài scope v1 (spec §7):** popover chi tiết notch, Link L/R UI, preset UI,
icon app, i18n.
