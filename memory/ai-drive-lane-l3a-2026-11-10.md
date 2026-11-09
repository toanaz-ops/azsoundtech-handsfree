# Lane L3a — AI drive, pha 0 dB (10/11/2026)

Pha đầu của "AI tự lái notch": công tắc TẮT / BÓNG / GỢI Ý / TỰ LÁI (khoá), tác giả notch, đánh dấu trên biểu
đồ, log schema 4, bộ giám sát LLM chỉ thu đề xuất. Fable review cả pha: 0 High, 7 Medium, 8 Low. Bài học:

## Hợp đồng giữa hai nửa (C++ ↔ tiến trình phụ)

- **Luật mà ranh giới tin cậy ép thì model phải được thấy.** C++ chỉ nhận đào sâu đúng một bậc; tiến trình phụ
  chỉ kiểm "sâu hơn, trong trần" và không nói gì về bậc thang ⇒ model đề xuất −18 từ −6, tiến trình phụ báo
  "queued", app từ chối sau khi lượt gọi đã tốn tiền. Sửa: file trạng thái mang `next_rung_db` do **cùng một
  hàm** với bộ kiểm C++ tính ra; tiến trình phụ từ chối `not_next_rung`. Ghim bằng một fixture dùng chung
  (`tests/fixtures/ai-state-schema2.json`) mà cả test Python lẫn test C++ đều đọc.
- **Hạn giờ của tiến trình con phải do chính nó giữ.** Watchdog 60 s của app nhỏ hơn 12 lượt × 50 s mỗi
  request ⇒ app kill giữa chừng, mất hết đề xuất, các lượt đã chạy vẫn bị tính tiền, lặp lại mỗi trigger. Ba
  con số (request 30 s, cả lượt 90 s, kill 105 s) định nghĩa một chỗ, `static_assert` kill ≥ hạn + 10 s; hết
  hạn thì con trả cái đã có thay vì bị giết.
- Khi hai agent sửa hai nửa song song: viết hợp đồng (tên khoá, kiểu, mã từ chối, con số) **vào cả hai brief**,
  chia theo quyền sở hữu file, không theo số finding.

## Tiền và sự đồng ý

- **Giới hạn chi tiêu trong bộ nhớ + chế độ được lưu = ngân sách nhân lên theo số lần mở app.** Trần theo giờ
  phải lưu (giờ đồng hồ, mốc ở tương lai tính là "vừa xong"); lượt đầu cũng phải chờ khoảng cách tối thiểu.
- **Rút đồng ý phải huỷ việc đang chạy, không chỉ ghi settings.** Bộ nhớ đệm 5 s của "dùng được" là một lỗ đồng
  ý: thêm đường huỷ + xoá trigger chờ + xoá đệm ở mọi nơi tắt đồng ý / xoá khoá.
- Lỗi ghi file trạng thái mà `return` sớm, không xoá trigger ⇒ vòng thử lại 500 ms kèm một dòng log mỗi lần.

## Test

- **Test hợp đồng mà rig không giữ tham chiếu tới thứ nó bảo vệ thì không thể đỏ.** `SuggestEmitsNoCommand` dùng
  một bản chụp trạng thái; nối `apply` vào `MainComponent` thì nó vẫn xanh. Test thật: `MainComponent` thật,
  kết quả hợp lệ đi qua đúng `onResult`, so toàn bộ snapshot notch, kèm một đối chứng âm (một thay đổi tay
  thật phải làm phép so đỏ).
- Đề xuất đang hiện phải được **kiểm lại mỗi tick** bằng chính bộ kiểm lúc nhận: dữ liệu AI cũ 2–60 s, đề xuất
  sống 10 phút, còn cú hú dựng trong 100–500 ms (đề xuất "nhả" nằm cạnh RING RISK CRITICAL).

## GUI

- Ảnh dàn dựng của `HandsFreeSnapshot` phải đi qua **bộ kiểm production**: hai lần liền ảnh dựng một trạng thái
  mà app thật sẽ từ chối (đề xuất không hợp lệ; đề xuất nhả cạnh chip CRITICAL).
- Console diễn không có chuột rê: lời giải thích cho nấc bị khoá là nhãn luôn hiện, không phải tooltip
  (`juce::TooltipWindow` toàn app đã bị gỡ).
- "AUTO" đã là tên mode engine ⇒ nấc AI gọi là `TỰ LÁI`. `nid` cấp theo controller nên trùng giữa các slot ⇒
  phía AI dùng `slot << 24 | nid`.

## Còn mở

- Chưa có lượt gọi nhà cung cấp thật nào: hạn 90 s, chi phí và chất lượng đề xuất đều chưa đo.
- 40 lượt/tiến trình vẫn đầy lại mỗi lần mở app (trần theo giờ là cái chặn vòng crash).
- Bảng COPILOT ở chiều cao tối thiểu có thể che danh sách đề xuất (L6); `setNotchAuthoredForTest` nằm trên lớp
  production (L7); notch từ preset / soundcheck ghi tác giả `human` (L5, đã ghi vào tài liệu).
