# Preset SAVE round-trip design — 2026-09-05

Từ LANE P (chuỗi preset, branch `feat/preset-chain`, `MainComponent::savePreset`).
Ba điều không hiển nhiên khi gom "trạng thái notch hiện tại" thành một `Preset`
mở lại được — cả ba đều buộc phải làm đúng nếu không round-trip test đỏ hoặc
`saveToFile` từ chối im lặng.

## 1. `Preset.sampleRate` PHẢI lấy từ snapshot, không được để 0

`PresetManager::saveToFile` gọi `validate` TRƯỚC rồi mới ghi; `validate` từ chối
`sampleRate <= 0` và kiểm mỗi `freq < 0.5*sampleRate` (Nyquist của FILE). Nếu
`savePreset` để `sampleRate=0` (app idle/không device) mà vẫn có notch → validate
đỏ → ghi KHÔNG gì, `saveToFile` trả false. Đúng nguồn: `SnapshotBuffer::sampleRate`
— notch được publish ở rate đó nên `freq < rate/2` đúng theo cấu trúc. Lấy rate
non-zero đầu tiên qua các slot (mọi slot chung device rate). Ca không có gì để
lưu → để `saveToFile` từ chối và trả bool, KHÔNG bịa rate/notch.

## 2. Snapshot chỉ refresh khi `runOnce()` xử lý MỘT block — không phải khi adopt

`adoptPreset`/detection cập nhật `model_` nhưng KHÔNG publish `latest_`. Snapshot
(`copySnapshot`) chỉ đổi trong `runOnce()` khi có block phổ (NotchController.cpp
~189-194). Hệ quả cho test headless: sau `adoptPreset` phải bơm 1 block vào tap
rồi `runOnce()` thì snapshot mới mang notch. Mẫu: `engine.getTapBuffer(slot)
.write(hop...)` → `getNotchControllerForTest(slot)->runOnce()` → `copySnapshot`.
Trong app thật detector thread chạy liên tục nên snapshot luôn tươi — chỉ test
mới cần bơm tay. (Xem `tests/test_notchcontroller.cpp` cho mẫu feed.)

## 3. Dedup theo lane khi đọc snapshot ngược ra `PresetNotch`

`adoptPreset` cài mỗi notch lên MỌI lane (`width_`), nên snapshot liệt kê cùng
một chain-`index` trên channel 0 và 1. Emit MỘT `PresetNotch` mỗi `(slot,index)`
— giữ channel thấp nhất (snapshot lặp channel tăng dần, first-seen = ch0). Format
preset không có trường per-channel; hai lane mang tham số y hệt nên chọn lane nào
cũng đúng. Round-trip test (load 1 notch → save → reload → đúng 1 notch) chốt
việc này.

## Liên quan
- [[gui-console-lessons-2026-08-24]] — FileChooser async: `onPicked` bắt `[this]`
  đi qua ranh giới async phải bọc `Component::SafePointer` (UAF khi cửa sổ đóng
  lúc hộp thoại mở). Bẫy này TÁI XUẤT ở P3, review chặn lại (round 1 fix).
- [[juce9-api-traps-2026-08-25]] — nút preset dựng `TextButton{"LOAD..."}` một
  tham số, tránh bẫy 2 tham số → tooltip.
