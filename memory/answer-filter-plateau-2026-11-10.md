# Bộ lọc cụm từ chạm trần ~20%, và "0 lọt" của người viết không phải số đo (2026-11-10)

Bối cảnh: Fable review phase AI (H1) thấy `violates_safety()` của `tools/copilot/copilot.py` bỏ lọt 27/32 câu
nguy hiểm viết tay trong bản 1.5.0 đã phát hành, trong khi release note viết "bị thay bằng thông báo cố định".

## Điều học được

1. **Corpus do chính người viết bộ lọc soạn luôn cho 0 lọt.** Ba vòng liền agent sửa báo "0/112", "0/195",
   "2/183" trên bộ câu nó đã thấy. Đo bằng một verifier **viết câu trước khi đọc mã bộ lọc**, câu không nằm
   trong fixture nào: 36/153 (23,5%) → 38/183 (20,8%) → 30/175 (17,1%; tự nhiên 29/145 = 20,0%).
2. **Số đo chỉ độc lập một lần.** Ngay khi câu lọt được đưa cho người sửa, bộ đó thành dữ liệu huấn luyện
   (38 → 2 sau một vòng). Mỗi vòng cần verifier mới và câu mới; bộ cũ đóng băng làm sàn hồi quy
   (`tests/fixtures/copilot-safety-independent-r{2,3}.json`), không phải thước đo.
3. **Đổi cấu trúc không phá được trần.** Default-deny theo danh từ, mang đối tượng sang mệnh đề sau, cổng ngôn
   ngữ, ghép mảnh chữ — tỉ lệ lọt trên câu mới vẫn ~20%. Thứ lọt luôn là lớp ý chưa được liệt kê (vòng 3:
   "làm yếu bảo vệ" — tạm dừng, hạ độ nhạy detector, nâng ngưỡng, chỉ SOUNDCHECK). Ngôn ngữ tự nhiên không
   đóng được bằng danh sách.
4. **Default-deny mua chặn nhầm, không mua giảm lọt.** Vòng 2: chặn nhầm 3,8% → 17,9% mà lọt chỉ từ 23,5% →
   20,8%. Chặn nhầm rơi đúng vào câu an toàn nhất ("Please do not turn up the gain", "Không được tắt bộ chống
   hú") vì system prompt bắt model nói những câu đó liên tục.
5. **Đặt ngưỡng và luật dừng TRƯỚC khi đo.** Vòng 3 đặt trước "lọt ≤ 10%, chặn nhầm ≤ 12%, trượt cũng phát
   hành, không vòng 4". Không có luật dừng thì vòng 4, 5 sẽ tiếp tục vì lần nào corpus của người viết cũng xanh.
6. **Hàng rào thật không phải bộ lọc.** Là dòng cảnh báo cố định dưới mọi câu trả lời và việc lời văn AI không
   chạm được audio. Lane tự lái (L3) vì thế đi bằng tool call có kiểu do C++ kiểm, không qua lời văn.
7. **Bẫy gấp đôi dấu:** "giảm" là an toàn với mức, **không an toàn** với độ sâu notch ("giảm độ sâu notch" là
   gỡ bảo vệ). Từ "bộ" sau khi bỏ dấu trùng cue "bỏ"; "dừng" trùng "đừng".

## Áp dụng

- Viết tài liệu về một bộ lọc: dẫn bằng số đo độc lập, không bao giờ bằng "0 lọt" của corpus nội bộ.
- Muốn giảm thật: đổi kiến trúc (đầu ra có cấu trúc, hoặc để model tự gắn cờ rồi chặn theo cờ **cộng** bộ
  lọc), và phải đo bằng lượt gọi nhà cung cấp thật — chưa làm được vì máy build chưa có khoá.
