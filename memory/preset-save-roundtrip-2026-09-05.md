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

## 3. ~~Dedup theo lane~~ → MỘT `PresetNotch` cho mỗi (slot, làn, index)

**Bài học chính, rút ra khi merge (05/09/2026).** Bản đầu (lane P) gộp hai lane
lại: `adoptPreset` cài mỗi notch lên MỌI lane (`width_`) nên snapshot liệt kê
cùng một chain-`index` trên channel 0 và 1, và code emit MỘT `PresetNotch` mỗi
`(slot,index)`, giữ channel thấp nhất. Lúc đó đúng — hai lane mang tham số y hệt
nên chọn lane nào cũng như nhau.

Lane S (dò theo làn) làm giả thiết đó sai mà **build vẫn xanh và test vẫn xanh**:
với INDEP, lane 0 và lane 1 mang notch KHÁC nhau ở cùng index, nên dedup âm thầm
vứt sạch notch lane 1 — soundman lưu một rig đã chỉnh và mở lại được một nửa.
Test cũ còn *khẳng định* hành vi dedup (`ASSERT_EQ(notches.size(), 1u)`), tức là
nó khoá luôn cái bug vào chỗ.

Quy tắc bây giờ: `savePreset` ghi một `PresetNotch` cho MỖI notch trong snapshot,
`pn.lane = sn.channel`, không gộp; cộng section `"slots"` mang routing + `linked`
của từng slot đang bật. `PresetManager::validate` tính duy nhất theo
**(slot, làn, index)**, và `lane` = −1 (mọi làn) đụng với bất kỳ làn nào ở cùng
index.

Điều đáng nhớ: một hằng số ngữ nghĩa ("hai lane luôn giống nhau") do một lane
khác giữ thì merge sẽ **không** báo conflict — file auto-merge sạch sẽ và sai.
Khi merge hai lane cùng chạm một luồng dữ liệu, hãy đi tìm giả thiết chứ đừng chỉ
đọc marker conflict.

## Liên quan
- [[gui-console-lessons-2026-08-24]] — FileChooser async: `onPicked` bắt `[this]`
  đi qua ranh giới async phải bọc `Component::SafePointer` (UAF khi cửa sổ đóng
  lúc hộp thoại mở). Bẫy này TÁI XUẤT ở P3, review chặn lại (round 1 fix).
- [[juce9-api-traps-2026-08-25]] — nút preset dựng `TextButton{"LOAD..."}` một
  tham số, tránh bẫy 2 tham số → tooltip.
