# P1: tần số notch dưới-bin (2026-10-20)

Nguồn: `docs/research/2026-10-20-defeedback-landscape.md` §7–§8. Quyết định Q5 ghi
trong `docs/superpowers/decisions/2026-10-20-review-and-research.md`.

## Bài học

1. **"Q 30 rộng hơn một bin" chỉ đúng từ khoảng 1 kHz trở lên.** Ở 300 Hz, Q 30
   chỉ rộng 10 Hz trong khi một bin rộng 23,4 Hz. Notch đặt ở tâm bin có thể cắt
   gần như 0 dB đúng chỗ hú. Một lập luận về độ rộng notch phải được kiểm ở **tần
   số thấp nhất** mà detector còn bắt được (117 Hz), không chỉ ở 1 kHz.
2. **Nội suy parabol trên log-biên độ với cửa sổ Hann cho sai số ≤ 0,016 bin**
   trên tone thật. Không cần phase-vocoder cho mục đích đặt notch.
3. **Phải kẹp độ lệch dưới nửa bin (0,45).** Mọi chỗ trong code đổi Hz ngược lại
   thành bin đều dùng `lround(hz/binWidth)`: reinforce, nhớ phòng, soundcheck. Một
   độ lệch đúng ±0,5 sẽ làm chúng nhảy sang bin bên cạnh.
4. **Test cũ vẫn xanh vì phổ tổng hợp đối xứng cho δ = 0 chính xác.** Chỉ test dùng
   tone thật ở 1000 Hz đổi kỳ vọng: từ 1007,8 Hz (tâm bin) sang khoảng 1000 Hz.
