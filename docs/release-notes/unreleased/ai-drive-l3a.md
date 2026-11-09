# AI và notch, pha L3a: công tắc chế độ, tác giả notch, đề xuất chỉ để xem

**Đụng đường audio:** không. **Mức ra thay đổi dự kiến: 0 dB ở mọi ngõ ra, ở mọi nấc của công tắc.**
Pha này không có đường mã nào để AI đặt, chỉnh, giữ hay nhả notch; nút `ÁP DỤNG` bị khoá.
Chứng minh: `Authorship.DoesNotChangeCommands`, `AiMode.SuggestEmitsNoCommand`,
`AiMode.FailureLeavesDetectorUntouched`, `ShadowClassifier.ScoringDisabledGivesIdenticalCommandsAndNoScore`, và
một test trên `MainComponent` thật đưa kết quả AI hợp lệ qua đúng đường nhận rồi so toàn bộ notch trước/sau.

Thiết kế: `docs/superpowers/specs/2026-11-09-ai-drive-design.md`; quyết định D0–D14 trong
`docs/superpowers/decisions/2026-11-09-ai-drive.md`.

## Tester thấy gì

- Geek mode, đầu bảng `COPILOT`: công tắc `TẮT` / `BÓNG` / `GỢI Ý` / `TỰ LÁI`. `TỰ LÁI` bị khoá (cạnh công tắc
  ghi `TỰ LÁI: bản sau`). Mặc định `BÓNG` — đúng hành vi của 1.5.1.
- `GỢI Ý`: app tự gọi AI khi có sự kiện (tần số tái diễn, notch chạm trần, nhịp 10 phút), bằng khoá đã lưu
  trong `CÀI AI`. Đề xuất hiện dưới nhãn `ĐỀ XUẤT chỉ xem ở bản này` và thành dấu rỗng `+A` trên biểu đồ. Chỉ
  bấm được `BỎ` / `BỎ HẾT`.
  - Giới hạn: cách nhau ≥ 2 phút (lượt đầu chờ 2 phút sau khi mở app), ≤ 12 lượt/giờ (ghi lại, đóng mở app
    không làm đầy lại), ≤ 40 lượt mỗi lần mở app, mỗi lượt tối đa 90 giây. Chưa chỉnh được trong app.
  - Ba lượt hỏng liên tiếp: dòng trạng thái báo `AI · tạm dừng sau 3 lỗi liên tiếp`; đổi nấc công tắc để thử lại.
  - Tắt công tắc đồng ý hoặc xoá khoá: lượt đang chạy bị huỷ ngay.
  - Đề xuất tự biến mất khi không còn đúng (notch đã đổi; đề xuất nhả khi `RING RISK` của slot lên trên mức
    thấp nhất).
- Bảng `ACTIVE NOTCHES` có cột `BY` (`D` detector, `H` người). `N` và `A` chưa xuất hiện ở bản này. `H` gồm cả
  notch từ preset và từ `ÁP DỤNG` của soundcheck.
- `TẮT` giờ cũng dừng việc mạng nơ-ron chấm điểm (log không còn `nn_score`).
- Log: `log_schema: 4`; mọi dòng notch có `author` và `nid`; thêm sự kiện `ai_mode`, `ai_call`,
  `ai_proposal`, `ai_decision`.

## Tester kiểm

1. Đổi nấc công tắc: dòng giải thích bên dưới đổi theo; đóng mở app, nấc được nhớ.
2. Ở `BÓNG` và `TẮT`: không có lượt gọi mạng nào (tường lửa không hỏi, không tốn token).
3. Ở `GỢI Ý` có khoá và đã đồng ý: chạy tới khi một tần số bị đặt notch ba lần — dòng trạng thái có báo "AI ·
   đang xét" không, mất bao lâu, đề xuất có hợp lý không. **Gửi lại file log và ảnh chụp màn hình.**
4. Rút mạng giữa chừng ở `GỢI Ý`: chống hú phải chạy y như cũ; bảng chỉ báo một dòng lỗi, và sau ba lỗi thì
   tạm dừng.
5. Đang ở `GỢI Ý`, tắt công tắc đồng ý trong `CÀI AI`: không còn lượt gọi nào sau đó.
6. Cột `BY` có làm lệch các cột khác không, ở cả slot mono và stereo.

## Giới hạn đã biết

- **Chưa có lượt gọi nhà cung cấp AI thật nào được kiểm** từ máy build (chưa có khoá): độ trễ, chi phí và chất
  lượng đề xuất là chưa biết; hạn 90 giây mỗi lượt là con số đặt trước, chưa đo. Mọi kiểm thử dùng máy chủ giả.
- `GỢI Ý` tốn tiền của khoá bạn lưu dù bạn không bấm gì. Không muốn thì để `BÓNG`.
- Dấu hiệu của mạng nơ-ron ở `GỢI Ý` chưa hiện: model nhúng chưa huấn luyện, ý kiến của nó vô nghĩa.
- AI không biết tuổi thật của notch: nó chỉ biết đã thấy notch đó bao lâu kể từ khi bật `GỢI Ý`.
- Ở cỡ cửa sổ nhỏ nhất, bảng `COPILOT` có thể không đủ chỗ cho danh sách đề xuất (dấu `+A` trên biểu đồ vẫn
  hiện). Chưa dựng ảnh ở cỡ đó.
