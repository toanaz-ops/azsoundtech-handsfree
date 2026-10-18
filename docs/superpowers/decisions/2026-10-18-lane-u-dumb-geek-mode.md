# Sổ quyết định — Lane U: Dumb mode ⇄ Geek mode (2026-10-18)

Mỗi mục là một ngả rẽ thiết kế đã hỏi owner trong brainstorm. Ghi đủ: câu hỏi,
các phương án đã đề xuất (kèm phương án khuyên dùng), và lựa chọn của owner.
Lật lại khi cần đổi hướng; đừng hỏi lại câu đã có đáp án ở đây.

**Trạng thái:** Q1–Q4 owner chọn 2026-10-18; Q5–Q12 điều phối chốt tạm, **owner chấp nhận ở Q13** ("merge, chạy tiếp cho tới khi xong", 2026-10-18) — lật lại sau khi nghe rig.

Spec đích: `docs/superpowers/specs/2026-10-18-dumb-geek-mode-design.md` (rev 3).
Người hỏi: Claude (điều phối). Người quyết: owner (ToanAZ).

## Bối cảnh nêu trước khi hỏi

- Quyết định owner 06/09/2026 (`docs/research/2026-09-06-cedar-realtime-plugins.md`
  §6.1): hai chế độ chuyển qua lại được, Dumb ≤ 3 nút, Geek = console đầy đủ
  hiện tại. Không bỏ console hiện tại.
- Điều khiển hiện có trên `ModeRail`: `SOUNDCHECK` (thụ động 15 s), `ĐO`
  (lane M, sweep chủ động), `AUTO`, `BYPASS`, `CLEAR ALL`. Ngoài ra TuningPanel
  (Safe/Balanced/Aggressive + 5 combo), SlotPanel (routing), DevicePanel,
  SpectrumView, NotchListPanel (verdict oan/đúng).
- Lane G đã hạ cánh (1.2.0): độ sâu tự đào theo nhu cầu, nhả dần — nên Dumb
  mode không cần slider độ sâu; trần = preset.
- Lane R có mức RING RISK theo slot — nguồn sẵn có cho một "đèn" trạng thái.
- Ràng buộc từ lane M Q10: chữ chính trong Dumb mode là câu người, không phải số.
- Lane U không đụng audio path: chỉ GUI + lưu một cờ chế độ.

## Q1 — Ba nút của Dumb mode là gì?

| # | Phương án | |
|---|---|---|
| 1 | **`ĐO PHÒNG`** (lane M) · **`BẬT CHỐNG HÚ`** (= AUTO) · **`TẮT`** (= BYPASS). CLEAR ALL và SOUNDCHECK thụ động chỉ ở Geek | khuyên dùng |
| 2 | `AUTO` · `BYPASS` · `CLEAR ALL` — không có đo phòng ở Dumb | |
| 3 | `ĐO PHÒNG` · một nút gạt `BẬT/TẮT` (AUTO⇄BYPASS) · `XOÁ HẾT` (CLEAR ALL) | |

**Chọn: 1.**

## Q2 — Chuyển Dumb ⇄ Geek bằng cách nào?

| # | Phương án | |
|---|---|---|
| 1 | **Nút nhỏ cố định ở góc (`GEEK` / `ĐƠN GIẢN`)**, nhớ chế độ qua lần mở app; cài mới mặc định Dumb | khuyên dùng |
| 2 | Lối vào Geek bị khoá (giữ nút 3 s hoặc mật khẩu) để người không chuyên không lạc vào | |
| 3 | Chọn chế độ một lần lúc khởi động app | |

**Chọn: 1.**

## Q3 — Ngoài ba nút, màn hình Dumb hiện gì?

| # | Phương án | |
|---|---|---|
| 1 | **Một đèn trạng thái lớn bằng câu người** lấy từ RING RISK + số notch: "An toàn" / "Sắp hú" / "Đang chặn 3 điểm"; không có phổ | khuyên dùng |
| 2 | Đèn trạng thái + phổ thu nhỏ (chỉ xem, không chạm) | |
| 3 | Chỉ ba nút, không gì khác | |

**Chọn: 1.**

## Q4 — Thiết bị / routing / slot khi đang ở Dumb mode?

| # | Phương án | |
|---|---|---|
| 1 | **Dumb dùng nguyên cấu hình cuối cùng làm ở Geek**; nếu chưa có thiết bị hợp lệ thì đèn hiện "Cần cài đặt — bấm GEEK"; mọi slot dùng chung 3 nút (áp cho tất cả slot) | khuyên dùng |
| 2 | Dumb có trình hướng dẫn chọn thiết bị riêng (3 bước) | |
| 3 | Dumb chỉ điều khiển slot đang chọn ở Geek | |

**Chọn: 2.** — owner chọn wizard riêng trong Dumb (không theo khuyên dùng).

## Bối cảnh phát hiện sau Q4 (đọc code 2026-10-18, main `f36ce29`)

- **Cấu hình thiết bị và routing KHÔNG được lưu giữa hai lần mở app.**
  `AudioEngine::start()` (`src/app/AudioEngine.cpp:50`) mở `desiredDeviceType_`
  (mặc định ASIO) + thiết bị mặc định của driver; `SlotConfig` chỉ sống trong
  RAM hoặc trong preset. Thứ duy nhất trong `%APPDATA%/AZSoundtech/HandsFree/`
  là `presets/`, `logs/`, `license.key`. Hệ quả cho Q4-2: wizard mà không có
  chỗ lưu thì chạy lại MỖI lần mở app.

## Q5 — Wizard ghi cấu hình vào đâu? (điều phối tự chọn)

| # | Phương án | |
|---|---|---|
| 1 | **File mới `settings.json` cạnh `presets/`**: chế độ UI + loại driver + tên thiết bị + sample rate + buffer + 8 `SlotConfig`. Nạp lúc khởi động cho CẢ HAI chế độ (Geek cũng được nhớ thiết bị — thay đổi hành vi có chủ đích) | khuyên dùng |
| 2 | Wizard ghi một preset tên cố định `_dumb.json` | |
| 3 | Không lưu; wizard chạy mỗi lần mở | |

**Chọn: 1.** — điều phối tự chọn 2026-10-18, owner CHƯA duyệt.

## Q6 — Wizard có mấy bước, tạo ra gì? (điều phối tự chọn)

| # | Phương án | |
|---|---|---|
| 1 | **Ba bước**: (1) driver + thiết bị, (2) mic vào kênh nào — một kênh (mono) hay một cặp (stereo), có đồng hồ mức vào sống, (3) ra kênh nào. Kết quả: **slot 0** bật với cấu hình đó, slot 1–7 tắt. Nếu Geek đang bật > 1 slot thì hỏi xác nhận trước khi ghi đè | khuyên dùng |
| 2 | Wizard cho thêm nhiều slot | |
| 3 | Wizard chỉ chọn thiết bị, routing giữ mặc định {0,1}→{0,1} | |

**Chọn: 1.** — điều phối tự chọn 2026-10-18, owner CHƯA duyệt.

## Q7 — Khi nào wizard hiện? (điều phối tự chọn)

| # | Phương án | |
|---|---|---|
| 1 | **Tự hiện khi vào Dumb mà chưa có cấu hình hợp lệ** (không có `settings.json`, thiết bị đã lưu không còn, hoặc slot 0 không hợp lệ với số kênh). Đèn "Cần cài đặt" bấm được để mở lại. Không thêm nút thứ tư | khuyên dùng |
| 2 | Thêm nút `CÀI ĐẶT` thường trực (vượt giới hạn 3 nút) | |

**Chọn: 1.** — điều phối tự chọn 2026-10-18, owner CHƯA duyệt.

## Q8 — `ĐO PHÒNG` ra kết quả thì Dumb làm gì? (điều phối tự chọn)

| # | Phương án | |
|---|---|---|
| 1 | **Tái dùng nguyên luồng lane M** (`beginSoundcheck`, cùng hộp thoại xác nhận). Trong lúc đo, `ĐO PHÒNG` thành `DỪNG`; khi có kết quả, đèn đọc "Tìm thấy N điểm dễ hú" và hai nút `ÁP DỤNG` / `BỎ` thay TẠM hai nút kia. Không có số dB/Hz | khuyên dùng |
| 2 | Dumb tự áp dụng kết quả không hỏi | |

**Chọn: 1.** — điều phối tự chọn 2026-10-18, owner CHƯA duyệt.

## Q9 — `settings.json` nằm trong app hay thư viện? (điều phối tự chọn)

| # | Phương án | |
|---|---|---|
| 1 | **Thư viện `az_host_settings`** (CMake target riêng, chỉ `juce_core`), vì "JUCE host shell" là ứng viên thư viện trong CLAUDE.md và voice isolator (`cedar-realtime-plugins.md` §7) cần đúng phần nhớ thiết bị này. Wizard + DumbView ở lại app | khuyên dùng |
| 2 | Để trong `src/app/`, tách khi sản phẩm hai có code | |

**Chọn: 1.** — điều phối tự chọn 2026-10-18, owner CHƯA duyệt.

## Q10 — `TẮT` trong lúc đang đo? (sinh ra từ phản biện vòng 1, B-5; điều phối tự chọn)

| # | Phương án | |
|---|---|---|
| 1 | **Khoá, y như `BYPASS` của Geek** (`setModeControlsEnabled(!measuring)`, `MainComponent.cpp:1477`); `DỪNG` ở ngay nút trái | khuyên dùng |
| 2 | `TẮT` = `DỪNG` rồi Bypass khi controller về Idle — một đường Geek không có | |
| 3 | `TẮT` gọi `requestMode(Bypass)` ngay giữa sweep — `requestMode` không dừng lần đo | |

**Chọn: 1.** — điều phối tự chọn 2026-10-18, owner CHƯA duyệt.

## Q11 — Tester 1.3.x mở 1.4.0 lần đầu (chưa có `settings.json`)? (vòng 1, I-13; điều phối tự chọn)

| # | Phương án | |
|---|---|---|
| 1 | **Có thư mục `presets/` → Geek; không có → Dumb** | khuyên dùng |
| 2 | Luôn Dumb + wizard | |

**Chọn: 1.** — điều phối tự chọn 2026-10-18, owner CHƯA duyệt.

## Q12 — Thiết bị đã lưu không còn lúc mở app? (vòng 1, B-3; điều phối tự chọn)

| # | Phương án | |
|---|---|---|
| 1 | **Không áp routing đã lưu lên thiết bị JUCE mở thay; tắt slot; "Cần cài đặt"; không ghi đè trí nhớ** | khuyên dùng |
| 2 | Áp routing đã lưu nếu chỉ số kênh còn trong biên | |

**Chọn: 1.** — điều phối tự chọn 2026-10-18, owner CHƯA duyệt.

## Q13 — Merge PR #7 với các phương án Q5–Q12 đang chốt tạm?

| # | Phương án | |
|---|---|---|
| 1 | **Merge; chấp nhận tạm Q5–Q12, lật lại sau khi nghe rig** (tiền lệ lane M Q24) | khuyên dùng |
| 2 | Duyệt từng mục Q5–Q12 trước khi merge | |

**Chọn: 1.** — owner: "merge, chạy tiếp cho tới khi xong" (2026-10-18). Lời này không
trả lời riêng từng mục; đọc là chấp nhận các phương án khuyên dùng như lane M Q24.
