# Nghiên cứu chống hú thế hệ kế tiếp — 2026-09-04

**Bối cảnh:** owner hỏi bốn việc: LLM tham gia anti-feedback, tác động
pha/polarity thay vì trừ gain, stereo independent, và đề xuất thêm.
Bản đầy đủ có nguồn: `docs/research/2026-09-04-anti-feedback-next-gen.md`.
Roadmap: `docs/superpowers/specs/2026-09-04-anti-feedback-v2-roadmap.md`.

## Bài học (để không nghiên cứu lại)

1. **App hiện "dò mono, đè stereo".** Tap chỉ làn 0; lệnh notch áp cả
   hai làn. Đây là lỗ hổng thật nhìn thấy từ `AudioEngine.h`, không
   phải giả định của owner. Sửa bằng một detector/slot nhìn cả hai làn
   (không phải 16 thread), vì tỉ lệ L/R là đặc trưng phân biệt hú với
   nhạc.
2. **LLM không bao giờ ở tầng realtime.** Ngân sách 10.7 ms/hop; LLM
   là copilot đọc telemetry (giây–phút) và agent offline (ngày). "AI
   in the loop" đúng nghĩa là mạng nhỏ < 1 ms trên detector thread.
   Tiền đề của cả hai: nút "oan/đúng" + log session. Không có nhãn thì
   không có gì để học.
3. **Lật cực không diệt hú, chỉ dời hú.** Toàn dải hay băng hẹp đều
   vậy. Pha chỉ hữu ích khi (a) đo được pha vòng lặp (soundcheck chủ
   động) hoặc (b) làm pha *biến thiên* (frequency shift, delay
   modulation) — cả hai đều có giá về tone trên music.
4. **AFC là lane cuối, không phải lane đầu.** +10–15 dB GBF nhưng phân
   kỳ = phát tiếng lạ ra PA. Chỉ làm khi đã có phép đo loa→mic và
   watchdog fallback về notch.
5. **Thứ tự roadmap theo phụ thuộc dữ liệu**, không theo độ hấp dẫn:
   stereo → vòng dữ liệu → depth theo nhu cầu/nhả dần/ring-risk →
   soundcheck đo → classifier → LLM copilot → AFC.
