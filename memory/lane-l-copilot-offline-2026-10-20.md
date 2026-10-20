# Lane L1 — copilot offline (2026-10-20)

**Bối cảnh:** owner bật "God mode" (21/10, qua DEV MASTER): làm tiếp không cần hỏi.
Mọi lane khác đều đã xong, đã dừng (A) hoặc đang chờ dữ liệu (C). L chỉ cần D, mà D
đã hạ cánh.

## Bài học

1. **Copilot = luật cố định + LLM giải thích, không phải LLM đọc log thô.** Luật test
   được và chạy không cần khoá API. `get_findings` đưa cho LLM đúng danh sách luật đã
   tính, nên LLM không tự sinh ra một phát hiện mà luật không thấy.
2. **Không đặt lời gọi mạng vào app âm thanh trực tiếp khi chưa có lời owner.** L1 là
   công cụ dòng lệnh cạnh `logstats.py`, nên `git diff -- src` rỗng và không cần
   release.
3. **Dùng lại `logstats.summarise()`, đừng viết bộ đọc thứ hai.** Các bẫy của lane G
   (`notch_retune` không đóng notch, `ev` là khóa dispatch) đã được giải một lần ở đó.
4. **"Chạm trần" phải loại notch bị đánh dấu oan.** Fixture lane D có một notch −18 dB
   bị đánh dấu oan; tính nó vào là khuyên hạ gain vì một nốt nhạc.
5. **Python SDK `anthropic` 1.15:** khi không có nguồn thông tin đăng nhập nào, SDK ném
   `TypeError` ngay lúc gửi request, không phải `AuthenticationError`; khoá sai mới ra
   401. `tool_runner` nhận `fallbacks="default"` cùng `betas`, `thinking`,
   `output_config` và `max_iterations`.
6. **Test cấm "tăng gain" phải xoá cụm "đừng tăng trần" trước khi so chuỗi.** Không thì
   chính lời cảnh báo làm test đỏ.
