# Peakiness sweep offline @ FFT-2048 (LANE T, Task T1) — 2026-09-04

`tools/peakiness_sweep.cpp` (target `PeakinessSweep`) chạy tín hiệu tổng hợp qua
`Detector`@2048 + `PeakinessAnalyzer::peakinessAt`, in CSV
`block,freq,peakiness,score,threshold` + bảng phân bố. Bảng đầy đủ +
cách chạy: `docs/peakiness-sweep-2048-findings.md`. Ba bài học:

1. **`analyse()` che mất phân bố dưới ngưỡng.** Nó chỉ trả candidate > threshold,
   nên noise-only ra 0 dòng. Muốn thấy noise floor phải tự tính MAX `peakinessAt`
   qua mọi bin có đủ annulus ±5 (đúng helper `maxPeakiness` trong
   `tests/test_peakiness.cpp`). Đây là điểm spec quan trọng nhất của tool.

2. **Baseline 7.35 là 60 seed lấy MỘT shot/seed — nó under-estimate worst case.**
   Sweep đi liên tục 1 seed qua 216 cửa sổ chồng 75% → noise-floor cross 10.0
   MỘT lần (13.99 @ block 173) rồi rớt ngay về ~6.2. Không phải metric hỏng, là
   extreme-value khi lấy mẫu dày/tương quan. Ý nghĩa: margin của 10.0 mỏng hơn
   con số 7.35 gợi ý dưới vận hành liên tục. Logic age/hold ở NotchController
   (tool offline KHÔNG mô phỏng) mới là thứ dập blip 1-block.

3. **Sine tĩnh KHÔNG phải proxy "nhạc" hợp lệ.** 9 partial tĩnh cách nhau xa hơn
   annulus ±5 → mỗi partial đọc y như tone đơn (metric scale-invariant), cross
   ngưỡng ngay. Test false-positive-headroom thật cần WAV thu thật hoặc partial
   có điều chế biên độ/tần số. Để lại cho T2 (rig + người).

Không đổi hằng số DSP nào — quyết định ngưỡng thuộc T2 với WAV thật. Liên quan:
[[brief-verification-2026-08-21]] (kiểm số học assertion), file
`src/dsp/PeakinessAnalyzer.h` (bảng đo cũ đo trên hình học 1024).
