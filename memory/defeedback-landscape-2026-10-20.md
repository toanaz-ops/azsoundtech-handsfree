# Khảo sát chống hú 2023–2026: lấy gì cho Hands-free (2026-10-20)

Bản đầy đủ: `docs/research/2026-10-20-defeedback-landscape.md`. Sổ quyết định:
`docs/superpowers/decisions/2026-10-20-review-and-research.md`.

## Bài học (để không nghiên cứu lại)

1. **Thứ gì cho nhiều GBF hơn notch thì đều đổi tiếng.** Gồm: Alpha Labs De-Feedback (tách
   giọng bằng mạng nơ-ron, khoảng +10 dB nhưng mất reverb phòng và mất nhạc cụ), dịch tần,
   AFC, tự mute, EQ toàn dải. Với ràng buộc "không méo" của owner, Hands-free chỉ cạnh tranh
   được bằng **độ chính xác và độ hẹp của notch, khả năng phân biệt nhạc với hú, và việc
   minh bạch cho người dùng thấy đã cắt gì**.
2. **Notch đặt ở tâm bin là lỗ hổng thật.** `PeakinessAnalyzer.cpp:144`: ở 300 Hz, notch
   lệch nửa bin chỉ còn cắt −3,9 dB thay vì −18 dB (RBJ, Q 30). Ghi chú "Q 30 rộng hơn một
   bin" chỉ đúng từ khoảng 1 kHz trở lên. Đã chọn sửa trước (P1).
3. **Một phép thử đơn lẻ báo nhầm tới 33 % trên nhạc; kết hợp nhiều phép thử còn 3 %**
   (van Waterschoot & Moonen 2010). App còn thiếu hai phép thử: so đỉnh với các họa âm của
   nó (PHPR) và độ đều của tốc độ tăng tính theo dB.
4. **"REA repo" được hiểu là `rta-tool`**, giấy phép AGPL-3.0. Dùng lại mã cho phép đo pha và
   coherence (soundcheck v2) phải chờ owner xác nhận cấp lại giấy phép.
5. **Proxy của phiên làm việc chặn web ngoài GitHub.** Số liệu chỉ lấy được từ bản tóm tắt
   của công cụ tìm kiếm, nên phải kiểm lại trên toàn văn trước khi đưa vào spec.
6. **"isovoice" không tìm thấy.** Đừng đoán; hỏi owner xin link.
